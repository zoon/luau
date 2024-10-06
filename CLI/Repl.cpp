// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#include "Repl.h"

#include "Luau/Common.h"
#include "lua.h"
#include "lualib.h"

#include "Luau/CodeGen.h"
#include "Luau/Compiler.h"
#include "Luau/Parser.h"
#include "Luau/TimeTrace.h"

#include "Coverage.h"
#include "FileUtils.h"
#include "Flags.h"
#include "Profiler.h"
#include "Require.h"

#include "isocline.h"

#include <memory>

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#ifdef __linux__
#include <unistd.h>
#endif

#ifdef CALLGRIND
#include <valgrind/callgrind.h>
#endif

#include <locale.h>
#include <signal.h>
#include <math.h>

LUAU_FASTFLAG(DebugLuauTimeTracing)


constexpr int MaxTraversalLimit = 50;

static bool codegen = false;
static int program_argc = 0;
char** program_argv = nullptr;

// Ctrl-C handling
static void sigintCallback(lua_State* L, int gc)
{
    if (gc >= 0)
        return;

    lua_callbacks(L)->interrupt = NULL;

    lua_rawcheckstack(L, 1); // reserve space for error string
    luaL_error(L, "Execution interrupted");
}

static lua_State* replState = NULL;

#ifdef _WIN32
BOOL WINAPI sigintHandler(DWORD signal)
{
    if (signal == CTRL_C_EVENT && replState)
        lua_callbacks(replState)->interrupt = &sigintCallback;
    return TRUE;
}
#else
static void sigintHandler(int signum)
{
    if (signum == SIGINT && replState)
        lua_callbacks(replState)->interrupt = &sigintCallback;
}
#endif

struct GlobalOptions
{
    int optimizationLevel = 1;
    int debugLevel = 1;
} globalOptions;

static Luau::CompileOptions copts()
{
    Luau::CompileOptions result = {};
    result.optimizationLevel = globalOptions.optimizationLevel;
    result.debugLevel = globalOptions.debugLevel;
    result.typeInfoLevel = 1;
    result.coverageLevel = coverageActive() ? 2 : 0;

    return result;
}

static int lua_loadstring(lua_State* L)
{
    size_t l = 0;
    const char* s = luaL_checklstring(L, 1, &l);
    const char* chunkname = luaL_optstring(L, 2, s);

    lua_setsafeenv(L, LUA_ENVIRONINDEX, false);

    std::string bytecode = Luau::compile(std::string(s, l), copts());
    if (luau_load(L, chunkname, bytecode.data(), bytecode.size(), 0) == 0)
        return 1;

    lua_pushnil(L);
    lua_insert(L, -2); // put before error message
    return 2;          // return nil plus error message
}

static int finishrequire(lua_State* L)
{
    if (lua_isstring(L, -1))
        lua_error(L);

    return 1;
}

static int lua_require(lua_State* L)
{
    std::string name = luaL_checkstring(L, 1);

    RequireResolver::ResolvedRequire resolvedRequire = RequireResolver::resolveRequire(L, std::move(name));

    if (resolvedRequire.status == RequireResolver::ModuleStatus::Cached)
        return finishrequire(L);
    else if (resolvedRequire.status == RequireResolver::ModuleStatus::Ambiguous)
        luaL_errorL(L, "require path could not be resolved to a unique file");
    else if (resolvedRequire.status == RequireResolver::ModuleStatus::NotFound)
        luaL_errorL(L, "error requiring: module not found");

    // module needs to run in a new thread, isolated from the rest
    // note: we create ML on main thread so that it doesn't inherit environment of L
    lua_State* GL = lua_mainthread(L);
    lua_State* ML = lua_newthread(GL);
    lua_xmove(GL, L, 1);

    // new thread needs to have the globals sandboxed
    luaL_sandboxthread(ML);

    // now we can compile & run module on the new thread
    std::string bytecode = Luau::compile(resolvedRequire.sourceCode, copts());
    if (luau_load(ML, resolvedRequire.chunkName.c_str(), bytecode.data(), bytecode.size(), 0) == 0)
    {
        if (codegen)
        {
            Luau::CodeGen::CompilationOptions nativeOptions;
            Luau::CodeGen::compile(ML, -1, nativeOptions);
        }

        if (coverageActive())
            coverageTrack(ML, -1);

        int status = lua_resume(ML, L, 0);

        if (status == 0)
        {
            if (lua_gettop(ML) == 0)
                lua_pushstring(ML, "module must return a value");
            else if (!lua_istable(ML, -1) && !lua_isfunction(ML, -1))
                lua_pushstring(ML, "module must return a table or function");
        }
        else if (status == LUA_YIELD)
        {
            lua_pushstring(ML, "module can not yield");
        }
        else if (!lua_isstring(ML, -1))
        {
            lua_pushstring(ML, "unknown error while running module");
        }
    }

    // there's now a return value on top of ML; L stack: _MODULES ML
    lua_xmove(ML, L, 1);
    lua_pushvalue(L, -1);
    lua_setfield(L, -4, resolvedRequire.absolutePath.c_str());

    // L stack: _MODULES ML result
    return finishrequire(L);
}

static int lua_collectgarbage(lua_State* L)
{
    const char* option = luaL_optstring(L, 1, "collect");

    if (strcmp(option, "collect") == 0)
    {
        lua_gc(L, LUA_GCCOLLECT, 0);
        return 0;
    }

    if (strcmp(option, "restart") == 0) {
        lua_gc(L, LUA_GCRESTART, 0);
        return 0;
    }

    if (strcmp(option, "stop") == 0)
    {
        lua_gc(L, LUA_GCSTOP, 0);
        return 0;
    }

    if (strcmp(option, "count") == 0)
    {
        int c = lua_gc(L, LUA_GCCOUNT, 0);
        lua_pushnumber(L, c);
        return 1;
    }
    if (strcmp(option, "countb") == 0)
    {
        int c = lua_gc(L, LUA_GCCOUNT, 0);
        int b = lua_gc(L, LUA_GCCOUNTB, 0);
        int64_t bytes = ((int64_t)c << 10) + b;
        lua_pushnumber(L, (double)bytes);
        return 1;
    }

    luaL_error(L, "collectgarbage must be called with 'stop', 'restart', 'collect' or 'count[b]'");
}

inline float lerp(float t, float a, float b)
{
    return a + t * (b - a);
}

inline float mag_sq(const float* v)
{
    return v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
}

inline float dot(const float* a, const float*b)
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

inline float clamp(float v, float min, float max)
{
    float r = v < min ? min : v;
    return r > max ? max : r;
}

inline float signf(float v)
{
    return v > 0.0f ? 1.0f : (v < 0.0f ? -1.0f : 0.0f);
}

static int lua_vector_dot(lua_State* L)
{
    const float* a = luaL_checkvector(L, 1);
    const float* b = luaL_checkvector(L, 2);
    lua_pushnumber(L, dot(a, b));
    return 1;
}

static int lua_vector_cross(lua_State* L)
{
    const float* a = luaL_checkvector(L, 1);
    const float* b = luaL_checkvector(L, 2);
    lua_pushvector(L,
        a[1]*b[2] - a[2]*b[1],
        a[2]*b[0] - a[0]*b[2],
        a[0]*b[1] - a[1]*b[0]);
    return 1;
}

static int lua_vector_lerp(lua_State* L)
{
    const float* a = luaL_checkvector(L, 1);
    const float* b = luaL_checkvector(L, 2);
    const float t  = (float)luaL_checknumber(L, 3);
    lua_pushvector(L,
        lerp(t, a[0], b[0]),
        lerp(t, a[1], b[1]),
        lerp(t, a[2], b[2])
    );
    return 1;
}

static int lua_vector_angle(lua_State *L)
{
    const float* a = luaL_checkvector(L, 1);
    const float* b = luaL_checkvector(L, 2);
    const float* axis = lua_tovector(L, 3);
    const float denom = sqrtf(mag_sq(a) * mag_sq(b));
    if (denom < 1e-15) {
        lua_pushnumber(L, 0.0f);
        return 1;
    }
    float angle = acosf(clamp(dot(a, b) / denom, -1.0f, 1.0f));
    float sign = 1.0f;
    if (!!axis)
    {
        float cx = a[1]*b[2] - a[2]*b[1];
        float cy = a[2]*b[0] - a[0]*b[2];
        float cz = a[0]*b[1] - a[1]*b[0];
        sign = signf(axis[0] * cx + axis[1] * cy + axis[2] * cz);
    }
    lua_pushnumber(L, sign * angle);
    return 1;
}

static int lua_vector_eq(lua_State* L)
{
    const float* a = luaL_checkvector(L, 1);
    const float* b = luaL_checkvector(L, 2);
    float eps = (float)luaL_optnumber(L, 3, 1e-5);
    float dx = a[0] - b[0];
    float dy = a[1] - b[1];
    float dz = a[2] - b[2];
    lua_pushboolean(L, dx*dx + dy*dy + dz*dz < eps*eps);
    return 1;
}

static int lua_vector_min(lua_State* L)
{
    const float* a = luaL_checkvector(L, 1);
    const float* b = luaL_checkvector(L, 2);
    lua_pushvector(L, std::min<float>(a[0], b[0]), std::min<float>(a[1], b[1]), std::min<float>(a[2], b[2]));
    return 1;
}

static int lua_vector_max(lua_State* L)
{
    const float* a = luaL_checkvector(L, 1);
    const float* b = luaL_checkvector(L, 2);
    lua_pushvector(L, std::max<float>(a[0], b[0]), std::max<float>(a[1], b[1]), std::max<float>(a[2], b[2]));
    return 1;
}

static int lua_vector_index(lua_State* L)
{
    const float* v = luaL_checkvector(L, 1);
    const char* name = luaL_checkstring(L, 2);

    // properties:
    if (strcmp(name, "Magnitude") == 0)
    {
        lua_pushnumber(L, sqrtf(mag_sq(v)));
        return 1;
    }

    if (strcmp(name, "Unit") == 0)
    {
        const float* v = luaL_checkvector(L, 1);
        const float msq = mag_sq(v);

        if (msq < 1e-8)
        {
            lua_pushvector(L, NAN, NAN, NAN);
            return 1;
        }
        const float scale = 1.0f / sqrtf(msq);
        lua_pushvector(L, scale*v[0], scale*v[1], scale*v[2]);
        return 1;
    }

    // methods:
    if (strcmp(name, "Dot") == 0)
    {
        lua_pushcfunction(L, lua_vector_dot, "Dot");
        return 1;
    }

    if (strcmp(name, "Cross") == 0)
    {
        lua_pushcfunction(L, lua_vector_cross, "Cross");
        return 1;
    }

    if (strcmp(name, "Lerp") == 0)
    {
        lua_pushcfunction(L, lua_vector_lerp, "Lerp");
        return 1;
    }

    if (strcmp(name, "Angle") == 0)
    {
        lua_pushcfunction(L, lua_vector_angle, "Angle");
        return 1;
    }

    if (strcmp(name, "FuzzyEq") == 0)
    {
        lua_pushcfunction(L, lua_vector_eq, "FuzzyEq");
        return 1;
    }

    if (strcmp(name, "Min") == 0)
    {
        lua_pushcfunction(L, lua_vector_min, "Min");
        return 1;
    }

    if (strcmp(name, "Max") == 0)
    {
        lua_pushcfunction(L, lua_vector_max, "Max");
        return 1;
    }

    luaL_error(L, "%s is not a valid member of vector", name);
}

static int lua_vector_namecall(lua_State* L)
{
    if (const char* str = lua_namecallatom(L, nullptr))
    {
        if (strcmp(str, "Dot") == 0)
            return lua_vector_dot(L);

        if (strcmp(str, "Cross") == 0)
            return lua_vector_cross(L);

        if (strcmp(str, "Lerp") == 0)
            return lua_vector_lerp(L);

        if (strcmp(str, "Angle") == 0)
            return lua_vector_angle(L);

        if (strcmp(str, "FuzzyEq") == 0)
            return lua_vector_eq(L);

        if (strcmp(str, "Min") == 0)
            return lua_vector_min(L);

        if (strcmp(str, "Max") == 0)
            return lua_vector_max(L);
    }

    luaL_error(L, "%s is not a valid method of vector", luaL_checkstring(L, 1));
}
#ifdef CALLGRIND
static int lua_callgrind(lua_State* L)
{
    const char* option = luaL_checkstring(L, 1);

    if (strcmp(option, "running") == 0)
    {
        int r = RUNNING_ON_VALGRIND;
        lua_pushboolean(L, r);
        return 1;
    }

    if (strcmp(option, "zero") == 0)
    {
        CALLGRIND_ZERO_STATS;
        return 0;
    }

    if (strcmp(option, "dump") == 0)
    {
        const char* name = luaL_checkstring(L, 2);

        CALLGRIND_DUMP_STATS_AT(name);
        return 0;
    }

    luaL_error(L, "callgrind must be called with one of 'running', 'zero', 'dump'");
}
#endif

void setupState(lua_State* L)
{
    if (codegen)
        Luau::CodeGen::create(L);

    luaL_openlibs(L);

    static const luaL_Reg funcs[] = {
        {"loadstring", lua_loadstring},
        {"require", lua_require},
        {"collectgarbage", lua_collectgarbage},
#ifdef CALLGRIND
        {"callgrind", lua_callgrind},
#endif
        {NULL, NULL},
    };

    lua_pushvalue(L, LUA_GLOBALSINDEX);
    luaL_register(L, NULL, funcs);
    lua_pop(L, 1);

    // add vector
    lua_pushvector(L, 0.0f, 0.0f, 0.0f);
    luaL_newmetatable(L, "vector");
    lua_pushstring(L, "__index");
    lua_pushcfunction(L, lua_vector_index, nullptr);
    lua_settable(L, -3);

    lua_pushstring(L, "__namecall");
    lua_pushcfunction(L, lua_vector_namecall, nullptr);
    lua_settable(L, -3);

    lua_setreadonly(L, -1, true);
    lua_setmetatable(L, -2);
    lua_pop(L, 1);

    luaL_sandbox(L);
}

void setupArguments(lua_State* L, int argc, char** argv)
{
    lua_checkstack(L, argc);

    for (int i = 0; i < argc; ++i)
        lua_pushstring(L, argv[i]);
}

std::string runCode(lua_State* L, const std::string& source)
{
    lua_checkstack(L, LUA_MINSTACK);

    std::string bytecode = Luau::compile(source, copts());

    if (luau_load(L, "=stdin", bytecode.data(), bytecode.size(), 0) != 0)
    {
        size_t len;
        const char* msg = lua_tolstring(L, -1, &len);

        std::string error(msg, len);
        lua_pop(L, 1);

        return error;
    }

    lua_State* T = lua_newthread(L);

    lua_pushvalue(L, -2);
    lua_remove(L, -3);
    lua_xmove(L, T, 1);

    int status = lua_resume(T, NULL, 0);

    if (status == 0)
    {
        int n = lua_gettop(T);

        if (n)
        {
            luaL_checkstack(T, LUA_MINSTACK, "too many results to print");
            lua_getglobal(T, "_PRETTYPRINT");
            // If _PRETTYPRINT is nil, then use the standard print function instead
            if (lua_isnil(T, -1))
            {
                lua_pop(T, 1);
                lua_getglobal(T, "print");
            }
            lua_insert(T, 1);
            lua_pcall(T, n, 0, 0);
        }

        lua_pop(L, 1);
        return std::string();
    }
    else
    {
        std::string error;

        if (status == LUA_YIELD)
        {
            error = "thread yielded unexpectedly";
        }
        else if (const char* str = lua_tostring(T, -1))
        {
            error = str;
        }

        error += "\nstack backtrace:\n";
        error += lua_debugtrace(T);

        lua_pop(L, 1);
        return error;
    }
}

// Replaces the top of the lua stack with the metatable __index for the value
// if it exists.  Returns true iff __index exists.
static bool tryReplaceTopWithIndex(lua_State* L)
{
    if (luaL_getmetafield(L, -1, "__index"))
    {
        // Remove the table leaving __index on the top of stack
        lua_remove(L, -2);
        return true;
    }
    return false;
}


// This function is similar to lua_gettable, but it avoids calling any
// lua callback functions (e.g. __index) which might modify the Lua VM state.
static void safeGetTable(lua_State* L, int tableIndex)
{
    lua_pushvalue(L, tableIndex); // Duplicate the table

    // The loop invariant is that the table to search is at -1
    // and the key is at -2.
    for (int loopCount = 0;; loopCount++)
    {
        lua_pushvalue(L, -2); // Duplicate the key
        lua_rawget(L, -2);    // Try to find the key
        if (!lua_isnil(L, -1) || loopCount >= MaxTraversalLimit)
        {
            // Either the key has been found, and/or we have reached the max traversal limit
            break;
        }
        else
        {
            lua_pop(L, 1); // Pop the nil result
            if (!luaL_getmetafield(L, -1, "__index"))
            {
                lua_pushnil(L);
                break;
            }
            else if (lua_istable(L, -1))
            {
                // Replace the current table being searched with __index table
                lua_replace(L, -2);
            }
            else
            {
                lua_pop(L, 1); // Pop the value
                lua_pushnil(L);
                break;
            }
        }
    }

    lua_remove(L, -2); // Remove the table
    lua_remove(L, -2); // Remove the original key
}

// completePartialMatches finds keys that match the specified 'prefix'
// Note: the table/object to be searched must be on the top of the Lua stack
static void completePartialMatches(
    lua_State* L,
    bool completeOnlyFunctions,
    const std::string& editBuffer,
    std::string_view prefix,
    const AddCompletionCallback& addCompletionCallback
)
{
    for (int i = 0; i < MaxTraversalLimit && lua_istable(L, -1); i++)
    {
        // table, key
        lua_pushnil(L);

        // Loop over all the keys in the current table
        while (lua_next(L, -2) != 0)
        {
            if (lua_type(L, -2) == LUA_TSTRING)
            {
                // table, key, value
                std::string_view key = lua_tostring(L, -2);
                int valueType = lua_type(L, -1);

                // If the last separator was a ':' (i.e. a method call) then only functions should be completed.
                bool requiredValueType = (!completeOnlyFunctions || valueType == LUA_TFUNCTION);

                if (!key.empty() && requiredValueType && Luau::startsWith(key, prefix))
                {
                    std::string completedComponent(key.substr(prefix.size()));
                    std::string completion(editBuffer + completedComponent);
                    if (valueType == LUA_TFUNCTION)
                    {
                        // Add an opening paren for function calls by default.
                        completion += "(";
                    }
                    addCompletionCallback(completion, std::string(key));
                }
            }
            lua_pop(L, 1);
        }

        // Replace the current table being searched with an __index table if one exists
        if (!tryReplaceTopWithIndex(L))
        {
            break;
        }
    }
}

static void completeIndexer(lua_State* L, const std::string& editBuffer, const AddCompletionCallback& addCompletionCallback)
{
    std::string_view lookup = editBuffer;
    bool completeOnlyFunctions = false;

    lua_checkstack(L, LUA_MINSTACK);

    // Push the global variable table to begin the search
    lua_pushvalue(L, LUA_GLOBALSINDEX);

    for (;;)
    {
        size_t sep = lookup.find_first_of(".:");
        std::string_view prefix = lookup.substr(0, sep);

        if (sep == std::string_view::npos)
        {
            completePartialMatches(L, completeOnlyFunctions, editBuffer, prefix, addCompletionCallback);
            break;
        }
        else
        {
            // find the key in the table
            lua_pushlstring(L, prefix.data(), prefix.size());
            safeGetTable(L, -2);
            lua_remove(L, -2);

            if (lua_istable(L, -1) || tryReplaceTopWithIndex(L))
            {
                completeOnlyFunctions = lookup[sep] == ':';
                lookup.remove_prefix(sep + 1);
            }
            else
            {
                // Unable to search for keys, so stop searching
                break;
            }
        }
    }

    lua_pop(L, 1);
}

void getCompletions(lua_State* L, const std::string& editBuffer, const AddCompletionCallback& addCompletionCallback)
{
    completeIndexer(L, editBuffer, addCompletionCallback);
}

static void icGetCompletions(ic_completion_env_t* cenv, const char* editBuffer)
{
    auto* L = reinterpret_cast<lua_State*>(ic_completion_arg(cenv));

    getCompletions(
        L,
        std::string(editBuffer),
        [cenv](const std::string& completion, const std::string& display)
        {
            ic_add_completion_ex(cenv, completion.data(), display.data(), nullptr);
        }
    );
}

static bool isMethodOrFunctionChar(const char* s, long len)
{
    char c = *s;
    return len == 1 && (isalnum(c) || c == '.' || c == ':' || c == '_');
}

static void completeRepl(ic_completion_env_t* cenv, const char* editBuffer)
{
    ic_complete_word(cenv, editBuffer, icGetCompletions, isMethodOrFunctionChar);
}

static void loadHistory(const char* name)
{
    std::string path;

    if (const char* home = getenv("HOME"))
    {
        path = joinPaths(home, name);
    }
    else if (const char* userProfile = getenv("USERPROFILE"))
    {
        path = joinPaths(userProfile, name);
    }

    if (!path.empty())
        ic_set_history(path.c_str(), -1 /* default entries (= 200) */);
}

static void runReplImpl(lua_State* L)
{
    ic_set_default_completer(completeRepl, L);

    // Reset the locale to C
    setlocale(LC_ALL, "C");

    // Make brace matching easier to see
    ic_style_def("ic-bracematch", "teal");

    // Prevent auto insertion of braces
    ic_enable_brace_insertion(false);

    // Loads history from the given file; isocline automatically saves the history on process exit
    loadHistory(".luau_history");

    std::string buffer;

    for (;;)
    {
        const char* prompt = buffer.empty() ? "" : ">";
        std::unique_ptr<char, void (*)(void*)> line(ic_readline(prompt), free);
        if (!line)
            break;

        if (buffer.empty() && runCode(L, std::string("return ") + line.get()) == std::string())
        {
            ic_history_add(line.get());
            continue;
        }

        if (!buffer.empty())
            buffer += "\n";
        buffer += line.get();

        std::string error = runCode(L, buffer);

        if (error.length() >= 5 && error.compare(error.length() - 5, 5, "<eof>") == 0)
        {
            continue;
        }

        if (error.length())
        {
            fprintf(stdout, "%s\n", error.c_str());
        }

        ic_history_add(buffer.c_str());
        buffer.clear();
    }
}

static void runRepl()
{
    std::unique_ptr<lua_State, void (*)(lua_State*)> globalState(luaL_newstate(), lua_close);
    lua_State* L = globalState.get();

    setupState(L);

    // setup Ctrl+C handling
    replState = L;
#ifdef _WIN32
    SetConsoleCtrlHandler(sigintHandler, TRUE);
#else
    signal(SIGINT, sigintHandler);
#endif

    luaL_sandboxthread(L);
    runReplImpl(L);
}

// `repl` is used it indicate if a repl should be started after executing the file.
static bool runFile(const char* name, lua_State* GL, bool repl)
{
    std::optional<std::string> source = readFile(name);
    if (!source)
    {
        fprintf(stderr, "Error opening %s\n", name);
        return false;
    }

    // module needs to run in a new thread, isolated from the rest
    lua_State* L = lua_newthread(GL);

    // new thread needs to have the globals sandboxed
    luaL_sandboxthread(L);

    std::string chunkname = "=" + std::string(name);

    std::string bytecode = Luau::compile(*source, copts());
    int status = 0;

    if (luau_load(L, chunkname.c_str(), bytecode.data(), bytecode.size(), 0) == 0)
    {
        if (codegen)
        {
            Luau::CodeGen::CompilationOptions nativeOptions;
            Luau::CodeGen::compile(L, -1, nativeOptions);
        }

        if (coverageActive())
            coverageTrack(L, -1);

        setupArguments(L, program_argc, program_argv);
        status = lua_resume(L, NULL, program_argc);
    }
    else
    {
        status = LUA_ERRSYNTAX;
    }

    if (status != 0)
    {
        std::string error;

        if (status == LUA_YIELD)
        {
            error = "thread yielded unexpectedly";
        }
        else if (const char* str = lua_tostring(L, -1))
        {
            error = str;
        }

        error += "\nstacktrace:\n";
        error += lua_debugtrace(L);

        fprintf(stderr, "%s", error.c_str());
    }

    if (repl)
    {
        runReplImpl(L);
    }
    lua_pop(GL, 1);
    return status == 0;
}

static void displayHelp(const char* argv0)
{
    printf("Usage: %s [options] [file list] [-a] [arg list]\n", argv0);
    printf("\n");
    printf("When file list is omitted, an interactive REPL is started instead.\n");
    printf("\n");
    printf("Available options:\n");
    printf("  --coverage: collect code coverage while running the code and output results to coverage.out\n");
    printf("  -h, --help: Display this usage message.\n");
    printf("  -i, --interactive: Run an interactive REPL after executing the last script specified.\n");
    printf("  -O<n>: compile with optimization level n (default 1, n should be between 0 and 2).\n");
    printf("  -g<n>: compile with debug level n (default 1, n should be between 0 and 2).\n");
    printf("  --profile[=N]: profile the code using N Hz sampling (default 10000) and output results to profile.out\n");
    printf("  --timetrace: record compiler time tracing information into trace.json\n");
    printf("  --codegen: execute code using native code generation\n");
    printf("  --program-args,-a: declare start of arguments to be passed to the Luau program\n");
}

static int assertionHandler(const char* expr, const char* file, int line, const char* function)
{
    printf("%s(%d): ASSERTION FAILED: %s\n", file, line, expr);
    return 1;
}

int replMain(int argc, char** argv)
{
    Luau::assertHandler() = assertionHandler;

    setLuauFlagsDefault();

#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif

    int profile = 0;
    bool coverage = false;
    bool interactive = false;
    bool codegenPerf = false;
    int program_args = argc;

    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0)
        {
            displayHelp(argv[0]);
            return 0;
        }
        else if (strcmp(argv[i], "-i") == 0 || strcmp(argv[i], "--interactive") == 0)
        {
            interactive = true;
        }
        else if (strncmp(argv[i], "-O", 2) == 0)
        {
            int level = atoi(argv[i] + 2);
            if (level < 0 || level > 2)
            {
                fprintf(stderr, "Error: Optimization level must be between 0 and 2 inclusive.\n");
                return 1;
            }
            globalOptions.optimizationLevel = level;
        }
        else if (strncmp(argv[i], "-g", 2) == 0)
        {
            int level = atoi(argv[i] + 2);
            if (level < 0 || level > 2)
            {
                fprintf(stderr, "Error: Debug level must be between 0 and 2 inclusive.\n");
                return 1;
            }
            globalOptions.debugLevel = level;
        }
        else if (strcmp(argv[i], "--profile") == 0)
        {
            profile = 10000; // default to 10 KHz
        }
        else if (strncmp(argv[i], "--profile=", 10) == 0)
        {
            profile = atoi(argv[i] + 10);
        }
        else if (strcmp(argv[i], "--codegen") == 0)
        {
            codegen = true;
        }
        else if (strcmp(argv[i], "--codegen-perf") == 0)
        {
            codegen = true;
            codegenPerf = true;
        }
        else if (strcmp(argv[i], "--coverage") == 0)
        {
            coverage = true;
        }
        else if (strcmp(argv[i], "--timetrace") == 0)
        {
            FFlag::DebugLuauTimeTracing.value = true;
        }
        else if (strncmp(argv[i], "--fflags=", 9) == 0)
        {
            setLuauFlags(argv[i] + 9);
        }
        else if (strcmp(argv[i], "--program-args") == 0 || strcmp(argv[i], "-a") == 0)
        {
            program_args = i + 1;
            break;
        }
        else if (argv[i][0] == '-')
        {
            fprintf(stderr, "Error: Unrecognized option '%s'.\n\n", argv[i]);
            displayHelp(argv[0]);
            return 1;
        }
    }

    program_argc = argc - program_args;
    program_argv = &argv[program_args];


#if !defined(LUAU_ENABLE_TIME_TRACE)
    if (FFlag::DebugLuauTimeTracing)
    {
        fprintf(stderr, "To run with --timetrace, Luau has to be built with LUAU_ENABLE_TIME_TRACE enabled\n");
        return 1;
    }
#endif

    if (codegenPerf)
    {
#if __linux__
        char path[128];
        snprintf(path, sizeof(path), "/tmp/perf-%d.map", getpid());

        // note, there's no need to close the log explicitly as it will be closed when the process exits
        FILE* codegenPerfLog = fopen(path, "w");

        Luau::CodeGen::setPerfLog(
            codegenPerfLog,
            [](void* context, uintptr_t addr, unsigned size, const char* symbol)
            {
                fprintf(static_cast<FILE*>(context), "%016lx %08x %s\n", long(addr), size, symbol);
            }
        );
#else
        fprintf(stderr, "--codegen-perf option is only supported on Linux\n");
        return 1;
#endif
    }

    if (codegen && !Luau::CodeGen::isSupported())
        fprintf(stderr, "Warning: Native code generation is not supported in current configuration\n");

    const std::vector<std::string> files = getSourceFiles(argc, argv);

    if (files.empty())
    {
        runRepl();
        return 0;
    }
    else
    {
        std::unique_ptr<lua_State, void (*)(lua_State*)> globalState(luaL_newstate(), lua_close);
        lua_State* L = globalState.get();

        setupState(L);

        if (profile)
            profilerStart(L, profile);

        if (coverage)
            coverageInit(L);

        int failed = 0;

        for (size_t i = 0; i < files.size(); ++i)
        {
            bool isLastFile = i == files.size() - 1;
            failed += !runFile(files[i].c_str(), L, interactive && isLastFile);
        }

        if (profile)
        {
            profilerStop();
            profilerDump("profile.out");
        }

        if (coverage)
            coverageDump("coverage.out");

        return failed ? 1 : 0;
    }
}
