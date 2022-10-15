// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#pragma once

#include "Luau/AssemblyBuilderX64.h"

#include "lobject.h"
#include "ltm.h"

// MS x64 ABI reminder:
// Arguments: rcx, rdx, r8, r9 ('overlapped' with xmm0-xmm3)
// Return: rax, xmm0
// Nonvolatile: r12-r15, rdi, rsi, rbx, rbp
// SIMD: only xmm6-xmm15 are non-volatile, all ymm upper parts are volatile

// AMD64 ABI reminder:
// Arguments: rdi, rsi, rdx, rcx, r8, r9 (xmm0-xmm7)
// Return: rax, rdx, xmm0, xmm1
// Nonvolatile: r12-r15, rbx, rbp
// SIMD: all volatile

namespace Luau
{
namespace CodeGen
{

struct NativeState;

// Data that is very common to access is placed in non-volatile registers
constexpr RegisterX64 rState = r15;         // lua_State* L
constexpr RegisterX64 rBase = r14;          // StkId base
constexpr RegisterX64 rNativeContext = r13; // NativeContext* context
constexpr RegisterX64 rConstants = r12;     // TValue* k

// Native code is as stackless as the interpreter, so we can place some data on the stack once and have it accessible at any point
constexpr OperandX64 sClosure = qword[rbp + 0]; // Closure* cl
constexpr OperandX64 sCode = qword[rbp + 8];    // Instruction* code

#if defined(_WIN32)

constexpr RegisterX64 rArg1 = rcx;
constexpr RegisterX64 rArg2 = rdx;
constexpr RegisterX64 rArg2d = edx;
constexpr RegisterX64 rArg3 = r8;
constexpr RegisterX64 rArg4 = r9;
constexpr RegisterX64 rArg5 = noreg;
constexpr RegisterX64 rArg6 = noreg;
constexpr OperandX64 sArg5 = qword[rsp + 32];
constexpr OperandX64 sArg6 = qword[rsp + 40];

#else

constexpr RegisterX64 rArg1 = rdi;
constexpr RegisterX64 rArg2 = rsi;
constexpr RegisterX64 rArg2d = esi;
constexpr RegisterX64 rArg3 = rdx;
constexpr RegisterX64 rArg4 = rcx;
constexpr RegisterX64 rArg5 = r8;
constexpr RegisterX64 rArg6 = r9;
constexpr OperandX64 sArg5 = noreg;
constexpr OperandX64 sArg6 = noreg;

#endif

constexpr unsigned kTValueSizeLog2 = 4;

inline OperandX64 luauReg(int ri)
{
    return xmmword[rBase + ri * sizeof(TValue)];
}

inline OperandX64 luauRegValue(int ri)
{
    return qword[rBase + ri * sizeof(TValue) + offsetof(TValue, value)];
}

inline OperandX64 luauRegTag(int ri)
{
    return dword[rBase + ri * sizeof(TValue) + offsetof(TValue, tt)];
}

inline OperandX64 luauRegValueBoolean(int ri)
{
    return dword[rBase + ri * sizeof(TValue) + offsetof(TValue, value)];
}

inline OperandX64 luauConstant(int ki)
{
    return xmmword[rConstants + ki * sizeof(TValue)];
}

inline OperandX64 luauConstantValue(int ki)
{
    return qword[rConstants + ki * sizeof(TValue) + offsetof(TValue, value)];
}

inline void setLuauReg(AssemblyBuilderX64& build, RegisterX64 tmp, int ri, OperandX64 op)
{
    LUAU_ASSERT(op.cat == CategoryX64::mem);

    build.vmovups(tmp, op);
    build.vmovups(luauReg(ri), tmp);
}

inline void jumpIfTagIs(AssemblyBuilderX64& build, int ri, lua_Type tag, Label& label)
{
    build.cmp(luauRegTag(ri), tag);
    build.jcc(Condition::Equal, label);
}

inline void jumpIfTagIsNot(AssemblyBuilderX64& build, int ri, lua_Type tag, Label& label)
{
    build.cmp(luauRegTag(ri), tag);
    build.jcc(Condition::NotEqual, label);
}

// Note: fallthrough label should be placed after this condition
inline void jumpIfFalsy(AssemblyBuilderX64& build, int ri, Label& target, Label& fallthrough)
{
    jumpIfTagIs(build, ri, LUA_TNIL, target);             // false if nil
    jumpIfTagIsNot(build, ri, LUA_TBOOLEAN, fallthrough); // true if not nil or boolean

    build.cmp(luauRegValueBoolean(ri), 0);
    build.jcc(Condition::Equal, target); // true if boolean value is 'true'
}

// Note: fallthrough label should be placed after this condition
inline void jumpIfTruthy(AssemblyBuilderX64& build, int ri, Label& target, Label& fallthrough)
{
    jumpIfTagIs(build, ri, LUA_TNIL, fallthrough);   // false if nil
    jumpIfTagIsNot(build, ri, LUA_TBOOLEAN, target); // true if not nil or boolean

    build.cmp(luauRegValueBoolean(ri), 0);
    build.jcc(Condition::NotEqual, target); // true if boolean value is 'true'
}

inline void jumpIfMetatablePresent(AssemblyBuilderX64& build, RegisterX64 table, Label& target)
{
    build.cmp(qword[table + offsetof(Table, metatable)], 0);
    build.jcc(Condition::NotEqual, target);
}

inline void jumpIfUnsafeEnv(AssemblyBuilderX64& build, RegisterX64 tmp, Label& label)
{
    build.mov(tmp, sClosure);
    build.mov(tmp, qword[tmp + offsetof(Closure, env)]);
    build.test(byte[tmp + offsetof(Table, safeenv)], 1);
    build.jcc(Condition::Zero, label); // Not a safe environment
}

inline void jumpIfTableIsReadOnly(AssemblyBuilderX64& build, RegisterX64 table, Label& label)
{
    build.cmp(byte[table + offsetof(Table, readonly)], 0);
    build.jcc(Condition::NotEqual, label);
}

void jumpOnNumberCmp(AssemblyBuilderX64& build, RegisterX64 tmp, OperandX64 lhs, OperandX64 rhs, Condition cond, Label& label);
void jumpOnAnyCmpFallback(AssemblyBuilderX64& build, int ra, int rb, Condition cond, Label& label, int pcpos);

void convertNumberToIndexOrJump(AssemblyBuilderX64& build, RegisterX64 tmp, RegisterX64 numd, RegisterX64 numi, int ri, Label& label);

void callArithHelper(AssemblyBuilderX64& build, int ra, int rb, OperandX64 c, int pcpos, TMS tm);
void callLengthHelper(AssemblyBuilderX64& build, int ra, int rb, int pcpos);
void callPrepareForN(AssemblyBuilderX64& build, int limit, int step, int init, int pcpos);
void callGetTable(AssemblyBuilderX64& build, int rb, OperandX64 c, int ra, int pcpos);
void callSetTable(AssemblyBuilderX64& build, int rb, OperandX64 c, int ra, int pcpos);
void callBarrierTable(AssemblyBuilderX64& build, RegisterX64 tmp, RegisterX64 table, int ra, Label& skip);

void emitExit(AssemblyBuilderX64& build, bool continueInVm);
void emitUpdateBase(AssemblyBuilderX64& build);
void emitSetSavedPc(AssemblyBuilderX64& build, int pcpos); // Note: only uses rax/rdx, the caller may use other registers
void emitInterrupt(AssemblyBuilderX64& build, int pcpos);
void emitFallback(AssemblyBuilderX64& build, NativeState& data, int op, int pcpos);

} // namespace CodeGen
} // namespace Luau
