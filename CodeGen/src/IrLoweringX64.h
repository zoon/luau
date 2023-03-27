// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#pragma once

#include "Luau/AssemblyBuilderX64.h"
#include "Luau/IrData.h"

#include "IrRegAllocX64.h"

#include <vector>

struct Proto;

namespace Luau
{
namespace CodeGen
{

struct ModuleHelpers;
struct NativeState;
struct AssemblyOptions;

namespace X64
{

struct IrLoweringX64
{
    IrLoweringX64(AssemblyBuilderX64& build, ModuleHelpers& helpers, NativeState& data, IrFunction& function);

    void lowerInst(IrInst& inst, uint32_t index, IrBlock& next);

    bool isFallthroughBlock(IrBlock target, IrBlock next);
    void jumpOrFallthrough(IrBlock& target, IrBlock& next);

    // Operand data lookup helpers
    OperandX64 memRegDoubleOp(IrOp op) const;
    OperandX64 memRegTagOp(IrOp op) const;
    RegisterX64 regOp(IrOp op) const;

    IrConst constOp(IrOp op) const;
    uint8_t tagOp(IrOp op) const;
    bool boolOp(IrOp op) const;
    int intOp(IrOp op) const;
    unsigned uintOp(IrOp op) const;
    double doubleOp(IrOp op) const;

    IrBlock& blockOp(IrOp op) const;
    Label& labelOp(IrOp op) const;

    AssemblyBuilderX64& build;
    ModuleHelpers& helpers;
    NativeState& data;

    IrFunction& function;

    IrRegAllocX64 regs;
};

} // namespace X64
} // namespace CodeGen
} // namespace Luau
