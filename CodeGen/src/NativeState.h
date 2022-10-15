// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#pragma once

#include "Luau/Bytecode.h"
#include "Luau/CodeAllocator.h"

#include <memory>

#include <stdint.h>

#include "lobject.h"
#include "ltm.h"

typedef int (*luau_FastFunction)(lua_State* L, StkId res, TValue* arg0, int nresults, StkId args, int nparams);

namespace Luau
{
namespace CodeGen
{

class UnwindBuilder;

using FallbackFn = const Instruction*(lua_State* L, const Instruction* pc, StkId base, TValue* k);

constexpr uint8_t kFallbackUpdatePc = 1 << 0;
constexpr uint8_t kFallbackUpdateCi = 1 << 1;
constexpr uint8_t kFallbackCheckInterrupt = 1 << 2;

struct NativeFallback
{
    FallbackFn* fallback;
    uint8_t flags;
};

struct NativeProto
{
    uintptr_t* instTargets = nullptr;

    Proto* proto = nullptr;
    uint32_t location = 0;
};

struct NativeContext
{
    // Gateway (C => native transition) entry & exit, compiled at runtime
    uint8_t* gateEntry = nullptr;
    uint8_t* gateExit = nullptr;

    // Opcode fallbacks, implemented in C
    NativeFallback fallback[LOP__COUNT] = {};

    // Fast call methods, implemented in C
    luau_FastFunction luauF_table[256] = {};

    // Helper functions, implemented in C
    int (*luaV_lessthan)(lua_State* L, const TValue* l, const TValue* r) = nullptr;
    int (*luaV_lessequal)(lua_State* L, const TValue* l, const TValue* r) = nullptr;
    int (*luaV_equalval)(lua_State* L, const TValue* t1, const TValue* t2) = nullptr;
    void (*luaV_doarith)(lua_State* L, StkId ra, const TValue* rb, const TValue* rc, TMS op) = nullptr;
    void (*luaV_dolen)(lua_State* L, StkId ra, const TValue* rb) = nullptr;
    void (*luaV_prepareFORN)(lua_State* L, StkId plimit, StkId pstep, StkId pinit) = nullptr;
    void (*luaV_gettable)(lua_State* L, const TValue* t, TValue* key, StkId val) = nullptr;
    void (*luaV_settable)(lua_State* L, const TValue* t, TValue* key, StkId val) = nullptr;

    int (*luaH_getn)(Table* t) = nullptr;

    void (*luaC_barriertable)(lua_State* L, Table* t, GCObject* v) = nullptr;

    double (*libm_pow)(double, double) = nullptr;
};

struct NativeState
{
    NativeState();
    ~NativeState();

    CodeAllocator codeAllocator;
    std::unique_ptr<UnwindBuilder> unwindBuilder;

    // For annotations in assembly text generation
    const char* names[LOP__COUNT] = {};

    uint8_t* gateData = nullptr;
    size_t gateDataSize = 0;

    NativeContext context;
};

void initFallbackTable(NativeState& data);
void initHelperFunctions(NativeState& data);
void initInstructionNames(NativeState& data);

} // namespace CodeGen
} // namespace Luau
