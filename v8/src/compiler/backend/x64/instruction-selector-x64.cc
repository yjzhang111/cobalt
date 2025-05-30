// Copyright 2014 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <algorithm>

#include "src/base/iterator.h"
#include "src/base/logging.h"
#include "src/base/overflowing-math.h"
#include "src/codegen/cpu-features.h"
#include "src/codegen/machine-type.h"
#include "src/compiler/backend/instruction-codes.h"
#include "src/compiler/backend/instruction-selector-impl.h"
#include "src/compiler/backend/instruction.h"
#include "src/compiler/machine-operator.h"
#include "src/compiler/node-matchers.h"
#include "src/compiler/node-properties.h"
#include "src/compiler/opcodes.h"
#include "src/roots/roots-inl.h"

#if V8_ENABLE_WEBASSEMBLY
#include "src/wasm/simd-shuffle.h"
#endif  // V8_ENABLE_WEBASSEMBLY

namespace v8 {
namespace internal {
namespace compiler {

namespace {

bool IsCompressed(Node* const node) {
  if (node == nullptr) return false;
  const IrOpcode::Value opcode = node->opcode();
  if (opcode == IrOpcode::kLoad || opcode == IrOpcode::kProtectedLoad ||
      opcode == IrOpcode::kLoadTrapOnNull ||
      opcode == IrOpcode::kUnalignedLoad ||
      opcode == IrOpcode::kLoadImmutable) {
    LoadRepresentation load_rep = LoadRepresentationOf(node->op());
    return load_rep.IsCompressed();
  } else if (node->opcode() == IrOpcode::kPhi) {
    MachineRepresentation phi_rep = PhiRepresentationOf(node->op());
    return phi_rep == MachineRepresentation::kCompressed ||
           phi_rep == MachineRepresentation::kCompressedPointer;
  }
  return false;
}

}  // namespace

// Adds X64-specific methods for generating operands.
class X64OperandGenerator final : public OperandGenerator {
 public:
  explicit X64OperandGenerator(InstructionSelector* selector)
      : OperandGenerator(selector) {}

  bool CanBeImmediate(Node* node) {
    switch (node->opcode()) {
      case IrOpcode::kCompressedHeapConstant: {
        if (!COMPRESS_POINTERS_BOOL) return false;
        // For builtin code we need static roots
        if (selector()->isolate()->bootstrapper() && !V8_STATIC_ROOTS_BOOL) {
          return false;
        }
        const RootsTable& roots_table = selector()->isolate()->roots_table();
        RootIndex root_index;
        CompressedHeapObjectMatcher m(node);
        if (m.HasResolvedValue() &&
            roots_table.IsRootHandle(m.ResolvedValue(), &root_index)) {
          return RootsTable::IsReadOnly(root_index);
        }
        return false;
      }
      case IrOpcode::kInt32Constant:
      case IrOpcode::kRelocatableInt32Constant: {
        const int32_t value = OpParameter<int32_t>(node->op());
        // int32_t min will overflow if displacement mode is
        // kNegativeDisplacement.
        return value != std::numeric_limits<int32_t>::min();
      }
      case IrOpcode::kInt64Constant: {
        const int64_t value = OpParameter<int64_t>(node->op());
        return std::numeric_limits<int32_t>::min() < value &&
               value <= std::numeric_limits<int32_t>::max();
      }
      case IrOpcode::kNumberConstant: {
        const double value = OpParameter<double>(node->op());
        return base::bit_cast<int64_t>(value) == 0;
      }
      default:
        return false;
    }
  }

  int32_t GetImmediateIntegerValue(Node* node) {
    DCHECK(CanBeImmediate(node));
    if (node->opcode() == IrOpcode::kInt32Constant) {
      return OpParameter<int32_t>(node->op());
    }
    DCHECK_EQ(IrOpcode::kInt64Constant, node->opcode());
    return static_cast<int32_t>(OpParameter<int64_t>(node->op()));
  }

  bool CanBeMemoryOperand(InstructionCode opcode, Node* node, Node* input,
                          int effect_level) {
    if ((input->opcode() != IrOpcode::kLoad &&
         input->opcode() != IrOpcode::kLoadImmutable) ||
        !selector()->CanCover(node, input)) {
      return false;
    }
    if (effect_level != selector()->GetEffectLevel(input)) {
      return false;
    }
    MachineRepresentation rep =
        LoadRepresentationOf(input->op()).representation();
    switch (opcode) {
      case kX64And:
      case kX64Or:
      case kX64Xor:
      case kX64Add:
      case kX64Sub:
      case kX64Push:
      case kX64Cmp:
      case kX64Test:
        // When pointer compression is enabled 64-bit memory operands can't be
        // used for tagged values.
        return rep == MachineRepresentation::kWord64 ||
               (!COMPRESS_POINTERS_BOOL && IsAnyTagged(rep));
      case kX64And32:
      case kX64Or32:
      case kX64Xor32:
      case kX64Add32:
      case kX64Sub32:
      case kX64Cmp32:
      case kX64Test32:
        // When pointer compression is enabled 32-bit memory operands can be
        // used for tagged values.
        return rep == MachineRepresentation::kWord32 ||
               (COMPRESS_POINTERS_BOOL &&
                (IsAnyTagged(rep) || IsAnyCompressed(rep)));
      case kAVXFloat64Add:
      case kAVXFloat64Sub:
      case kAVXFloat64Mul:
        DCHECK_EQ(MachineRepresentation::kFloat64, rep);
        return true;
      case kAVXFloat32Add:
      case kAVXFloat32Sub:
      case kAVXFloat32Mul:
        DCHECK_EQ(MachineRepresentation::kFloat32, rep);
        return true;
      case kX64Cmp16:
      case kX64Test16:
        return rep == MachineRepresentation::kWord16;
      case kX64Cmp8:
      case kX64Test8:
        return rep == MachineRepresentation::kWord8;
      default:
        break;
    }
    return false;
  }

  AddressingMode GenerateMemoryOperandInputs(
      Node* index, int scale_exponent, Node* base, Node* displacement,
      DisplacementMode displacement_mode, InstructionOperand inputs[],
      size_t* input_count,
      RegisterUseKind reg_kind = RegisterUseKind::kUseRegister) {
    AddressingMode mode = kMode_MRI;
    bool fold_base_into_displacement = false;
    int64_t fold_value = 0;
    if (base != nullptr && (index != nullptr || displacement != nullptr)) {
      if (index != nullptr && displacement != nullptr &&
          (base->opcode() == IrOpcode::kInt32Constant ||
           base->opcode() == IrOpcode::kInt64Constant) &&
          CanBeImmediate(base) &&
          (displacement->opcode() == IrOpcode::kInt32Constant ||
           displacement->opcode() == IrOpcode::kInt64Constant) &&
          CanBeImmediate(displacement)) {
        fold_value = GetImmediateIntegerValue(base);
        int64_t displacement_val = GetImmediateIntegerValue(displacement);
        if (displacement_mode == kNegativeDisplacement) {
          fold_value -= displacement_val;
        } else {
          fold_value += displacement_val;
        }
        if (fold_value == 0) {
          base = nullptr;
          displacement = nullptr;
        } else if (std::numeric_limits<int32_t>::min() < fold_value &&
                   fold_value <= std::numeric_limits<int32_t>::max()) {
          base = nullptr;
          fold_base_into_displacement = true;
        }
      } else if (base->opcode() == IrOpcode::kInt32Constant &&
                 OpParameter<int32_t>(base->op()) == 0) {
        base = nullptr;
      } else if (base->opcode() == IrOpcode::kInt64Constant &&
                 OpParameter<int64_t>(base->op()) == 0) {
        base = nullptr;
      }
    }
    if (base != nullptr) {
      inputs[(*input_count)++] = UseRegister(base, reg_kind);
      if (index != nullptr) {
        DCHECK(scale_exponent >= 0 && scale_exponent <= 3);
        inputs[(*input_count)++] = UseRegister(index, reg_kind);
        if (displacement != nullptr) {
          inputs[(*input_count)++] = displacement_mode == kNegativeDisplacement
                                         ? UseNegatedImmediate(displacement)
                                         : UseImmediate(displacement);
          static const AddressingMode kMRnI_modes[] = {kMode_MR1I, kMode_MR2I,
                                                       kMode_MR4I, kMode_MR8I};
          mode = kMRnI_modes[scale_exponent];
        } else {
          static const AddressingMode kMRn_modes[] = {kMode_MR1, kMode_MR2,
                                                      kMode_MR4, kMode_MR8};
          mode = kMRn_modes[scale_exponent];
        }
      } else {
        if (displacement == nullptr) {
          mode = kMode_MR;
        } else {
          inputs[(*input_count)++] = displacement_mode == kNegativeDisplacement
                                         ? UseNegatedImmediate(displacement)
                                         : UseImmediate(displacement);
          mode = kMode_MRI;
        }
      }
    } else {
      DCHECK(scale_exponent >= 0 && scale_exponent <= 3);
      if (fold_base_into_displacement) {
        DCHECK(base == nullptr);
        DCHECK(index != nullptr);
        DCHECK(displacement != nullptr);
        inputs[(*input_count)++] = UseRegister(index, reg_kind);
        inputs[(*input_count)++] = UseImmediate(static_cast<int>(fold_value));
        static const AddressingMode kMnI_modes[] = {kMode_MRI, kMode_M2I,
                                                    kMode_M4I, kMode_M8I};
        mode = kMnI_modes[scale_exponent];
      } else if (displacement != nullptr) {
        if (index == nullptr) {
          inputs[(*input_count)++] = UseRegister(displacement, reg_kind);
          mode = kMode_MR;
        } else {
          inputs[(*input_count)++] = UseRegister(index, reg_kind);
          inputs[(*input_count)++] = displacement_mode == kNegativeDisplacement
                                         ? UseNegatedImmediate(displacement)
                                         : UseImmediate(displacement);
          static const AddressingMode kMnI_modes[] = {kMode_MRI, kMode_M2I,
                                                      kMode_M4I, kMode_M8I};
          mode = kMnI_modes[scale_exponent];
        }
      } else {
        inputs[(*input_count)++] = UseRegister(index, reg_kind);
        static const AddressingMode kMn_modes[] = {kMode_MR, kMode_MR1,
                                                   kMode_M4, kMode_M8};
        mode = kMn_modes[scale_exponent];
        if (mode == kMode_MR1) {
          // [%r1 + %r1*1] has a smaller encoding than [%r1*2+0]
          inputs[(*input_count)++] = UseRegister(index, reg_kind);
        }
      }
    }
    return mode;
  }

  AddressingMode GetEffectiveAddressMemoryOperand(
      Node* operand, InstructionOperand inputs[], size_t* input_count,
      RegisterUseKind reg_kind = RegisterUseKind::kUseRegister) {
    {
      LoadMatcher<ExternalReferenceMatcher> m(operand);
      if (m.index().HasResolvedValue() && m.object().HasResolvedValue() &&
          selector()->CanAddressRelativeToRootsRegister(
              m.object().ResolvedValue())) {
        ptrdiff_t const delta =
            m.index().ResolvedValue() +
            MacroAssemblerBase::RootRegisterOffsetForExternalReference(
                selector()->isolate(), m.object().ResolvedValue());
        if (is_int32(delta)) {
          inputs[(*input_count)++] = TempImmediate(static_cast<int32_t>(delta));
          return kMode_Root;
        }
      }
    }
    BaseWithIndexAndDisplacement64Matcher m(operand, AddressOption::kAllowAll);
    DCHECK(m.matches());
    // Decompress pointer by complex addressing mode.
    if (IsCompressed(m.base())) {
      DCHECK(m.index() == nullptr);
      DCHECK(m.displacement() == nullptr || CanBeImmediate(m.displacement()));
      AddressingMode mode = kMode_MCR;
      inputs[(*input_count)++] = UseRegister(m.base(), reg_kind);
      if (m.displacement() != nullptr) {
        inputs[(*input_count)++] =
            m.displacement_mode() == kNegativeDisplacement
                ? UseNegatedImmediate(m.displacement())
                : UseImmediate(m.displacement());
        mode = kMode_MCRI;
      }
      return mode;
    }
    if (m.base() != nullptr &&
        m.base()->opcode() == IrOpcode::kLoadRootRegister) {
      DCHECK_EQ(m.index(), nullptr);
      DCHECK_EQ(m.scale(), 0);
      inputs[(*input_count)++] = UseImmediate(m.displacement());
      return kMode_Root;
    } else if (m.displacement() == nullptr ||
               CanBeImmediate(m.displacement())) {
      return GenerateMemoryOperandInputs(
          m.index(), m.scale(), m.base(), m.displacement(),
          m.displacement_mode(), inputs, input_count, reg_kind);
    } else if (m.base() == nullptr &&
               m.displacement_mode() == kPositiveDisplacement) {
      // The displacement cannot be an immediate, but we can use the
      // displacement as base instead and still benefit from addressing
      // modes for the scale.
      return GenerateMemoryOperandInputs(m.index(), m.scale(), m.displacement(),
                                         nullptr, m.displacement_mode(), inputs,
                                         input_count, reg_kind);
    } else {
      inputs[(*input_count)++] = UseRegister(operand->InputAt(0), reg_kind);
      inputs[(*input_count)++] = UseRegister(operand->InputAt(1), reg_kind);
      return kMode_MR1;
    }
  }

  InstructionOperand GetEffectiveIndexOperand(Node* index,
                                              AddressingMode* mode) {
    if (CanBeImmediate(index)) {
      *mode = kMode_MRI;
      return UseImmediate(index);
    } else {
      *mode = kMode_MR1;
      return UseUniqueRegister(index);
    }
  }

  bool CanBeBetterLeftOperand(Node* node) const {
    return !selector()->IsLive(node);
  }
};

namespace {

ArchOpcode GetLoadOpcode(LoadRepresentation load_rep) {
  ArchOpcode opcode;
  switch (load_rep.representation()) {
    case MachineRepresentation::kFloat32:
      opcode = kX64Movss;
      break;
    case MachineRepresentation::kFloat64:
      opcode = kX64Movsd;
      break;
    case MachineRepresentation::kBit:  // Fall through.
    case MachineRepresentation::kWord8:
      opcode = load_rep.IsSigned() ? kX64Movsxbl : kX64Movzxbl;
      break;
    case MachineRepresentation::kWord16:
      opcode = load_rep.IsSigned() ? kX64Movsxwl : kX64Movzxwl;
      break;
    case MachineRepresentation::kWord32:
      opcode = kX64Movl;
      break;
    case MachineRepresentation::kCompressedPointer:  // Fall through.
    case MachineRepresentation::kCompressed:
#ifdef V8_COMPRESS_POINTERS
      opcode = kX64Movl;
      break;
#else
      UNREACHABLE();
#endif
#ifdef V8_COMPRESS_POINTERS
    case MachineRepresentation::kTaggedSigned:
      opcode = kX64MovqDecompressTaggedSigned;
      break;
    case MachineRepresentation::kTaggedPointer:
    case MachineRepresentation::kTagged:
      opcode = kX64MovqDecompressTagged;
      break;
#else
    case MachineRepresentation::kTaggedSigned:   // Fall through.
    case MachineRepresentation::kTaggedPointer:  // Fall through.
    case MachineRepresentation::kTagged:         // Fall through.
#endif
    case MachineRepresentation::kWord64:
      opcode = kX64Movq;
      break;
    case MachineRepresentation::kSandboxedPointer:
      opcode = kX64MovqDecodeSandboxedPointer;
      break;
    case MachineRepresentation::kSimd128:
      opcode = kX64Movdqu;
      break;
    case MachineRepresentation::kSimd256:  // Fall through.
      opcode = kX64Movdqu256;
      break;
    case MachineRepresentation::kNone:     // Fall through.
    case MachineRepresentation::kMapWord:  // Fall through.
      UNREACHABLE();
  }
  return opcode;
}

ArchOpcode GetStoreOpcode(StoreRepresentation store_rep) {
  switch (store_rep.representation()) {
    case MachineRepresentation::kFloat32:
      return kX64Movss;
    case MachineRepresentation::kFloat64:
      return kX64Movsd;
    case MachineRepresentation::kBit:  // Fall through.
    case MachineRepresentation::kWord8:
      return kX64Movb;
    case MachineRepresentation::kWord16:
      return kX64Movw;
    case MachineRepresentation::kWord32:
      return kX64Movl;
    case MachineRepresentation::kCompressedPointer:  // Fall through.
    case MachineRepresentation::kCompressed:
#ifdef V8_COMPRESS_POINTERS
      return kX64MovqCompressTagged;
#else
      UNREACHABLE();
#endif
    case MachineRepresentation::kTaggedSigned:   // Fall through.
    case MachineRepresentation::kTaggedPointer:  // Fall through.
    case MachineRepresentation::kTagged:
      return kX64MovqCompressTagged;
    case MachineRepresentation::kWord64:
      return kX64Movq;
    case MachineRepresentation::kSandboxedPointer:
      return kX64MovqEncodeSandboxedPointer;
    case MachineRepresentation::kSimd128:
      return kX64Movdqu;
    case MachineRepresentation::kSimd256:  // Fall through.
      return kX64Movdqu256;
    case MachineRepresentation::kNone:     // Fall through.
    case MachineRepresentation::kMapWord:  // Fall through.
      UNREACHABLE();
  }
  UNREACHABLE();
}

ArchOpcode GetSeqCstStoreOpcode(StoreRepresentation store_rep) {
  switch (store_rep.representation()) {
    case MachineRepresentation::kWord8:
      return kAtomicStoreWord8;
    case MachineRepresentation::kWord16:
      return kAtomicStoreWord16;
    case MachineRepresentation::kWord32:
      return kAtomicStoreWord32;
    case MachineRepresentation::kWord64:
      return kX64Word64AtomicStoreWord64;
    case MachineRepresentation::kTaggedSigned:   // Fall through.
    case MachineRepresentation::kTaggedPointer:  // Fall through.
    case MachineRepresentation::kTagged:
      if (COMPRESS_POINTERS_BOOL) return kAtomicStoreWord32;
      return kX64Word64AtomicStoreWord64;
    case MachineRepresentation::kCompressedPointer:  // Fall through.
    case MachineRepresentation::kCompressed:
      CHECK(COMPRESS_POINTERS_BOOL);
      return kAtomicStoreWord32;
    default:
      UNREACHABLE();
  }
}

}  // namespace

void InstructionSelector::VisitTraceInstruction(Node* node) {
  X64OperandGenerator g(this);
  uint32_t markid = OpParameter<uint32_t>(node->op());
  Emit(kX64TraceInstruction, g.Use(node), g.UseImmediate(markid));
}

void InstructionSelector::VisitStackSlot(Node* node) {
  StackSlotRepresentation rep = StackSlotRepresentationOf(node->op());
  int slot = frame_->AllocateSpillSlot(rep.size(), rep.alignment());
  OperandGenerator g(this);

  Emit(kArchStackSlot, g.DefineAsRegister(node),
       sequence()->AddImmediate(Constant(slot)), 0, nullptr);
}

void InstructionSelector::VisitAbortCSADcheck(Node* node) {
  X64OperandGenerator g(this);
  Emit(kArchAbortCSADcheck, g.NoOutput(), g.UseFixed(node->InputAt(0), rdx));
}

void InstructionSelector::VisitLoadLane(Node* node) {
  LoadLaneParameters params = LoadLaneParametersOf(node->op());
  InstructionCode opcode = kArchNop;
  if (params.rep == MachineType::Int8()) {
    opcode = kX64Pinsrb;
  } else if (params.rep == MachineType::Int16()) {
    opcode = kX64Pinsrw;
  } else if (params.rep == MachineType::Int32()) {
    opcode = kX64Pinsrd;
  } else if (params.rep == MachineType::Int64()) {
    opcode = kX64Pinsrq;
  } else {
    UNREACHABLE();
  }

  X64OperandGenerator g(this);
  InstructionOperand outputs[] = {g.DefineAsRegister(node)};
  // Input 0 is value node, 1 is lane idx, and GetEffectiveAddressMemoryOperand
  // uses up to 3 inputs. This ordering is consistent with other operations that
  // use the same opcode.
  InstructionOperand inputs[5];
  size_t input_count = 0;

  inputs[input_count++] = g.UseRegister(node->InputAt(2));
  inputs[input_count++] = g.UseImmediate(params.laneidx);

  AddressingMode mode =
      g.GetEffectiveAddressMemoryOperand(node, inputs, &input_count);
  opcode |= AddressingModeField::encode(mode);

  DCHECK_GE(5, input_count);

  // x64 supports unaligned loads.
  DCHECK_NE(params.kind, MemoryAccessKind::kUnaligned);
  if (params.kind == MemoryAccessKind::kProtected) {
    opcode |= AccessModeField::encode(kMemoryAccessProtectedMemOutOfBounds);
  }
  Emit(opcode, 1, outputs, input_count, inputs);
}

void InstructionSelector::VisitLoadTransform(Node* node) {
  LoadTransformParameters params = LoadTransformParametersOf(node->op());
  ArchOpcode opcode;
  switch (params.transformation) {
    case LoadTransformation::kS128Load8Splat:
      opcode = kX64S128Load8Splat;
      break;
    case LoadTransformation::kS128Load16Splat:
      opcode = kX64S128Load16Splat;
      break;
    case LoadTransformation::kS128Load32Splat:
      opcode = kX64S128Load32Splat;
      break;
    case LoadTransformation::kS128Load64Splat:
      opcode = kX64S128Load64Splat;
      break;
    case LoadTransformation::kS128Load8x8S:
      opcode = kX64S128Load8x8S;
      break;
    case LoadTransformation::kS128Load8x8U:
      opcode = kX64S128Load8x8U;
      break;
    case LoadTransformation::kS128Load16x4S:
      opcode = kX64S128Load16x4S;
      break;
    case LoadTransformation::kS128Load16x4U:
      opcode = kX64S128Load16x4U;
      break;
    case LoadTransformation::kS128Load32x2S:
      opcode = kX64S128Load32x2S;
      break;
    case LoadTransformation::kS128Load32x2U:
      opcode = kX64S128Load32x2U;
      break;
    case LoadTransformation::kS128Load32Zero:
      opcode = kX64Movss;
      break;
    case LoadTransformation::kS128Load64Zero:
      opcode = kX64Movsd;
      break;
    // Simd256
    case LoadTransformation::kS256Load32Splat:
      opcode = kX64S256Load32Splat;
      break;
    case LoadTransformation::kS256Load64Splat:
      opcode = kX64S256Load64Splat;
      break;
    default:
      UNREACHABLE();
  }
  // x64 supports unaligned loads
  DCHECK_NE(params.kind, MemoryAccessKind::kUnaligned);
  InstructionCode code = opcode;
  if (params.kind == MemoryAccessKind::kProtected) {
    code |= AccessModeField::encode(kMemoryAccessProtectedMemOutOfBounds);
  }
  VisitLoad(node, node, code);
}

void InstructionSelector::VisitLoad(Node* node, Node* value,
                                    InstructionCode opcode) {
  X64OperandGenerator g(this);
#ifdef V8_IS_TSAN
  // On TSAN builds we require one scratch register. Because of this we also
  // have to modify the inputs to take into account possible aliasing and use
  // UseUniqueRegister which is not required for non-TSAN builds.
  InstructionOperand temps[] = {g.TempRegister()};
  size_t temp_count = arraysize(temps);
  auto reg_kind = OperandGenerator::RegisterUseKind::kUseUniqueRegister;
#else
  InstructionOperand* temps = nullptr;
  size_t temp_count = 0;
  auto reg_kind = OperandGenerator::RegisterUseKind::kUseRegister;
#endif  // V8_IS_TSAN
  InstructionOperand outputs[] = {g.DefineAsRegister(node)};
  InstructionOperand inputs[3];
  size_t input_count = 0;
  AddressingMode mode =
      g.GetEffectiveAddressMemoryOperand(value, inputs, &input_count, reg_kind);
  InstructionCode code = opcode | AddressingModeField::encode(mode);
  if (node->opcode() == IrOpcode::kProtectedLoad ||
      ((node->opcode() == IrOpcode::kWord32AtomicLoad ||
        node->opcode() == IrOpcode::kWord64AtomicLoad) &&
       (AtomicLoadParametersOf(node->op()).kind() ==
        MemoryAccessKind::kProtected))) {
    code |= AccessModeField::encode(kMemoryAccessProtectedMemOutOfBounds);
  } else if (node->opcode() == IrOpcode::kLoadTrapOnNull) {
    code |= AccessModeField::encode(kMemoryAccessProtectedNullDereference);
  }
  Emit(code, 1, outputs, input_count, inputs, temp_count, temps);
}

void InstructionSelector::VisitLoad(Node* node) {
  LoadRepresentation load_rep = LoadRepresentationOf(node->op());
  DCHECK(!load_rep.IsMapWord());
  VisitLoad(node, node, GetLoadOpcode(load_rep));
}

void InstructionSelector::VisitProtectedLoad(Node* node) { VisitLoad(node); }

namespace {

// Shared routine for Word32/Word64 Atomic Exchange
void VisitAtomicExchange(InstructionSelector* selector, Node* node,
                         ArchOpcode opcode, AtomicWidth width,
                         MemoryAccessKind access_kind) {
  X64OperandGenerator g(selector);
  Node* base = node->InputAt(0);
  Node* index = node->InputAt(1);
  Node* value = node->InputAt(2);
  AddressingMode addressing_mode;
  InstructionOperand inputs[] = {
      g.UseUniqueRegister(value), g.UseUniqueRegister(base),
      g.GetEffectiveIndexOperand(index, &addressing_mode)};
  InstructionOperand outputs[] = {g.DefineSameAsFirst(node)};
  InstructionCode code = opcode | AddressingModeField::encode(addressing_mode) |
                         AtomicWidthField::encode(width);
  if (access_kind == MemoryAccessKind::kProtected) {
    code |= AccessModeField::encode(kMemoryAccessProtectedMemOutOfBounds);
  }
  selector->Emit(code, arraysize(outputs), outputs, arraysize(inputs), inputs);
}

void VisitStoreCommon(InstructionSelector* selector, Node* node,
                      StoreRepresentation store_rep,
                      base::Optional<AtomicMemoryOrder> atomic_order,
                      MemoryAccessKind acs_kind = MemoryAccessKind::kNormal) {
  X64OperandGenerator g(selector);
  Node* base = node->InputAt(0);
  Node* index = node->InputAt(1);
  Node* value = node->InputAt(2);

  DCHECK_NE(store_rep.representation(), MachineRepresentation::kMapWord);
  WriteBarrierKind write_barrier_kind = store_rep.write_barrier_kind();
  const bool is_seqcst =
      atomic_order && *atomic_order == AtomicMemoryOrder::kSeqCst;

  if (v8_flags.enable_unconditional_write_barriers &&
      CanBeTaggedOrCompressedPointer(store_rep.representation())) {
    write_barrier_kind = kFullWriteBarrier;
  }

  const auto access_mode =
      acs_kind == MemoryAccessKind::kProtected
          ? (node->opcode() == IrOpcode::kStoreTrapOnNull
                 ? kMemoryAccessProtectedNullDereference
                 : MemoryAccessMode::kMemoryAccessProtectedMemOutOfBounds)
          : MemoryAccessMode::kMemoryAccessDirect;

  if (write_barrier_kind != kNoWriteBarrier &&
      !v8_flags.disable_write_barriers) {
    DCHECK(CanBeTaggedOrCompressedPointer(store_rep.representation()));
    AddressingMode addressing_mode;
    InstructionOperand inputs[] = {
        g.UseUniqueRegister(base),
        g.GetEffectiveIndexOperand(index, &addressing_mode),
        g.UseUniqueRegister(value)};
    RecordWriteMode record_write_mode =
        WriteBarrierKindToRecordWriteMode(write_barrier_kind);
    InstructionOperand temps[] = {g.TempRegister(), g.TempRegister()};
    InstructionCode code = is_seqcst ? kArchAtomicStoreWithWriteBarrier
                                     : kArchStoreWithWriteBarrier;
    code |= AddressingModeField::encode(addressing_mode);
    code |= RecordWriteModeField::encode(record_write_mode);
    code |= AccessModeField::encode(access_mode);
    selector->Emit(code, 0, nullptr, arraysize(inputs), inputs,
                   arraysize(temps), temps);
  } else {
#ifdef V8_IS_TSAN
    // On TSAN builds we require two scratch registers. Because of this we also
    // have to modify the inputs to take into account possible aliasing and use
    // UseUniqueRegister which is not required for non-TSAN builds.
    InstructionOperand temps[] = {g.TempRegister(), g.TempRegister()};
    size_t temp_count = arraysize(temps);
    auto reg_kind = OperandGenerator::RegisterUseKind::kUseUniqueRegister;
#else
    InstructionOperand* temps = nullptr;
    size_t temp_count = 0;
    auto reg_kind = OperandGenerator::RegisterUseKind::kUseRegister;
#endif  // V8_IS_TSAN

    // Release and non-atomic stores emit MOV and sequentially consistent stores
    // emit XCHG.
    // https://www.cl.cam.ac.uk/~pes20/cpp/cpp0xmappings.html

    ArchOpcode opcode;
    AddressingMode addressing_mode;
    InstructionOperand inputs[4];
    size_t input_count = 0;

    if (is_seqcst) {
      // SeqCst stores emit XCHG instead of MOV, so encode the inputs as we
      // would for XCHG. XCHG can't encode the value as an immediate and has
      // fewer addressing modes available.
      inputs[input_count++] = g.UseUniqueRegister(value);
      inputs[input_count++] = g.UseUniqueRegister(base);
      inputs[input_count++] =
          g.GetEffectiveIndexOperand(index, &addressing_mode);
      opcode = GetSeqCstStoreOpcode(store_rep);
    } else {
      if ((ElementSizeLog2Of(store_rep.representation()) <
           kSystemPointerSizeLog2) &&
          value->opcode() == IrOpcode::kTruncateInt64ToInt32) {
        value = value->InputAt(0);
      }

      addressing_mode = g.GetEffectiveAddressMemoryOperand(
          node, inputs, &input_count, reg_kind);
      InstructionOperand value_operand = g.CanBeImmediate(value)
                                             ? g.UseImmediate(value)
                                             : g.UseRegister(value, reg_kind);
      inputs[input_count++] = value_operand;
      opcode = GetStoreOpcode(store_rep);
    }

    InstructionCode code = opcode
      | AddressingModeField::encode(addressing_mode)
      | AccessModeField::encode(access_mode);
    selector->Emit(code, 0, static_cast<InstructionOperand*>(nullptr),
                   input_count, inputs, temp_count, temps);
  }
}

}  // namespace

void InstructionSelector::VisitStore(Node* node) {
  return VisitStoreCommon(this, node, StoreRepresentationOf(node->op()),
                          base::nullopt);
}

void InstructionSelector::VisitProtectedStore(Node* node) {
  return VisitStoreCommon(this, node, StoreRepresentationOf(node->op()),
                          base::nullopt, MemoryAccessKind::kProtected);
}

// Architecture supports unaligned access, therefore VisitLoad is used instead
void InstructionSelector::VisitUnalignedLoad(Node* node) { UNREACHABLE(); }

// Architecture supports unaligned access, therefore VisitStore is used instead
void InstructionSelector::VisitUnalignedStore(Node* node) { UNREACHABLE(); }

void InstructionSelector::VisitStoreLane(Node* node) {
  X64OperandGenerator g(this);

  StoreLaneParameters params = StoreLaneParametersOf(node->op());
  InstructionCode opcode = kArchNop;
  if (params.rep == MachineRepresentation::kWord8) {
    opcode = kX64Pextrb;
  } else if (params.rep == MachineRepresentation::kWord16) {
    opcode = kX64Pextrw;
  } else if (params.rep == MachineRepresentation::kWord32) {
    opcode = kX64S128Store32Lane;
  } else if (params.rep == MachineRepresentation::kWord64) {
    opcode = kX64S128Store64Lane;
  } else {
    UNREACHABLE();
  }

  InstructionOperand inputs[4];
  size_t input_count = 0;
  AddressingMode addressing_mode =
      g.GetEffectiveAddressMemoryOperand(node, inputs, &input_count);
  opcode |= AddressingModeField::encode(addressing_mode);

  if (params.kind == MemoryAccessKind::kProtected) {
    opcode |= AccessModeField::encode(kMemoryAccessProtectedMemOutOfBounds);
  }

  InstructionOperand value_operand = g.UseRegister(node->InputAt(2));
  inputs[input_count++] = value_operand;
  inputs[input_count++] = g.UseImmediate(params.laneidx);
  DCHECK_GE(4, input_count);
  Emit(opcode, 0, nullptr, input_count, inputs);
}

// Shared routine for multiple binary operations.
static void VisitBinop(InstructionSelector* selector, Node* node,
                       InstructionCode opcode, FlagsContinuation* cont) {
  X64OperandGenerator g(selector);
  Int32BinopMatcher m(node);
  Node* left = m.left().node();
  Node* right = m.right().node();
  InstructionOperand inputs[8];
  size_t input_count = 0;
  InstructionOperand outputs[1];
  size_t output_count = 0;

  // TODO(turbofan): match complex addressing modes.
  if (left == right) {
    // If both inputs refer to the same operand, enforce allocating a register
    // for both of them to ensure that we don't end up generating code like
    // this:
    //
    //   mov rax, [rbp-0x10]
    //   add rax, [rbp-0x10]
    //   jo label
    InstructionOperand const input = g.UseRegister(left);
    inputs[input_count++] = input;
    inputs[input_count++] = input;
  } else if (g.CanBeImmediate(right)) {
    inputs[input_count++] = g.UseRegister(left);
    inputs[input_count++] = g.UseImmediate(right);
  } else {
    int effect_level = selector->GetEffectLevel(node, cont);
    if (node->op()->HasProperty(Operator::kCommutative) &&
        g.CanBeBetterLeftOperand(right) &&
        (!g.CanBeBetterLeftOperand(left) ||
         !g.CanBeMemoryOperand(opcode, node, right, effect_level))) {
      std::swap(left, right);
    }
    if (g.CanBeMemoryOperand(opcode, node, right, effect_level)) {
      inputs[input_count++] = g.UseRegister(left);
      AddressingMode addressing_mode =
          g.GetEffectiveAddressMemoryOperand(right, inputs, &input_count);
      opcode |= AddressingModeField::encode(addressing_mode);
    } else {
      inputs[input_count++] = g.UseRegister(left);
      inputs[input_count++] = g.Use(right);
    }
  }

  if (cont->IsBranch()) {
    inputs[input_count++] = g.Label(cont->true_block());
    inputs[input_count++] = g.Label(cont->false_block());
  }

  outputs[output_count++] = g.DefineSameAsFirst(node);

  DCHECK_NE(0u, input_count);
  DCHECK_EQ(1u, output_count);
  DCHECK_GE(arraysize(inputs), input_count);
  DCHECK_GE(arraysize(outputs), output_count);

  selector->EmitWithContinuation(opcode, output_count, outputs, input_count,
                                 inputs, cont);
}

// Shared routine for multiple binary operations.
static void VisitBinop(InstructionSelector* selector, Node* node,
                       InstructionCode opcode) {
  FlagsContinuation cont;
  VisitBinop(selector, node, opcode, &cont);
}

void InstructionSelector::VisitWord32And(Node* node) {
  X64OperandGenerator g(this);
  Uint32BinopMatcher m(node);
  if (m.right().Is(0xFF)) {
    Emit(kX64Movzxbl, g.DefineAsRegister(node), g.Use(m.left().node()));
  } else if (m.right().Is(0xFFFF)) {
    Emit(kX64Movzxwl, g.DefineAsRegister(node), g.Use(m.left().node()));
  } else {
    VisitBinop(this, node, kX64And32);
  }
}

void InstructionSelector::VisitWord64And(Node* node) {
  X64OperandGenerator g(this);
  Uint64BinopMatcher m(node);
  if (m.right().Is(0xFF)) {
    Emit(kX64Movzxbq, g.DefineAsRegister(node), g.Use(m.left().node()));
  } else if (m.right().Is(0xFFFF)) {
    Emit(kX64Movzxwq, g.DefineAsRegister(node), g.Use(m.left().node()));
  } else if (m.right().Is(0xFFFFFFFF)) {
    Emit(kX64Movl, g.DefineAsRegister(node), g.Use(m.left().node()));
  } else if (m.right().IsInRange(std::numeric_limits<uint32_t>::min(),
                                 std::numeric_limits<uint32_t>::max())) {
    Emit(kX64And32, g.DefineSameAsFirst(node), g.UseRegister(m.left().node()),
         g.UseImmediate(static_cast<int32_t>(m.right().ResolvedValue())));
  } else {
    VisitBinop(this, node, kX64And);
  }
}

void InstructionSelector::VisitWord32Or(Node* node) {
  VisitBinop(this, node, kX64Or32);
}

void InstructionSelector::VisitWord64Or(Node* node) {
  VisitBinop(this, node, kX64Or);
}

void InstructionSelector::VisitWord32Xor(Node* node) {
  X64OperandGenerator g(this);
  Uint32BinopMatcher m(node);
  if (m.right().Is(-1)) {
    Emit(kX64Not32, g.DefineSameAsFirst(node), g.UseRegister(m.left().node()));
  } else {
    VisitBinop(this, node, kX64Xor32);
  }
}

void InstructionSelector::VisitWord64Xor(Node* node) {
  X64OperandGenerator g(this);
  Uint64BinopMatcher m(node);
  if (m.right().Is(-1)) {
    Emit(kX64Not, g.DefineSameAsFirst(node), g.UseRegister(m.left().node()));
  } else {
    VisitBinop(this, node, kX64Xor);
  }
}

void InstructionSelector::VisitStackPointerGreaterThan(
    Node* node, FlagsContinuation* cont) {
  StackCheckKind kind = StackCheckKindOf(node->op());
  InstructionCode opcode =
      kArchStackPointerGreaterThan | MiscField::encode(static_cast<int>(kind));

  int effect_level = GetEffectLevel(node, cont);

  X64OperandGenerator g(this);
  Node* const value = node->InputAt(0);
  if (g.CanBeMemoryOperand(kX64Cmp, node, value, effect_level)) {
    DCHECK(IrOpcode::kLoad == value->opcode() ||
           IrOpcode::kLoadImmutable == value->opcode());

    // GetEffectiveAddressMemoryOperand can create at most 3 inputs.
    static constexpr int kMaxInputCount = 3;

    size_t input_count = 0;
    InstructionOperand inputs[kMaxInputCount];
    AddressingMode addressing_mode =
        g.GetEffectiveAddressMemoryOperand(value, inputs, &input_count);
    opcode |= AddressingModeField::encode(addressing_mode);
    DCHECK_LE(input_count, kMaxInputCount);

    EmitWithContinuation(opcode, 0, nullptr, input_count, inputs, cont);
  } else {
    EmitWithContinuation(opcode, g.UseRegister(value), cont);
  }
}

namespace {

void TryMergeTruncateInt64ToInt32IntoLoad(InstructionSelector* selector,
                                          Node* node, Node* load) {
  LoadRepresentation load_rep = LoadRepresentationOf(load->op());
  MachineRepresentation rep = load_rep.representation();
  InstructionCode opcode;
  switch (rep) {
    case MachineRepresentation::kBit:  // Fall through.
    case MachineRepresentation::kWord8:
      opcode = load_rep.IsSigned() ? kX64Movsxbl : kX64Movzxbl;
      break;
    case MachineRepresentation::kWord16:
      opcode = load_rep.IsSigned() ? kX64Movsxwl : kX64Movzxwl;
      break;
    case MachineRepresentation::kWord32:
    case MachineRepresentation::kWord64:
    case MachineRepresentation::kTaggedSigned:
    case MachineRepresentation::kTagged:
    case MachineRepresentation::kCompressed:  // Fall through.
      opcode = kX64Movl;
      break;
    default:
      UNREACHABLE();
  }
  X64OperandGenerator g(selector);
#ifdef V8_IS_TSAN
  // On TSAN builds we require one scratch register. Because of this we also
  // have to modify the inputs to take into account possible aliasing and use
  // UseUniqueRegister which is not required for non-TSAN builds.
  InstructionOperand temps[] = {g.TempRegister()};
  size_t temp_count = arraysize(temps);
  auto reg_kind = OperandGenerator::RegisterUseKind::kUseUniqueRegister;
#else
  InstructionOperand* temps = nullptr;
  size_t temp_count = 0;
  auto reg_kind = OperandGenerator::RegisterUseKind::kUseRegister;
#endif  // V8_IS_TSAN
  InstructionOperand outputs[] = {g.DefineAsRegister(node)};
  size_t input_count = 0;
  InstructionOperand inputs[3];
  AddressingMode mode =
      g.GetEffectiveAddressMemoryOperand(load, inputs, &input_count, reg_kind);
  opcode |= AddressingModeField::encode(mode);

  selector->Emit(opcode, 1, outputs, input_count, inputs, temp_count, temps);
}

// Shared routine for multiple 32-bit shift operations.
// TODO(bmeurer): Merge this with VisitWord64Shift using template magic?
void VisitWord32Shift(InstructionSelector* selector, Node* node,
                      ArchOpcode opcode) {
  X64OperandGenerator g(selector);
  Int32BinopMatcher m(node);
  Node* left = m.left().node();
  Node* right = m.right().node();

  if (left->opcode() == IrOpcode::kTruncateInt64ToInt32) {
    left = left->InputAt(0);
  }

  if (g.CanBeImmediate(right)) {
    selector->Emit(opcode, g.DefineSameAsFirst(node), g.UseRegister(left),
                   g.UseImmediate(right));
  } else {
    selector->Emit(opcode, g.DefineSameAsFirst(node), g.UseRegister(left),
                   g.UseFixed(right, rcx));
  }
}

// Shared routine for multiple 64-bit shift operations.
// TODO(bmeurer): Merge this with VisitWord32Shift using template magic?
void VisitWord64Shift(InstructionSelector* selector, Node* node,
                      ArchOpcode opcode) {
  X64OperandGenerator g(selector);
  Int64BinopMatcher m(node);
  Node* left = m.left().node();
  Node* right = m.right().node();

  if (g.CanBeImmediate(right)) {
    if (opcode == kX64Shr && m.left().IsChangeUint32ToUint64() &&
        m.right().HasResolvedValue() && m.right().ResolvedValue() < 32 &&
        m.right().ResolvedValue() >= 0) {
      opcode = kX64Shr32;
      left = left->InputAt(0);
    }
    selector->Emit(opcode, g.DefineSameAsFirst(node), g.UseRegister(left),
                   g.UseImmediate(right));
  } else {
    if (m.right().IsWord64And()) {
      Int64BinopMatcher mright(right);
      if (mright.right().Is(0x3F)) {
        right = mright.left().node();
      }
    }
    selector->Emit(opcode, g.DefineSameAsFirst(node), g.UseRegister(left),
                   g.UseFixed(right, rcx));
  }
}

// Shared routine for multiple shift operations with continuation.
template <typename BinopMatcher, int Bits>
bool TryVisitWordShift(InstructionSelector* selector, Node* node,
                       ArchOpcode opcode, FlagsContinuation* cont) {
  X64OperandGenerator g(selector);
  BinopMatcher m(node);
  Node* left = m.left().node();
  Node* right = m.right().node();

  // If the shift count is 0, the flags are not affected.
  if (!g.CanBeImmediate(right) ||
      (g.GetImmediateIntegerValue(right) & (Bits - 1)) == 0) {
    return false;
  }
  InstructionOperand output = g.DefineSameAsFirst(node);
  InstructionOperand inputs[2];
  inputs[0] = g.UseRegister(left);
  inputs[1] = g.UseImmediate(right);
  selector->EmitWithContinuation(opcode, 1, &output, 2, inputs, cont);
  return true;
}

void EmitLea(InstructionSelector* selector, InstructionCode opcode,
             Node* result, Node* index, int scale, Node* base,
             Node* displacement, DisplacementMode displacement_mode) {
  X64OperandGenerator g(selector);

  InstructionOperand inputs[4];
  size_t input_count = 0;
  AddressingMode mode =
      g.GenerateMemoryOperandInputs(index, scale, base, displacement,
                                    displacement_mode, inputs, &input_count);

  DCHECK_NE(0u, input_count);
  DCHECK_GE(arraysize(inputs), input_count);

  InstructionOperand outputs[1];
  outputs[0] = g.DefineAsRegister(result);

  opcode = AddressingModeField::encode(mode) | opcode;

  selector->Emit(opcode, 1, outputs, input_count, inputs);
}

}  // namespace

void InstructionSelector::VisitWord32Shl(Node* node) {
  Int32ScaleMatcher m(node, true);
  if (m.matches()) {
    Node* index = node->InputAt(0);
    Node* base = m.power_of_two_plus_one() ? index : nullptr;
    EmitLea(this, kX64Lea32, node, index, m.scale(), base, nullptr,
            kPositiveDisplacement);
    return;
  }
  VisitWord32Shift(this, node, kX64Shl32);
}

void InstructionSelector::VisitWord64Shl(Node* node) {
  X64OperandGenerator g(this);
  Int64ScaleMatcher m(node, true);
  if (m.matches()) {
    Node* index = node->InputAt(0);
    Node* base = m.power_of_two_plus_one() ? index : nullptr;
    EmitLea(this, kX64Lea, node, index, m.scale(), base, nullptr,
            kPositiveDisplacement);
    return;
  } else {
    Int64BinopMatcher bm(node);
    if ((bm.left().IsChangeInt32ToInt64() ||
         bm.left().IsChangeUint32ToUint64()) &&
        bm.right().IsInRange(32, 63)) {
      // There's no need to sign/zero-extend to 64-bit if we shift out the upper
      // 32 bits anyway.
      Emit(kX64Shl, g.DefineSameAsFirst(node),
           g.UseRegister(bm.left().node()->InputAt(0)),
           g.UseImmediate(bm.right().node()));
      return;
    }
  }
  VisitWord64Shift(this, node, kX64Shl);
}

void InstructionSelector::VisitWord32Shr(Node* node) {
  VisitWord32Shift(this, node, kX64Shr32);
}

namespace {

inline AddressingMode AddDisplacementToAddressingMode(AddressingMode mode) {
  switch (mode) {
    case kMode_MR:
      return kMode_MRI;
    case kMode_MR1:
      return kMode_MR1I;
    case kMode_MR2:
      return kMode_MR2I;
    case kMode_MR4:
      return kMode_MR4I;
    case kMode_MR8:
      return kMode_MR8I;
    case kMode_M1:
      return kMode_M1I;
    case kMode_M2:
      return kMode_M2I;
    case kMode_M4:
      return kMode_M4I;
    case kMode_M8:
      return kMode_M8I;
    case kMode_None:
    case kMode_MRI:
    case kMode_MR1I:
    case kMode_MR2I:
    case kMode_MR4I:
    case kMode_MR8I:
    case kMode_M1I:
    case kMode_M2I:
    case kMode_M4I:
    case kMode_M8I:
    case kMode_Root:
    case kMode_MCR:
    case kMode_MCRI:
      UNREACHABLE();
  }
  UNREACHABLE();
}

bool TryMatchLoadWord64AndShiftRight(InstructionSelector* selector, Node* node,
                                     InstructionCode opcode) {
  DCHECK(IrOpcode::kWord64Sar == node->opcode() ||
         IrOpcode::kWord64Shr == node->opcode());
  X64OperandGenerator g(selector);
  Int64BinopMatcher m(node);
  if (selector->CanCover(m.node(), m.left().node()) && m.left().IsLoad() &&
      m.right().Is(32)) {
    DCHECK_EQ(selector->GetEffectLevel(node),
              selector->GetEffectLevel(m.left().node()));
    // Just load and sign-extend the interesting 4 bytes instead. This happens,
    // for example, when we're loading and untagging SMIs.
    BaseWithIndexAndDisplacement64Matcher mleft(m.left().node(),
                                                AddressOption::kAllowAll);
    if (mleft.matches() && (mleft.displacement() == nullptr ||
                            g.CanBeImmediate(mleft.displacement()))) {
#ifdef V8_IS_TSAN
      // On TSAN builds we require one scratch register. Because of this we also
      // have to modify the inputs to take into account possible aliasing and
      // use UseUniqueRegister which is not required for non-TSAN builds.
      InstructionOperand temps[] = {g.TempRegister()};
      size_t temp_count = arraysize(temps);
      auto reg_kind = OperandGenerator::RegisterUseKind::kUseUniqueRegister;
#else
      InstructionOperand* temps = nullptr;
      size_t temp_count = 0;
      auto reg_kind = OperandGenerator::RegisterUseKind::kUseRegister;
#endif  // V8_IS_TSAN
      size_t input_count = 0;
      InstructionOperand inputs[3];
      AddressingMode mode = g.GetEffectiveAddressMemoryOperand(
          m.left().node(), inputs, &input_count, reg_kind);
      if (mleft.displacement() == nullptr) {
        // Make sure that the addressing mode indicates the presence of an
        // immediate displacement. It seems that we never use M1 and M2, but we
        // handle them here anyways.
        mode = AddDisplacementToAddressingMode(mode);
        inputs[input_count++] =
            ImmediateOperand(ImmediateOperand::INLINE_INT32, 4);
      } else {
        // In the case that the base address was zero, the displacement will be
        // in a register and replacing it with an immediate is not allowed. This
        // usually only happens in dead code anyway.
        if (!inputs[input_count - 1].IsImmediate()) return false;
        int32_t displacement = g.GetImmediateIntegerValue(mleft.displacement());
        inputs[input_count - 1] =
            ImmediateOperand(ImmediateOperand::INLINE_INT32, displacement + 4);
      }
      InstructionOperand outputs[] = {g.DefineAsRegister(node)};
      InstructionCode code = opcode | AddressingModeField::encode(mode);
      selector->Emit(code, 1, outputs, input_count, inputs, temp_count, temps);
      return true;
    }
  }
  return false;
}

}  // namespace

void InstructionSelector::VisitWord64Shr(Node* node) {
  if (TryMatchLoadWord64AndShiftRight(this, node, kX64Movl)) return;
  VisitWord64Shift(this, node, kX64Shr);
}

void InstructionSelector::VisitWord32Sar(Node* node) {
  X64OperandGenerator g(this);
  Int32BinopMatcher m(node);
  if (CanCover(m.node(), m.left().node()) && m.left().IsWord32Shl()) {
    Int32BinopMatcher mleft(m.left().node());
    if (mleft.right().Is(16) && m.right().Is(16)) {
      Emit(kX64Movsxwl, g.DefineAsRegister(node), g.Use(mleft.left().node()));
      return;
    } else if (mleft.right().Is(24) && m.right().Is(24)) {
      Emit(kX64Movsxbl, g.DefineAsRegister(node), g.Use(mleft.left().node()));
      return;
    }
  }
  VisitWord32Shift(this, node, kX64Sar32);
}

void InstructionSelector::VisitWord64Sar(Node* node) {
  if (TryMatchLoadWord64AndShiftRight(this, node, kX64Movsxlq)) return;
  VisitWord64Shift(this, node, kX64Sar);
}

void InstructionSelector::VisitWord32Rol(Node* node) {
  VisitWord32Shift(this, node, kX64Rol32);
}

void InstructionSelector::VisitWord64Rol(Node* node) {
  VisitWord64Shift(this, node, kX64Rol);
}

void InstructionSelector::VisitWord32Ror(Node* node) {
  VisitWord32Shift(this, node, kX64Ror32);
}

void InstructionSelector::VisitWord64Ror(Node* node) {
  VisitWord64Shift(this, node, kX64Ror);
}

void InstructionSelector::VisitWord32ReverseBits(Node* node) { UNREACHABLE(); }

void InstructionSelector::VisitWord64ReverseBits(Node* node) { UNREACHABLE(); }

void InstructionSelector::VisitWord64ReverseBytes(Node* node) {
  X64OperandGenerator g(this);
  Emit(kX64Bswap, g.DefineSameAsFirst(node), g.UseRegister(node->InputAt(0)));
}

void InstructionSelector::VisitWord32ReverseBytes(Node* node) {
  X64OperandGenerator g(this);
  Emit(kX64Bswap32, g.DefineSameAsFirst(node), g.UseRegister(node->InputAt(0)));
}

void InstructionSelector::VisitSimd128ReverseBytes(Node* node) {
  UNREACHABLE();
}

void InstructionSelector::VisitInt32Add(Node* node) {
  X64OperandGenerator g(this);

  // No need to truncate the values before Int32Add.
  DCHECK_EQ(node->InputCount(), 2);
  Node* left = node->InputAt(0);
  Node* right = node->InputAt(1);
  if (left->opcode() == IrOpcode::kTruncateInt64ToInt32) {
    node->ReplaceInput(0, left->InputAt(0));
  }
  if (right->opcode() == IrOpcode::kTruncateInt64ToInt32) {
    node->ReplaceInput(1, right->InputAt(0));
  }

  // Try to match the Add to a leal pattern
  BaseWithIndexAndDisplacement32Matcher m(node);
  if (m.matches() &&
      (m.displacement() == nullptr || g.CanBeImmediate(m.displacement()))) {
    EmitLea(this, kX64Lea32, node, m.index(), m.scale(), m.base(),
            m.displacement(), m.displacement_mode());
    return;
  }

  // No leal pattern match, use addl
  VisitBinop(this, node, kX64Add32);
}

void InstructionSelector::VisitInt64Add(Node* node) {
  X64OperandGenerator g(this);

  // Try to match the Add to a leaq pattern
  BaseWithIndexAndDisplacement64Matcher m(node);
  if (m.matches() &&
      (m.displacement() == nullptr || g.CanBeImmediate(m.displacement()))) {
    EmitLea(this, kX64Lea, node, m.index(), m.scale(), m.base(),
            m.displacement(), m.displacement_mode());
    return;
  }

  // No leal pattern match, use addq
  VisitBinop(this, node, kX64Add);
}

void InstructionSelector::VisitInt64AddWithOverflow(Node* node) {
  if (Node* ovf = NodeProperties::FindProjection(node, 1)) {
    FlagsContinuation cont = FlagsContinuation::ForSet(kOverflow, ovf);
    return VisitBinop(this, node, kX64Add, &cont);
  }
  FlagsContinuation cont;
  VisitBinop(this, node, kX64Add, &cont);
}

void InstructionSelector::VisitInt32Sub(Node* node) {
  X64OperandGenerator g(this);
  DCHECK_EQ(node->InputCount(), 2);
  Node* input1 = node->InputAt(0);
  Node* input2 = node->InputAt(1);
  if (input1->opcode() == IrOpcode::kTruncateInt64ToInt32 &&
      g.CanBeImmediate(input2)) {
    int32_t imm = g.GetImmediateIntegerValue(input2);
    InstructionOperand int64_input = g.UseRegister(input1->InputAt(0));
    if (imm == 0) {
      // Emit "movl" for subtraction of 0.
      Emit(kX64Movl, g.DefineAsRegister(node), int64_input);
    } else {
      // Omit truncation and turn subtractions of constant values into immediate
      // "leal" instructions by negating the value.
      Emit(kX64Lea32 | AddressingModeField::encode(kMode_MRI),
           g.DefineAsRegister(node), int64_input,
           g.TempImmediate(base::NegateWithWraparound(imm)));
    }
    return;
  }

  Int32BinopMatcher m(node);
  if (m.left().Is(0)) {
    Emit(kX64Neg32, g.DefineSameAsFirst(node), g.UseRegister(m.right().node()));
  } else if (m.right().Is(0)) {
    // {EmitIdentity} reuses the virtual register of the first input
    // for the output. This is exactly what we want here.
    EmitIdentity(node);
  } else if (m.right().HasResolvedValue() &&
             g.CanBeImmediate(m.right().node())) {
    // Turn subtractions of constant values into immediate "leal" instructions
    // by negating the value.
    Emit(
        kX64Lea32 | AddressingModeField::encode(kMode_MRI),
        g.DefineAsRegister(node), g.UseRegister(m.left().node()),
        g.TempImmediate(base::NegateWithWraparound(m.right().ResolvedValue())));
  } else {
    VisitBinop(this, node, kX64Sub32);
  }
}

void InstructionSelector::VisitInt64Sub(Node* node) {
  X64OperandGenerator g(this);
  Int64BinopMatcher m(node);
  if (m.left().Is(0)) {
    Emit(kX64Neg, g.DefineSameAsFirst(node), g.UseRegister(m.right().node()));
  } else {
    if (m.right().HasResolvedValue() && g.CanBeImmediate(m.right().node())) {
      // Turn subtractions of constant values into immediate "leaq" instructions
      // by negating the value.
      Emit(kX64Lea | AddressingModeField::encode(kMode_MRI),
           g.DefineAsRegister(node), g.UseRegister(m.left().node()),
           g.TempImmediate(-static_cast<int32_t>(m.right().ResolvedValue())));
      return;
    }
    VisitBinop(this, node, kX64Sub);
  }
}

void InstructionSelector::VisitInt64SubWithOverflow(Node* node) {
  if (Node* ovf = NodeProperties::FindProjection(node, 1)) {
    FlagsContinuation cont = FlagsContinuation::ForSet(kOverflow, ovf);
    return VisitBinop(this, node, kX64Sub, &cont);
  }
  FlagsContinuation cont;
  VisitBinop(this, node, kX64Sub, &cont);
}

namespace {

void VisitMul(InstructionSelector* selector, Node* node, ArchOpcode opcode) {
  X64OperandGenerator g(selector);
  Int32BinopMatcher m(node);
  Node* left = m.left().node();
  Node* right = m.right().node();
  if (g.CanBeImmediate(right)) {
    selector->Emit(opcode, g.DefineAsRegister(node), g.Use(left),
                   g.UseImmediate(right));
  } else {
    if (g.CanBeBetterLeftOperand(right)) {
      std::swap(left, right);
    }
    selector->Emit(opcode, g.DefineSameAsFirst(node), g.UseRegister(left),
                   g.Use(right));
  }
}

void VisitMulHigh(InstructionSelector* selector, Node* node,
                  ArchOpcode opcode) {
  X64OperandGenerator g(selector);
  Node* left = node->InputAt(0);
  Node* right = node->InputAt(1);
  if (selector->IsLive(left) && !selector->IsLive(right)) {
    std::swap(left, right);
  }
  InstructionOperand temps[] = {g.TempRegister(rax)};
  // TODO(turbofan): We use UseUniqueRegister here to improve register
  // allocation.
  selector->Emit(opcode, g.DefineAsFixed(node, rdx), g.UseFixed(left, rax),
                 g.UseUniqueRegister(right), arraysize(temps), temps);
}

void VisitDiv(InstructionSelector* selector, Node* node, ArchOpcode opcode) {
  X64OperandGenerator g(selector);
  InstructionOperand temps[] = {g.TempRegister(rdx)};
  selector->Emit(
      opcode, g.DefineAsFixed(node, rax), g.UseFixed(node->InputAt(0), rax),
      g.UseUniqueRegister(node->InputAt(1)), arraysize(temps), temps);
}

void VisitMod(InstructionSelector* selector, Node* node, ArchOpcode opcode) {
  X64OperandGenerator g(selector);
  InstructionOperand temps[] = {g.TempRegister(rax)};
  selector->Emit(
      opcode, g.DefineAsFixed(node, rdx), g.UseFixed(node->InputAt(0), rax),
      g.UseUniqueRegister(node->InputAt(1)), arraysize(temps), temps);
}

}  // namespace

void InstructionSelector::VisitInt32Mul(Node* node) {
  Int32ScaleMatcher m(node, true);
  if (m.matches()) {
    Node* index = node->InputAt(0);
    Node* base = m.power_of_two_plus_one() ? index : nullptr;
    EmitLea(this, kX64Lea32, node, index, m.scale(), base, nullptr,
            kPositiveDisplacement);
    return;
  }
  VisitMul(this, node, kX64Imul32);
}

void InstructionSelector::VisitInt32MulWithOverflow(Node* node) {
  // TODO(mvstanton): Use Int32ScaleMatcher somehow.
  if (Node* ovf = NodeProperties::FindProjection(node, 1)) {
    FlagsContinuation cont = FlagsContinuation::ForSet(kOverflow, ovf);
    return VisitBinop(this, node, kX64Imul32, &cont);
  }
  FlagsContinuation cont;
  VisitBinop(this, node, kX64Imul32, &cont);
}

void InstructionSelector::VisitInt64Mul(Node* node) {
  Int64ScaleMatcher m(node, true);
  if (m.matches()) {
    Node* index = node->InputAt(0);
    Node* base = m.power_of_two_plus_one() ? index : nullptr;
    EmitLea(this, kX64Lea, node, index, m.scale(), base, nullptr,
            kPositiveDisplacement);
    return;
  }
  VisitMul(this, node, kX64Imul);
}

void InstructionSelector::VisitInt64MulWithOverflow(Node* node) {
  if (Node* ovf = NodeProperties::FindProjection(node, 1)) {
    FlagsContinuation cont = FlagsContinuation::ForSet(kOverflow, ovf);
    return VisitBinop(this, node, kX64Imul, &cont);
  }
  FlagsContinuation cont;
  VisitBinop(this, node, kX64Imul, &cont);
}

void InstructionSelector::VisitInt32MulHigh(Node* node) {
  VisitMulHigh(this, node, kX64ImulHigh32);
}

void InstructionSelector::VisitInt64MulHigh(Node* node) {
  VisitMulHigh(this, node, kX64ImulHigh64);
}

void InstructionSelector::VisitInt32Div(Node* node) {
  VisitDiv(this, node, kX64Idiv32);
}

void InstructionSelector::VisitInt64Div(Node* node) {
  VisitDiv(this, node, kX64Idiv);
}

void InstructionSelector::VisitUint32Div(Node* node) {
  VisitDiv(this, node, kX64Udiv32);
}

void InstructionSelector::VisitUint64Div(Node* node) {
  VisitDiv(this, node, kX64Udiv);
}

void InstructionSelector::VisitInt32Mod(Node* node) {
  VisitMod(this, node, kX64Idiv32);
}

void InstructionSelector::VisitInt64Mod(Node* node) {
  VisitMod(this, node, kX64Idiv);
}

void InstructionSelector::VisitUint32Mod(Node* node) {
  VisitMod(this, node, kX64Udiv32);
}

void InstructionSelector::VisitUint64Mod(Node* node) {
  VisitMod(this, node, kX64Udiv);
}

void InstructionSelector::VisitUint32MulHigh(Node* node) {
  VisitMulHigh(this, node, kX64UmulHigh32);
}

void InstructionSelector::VisitUint64MulHigh(Node* node) {
  VisitMulHigh(this, node, kX64UmulHigh64);
}

// TryTruncateFloat32ToInt64 and TryTruncateFloat64ToInt64 operations attempt
// truncation from 32|64-bit float to 64-bit integer by performing roughly the
// following steps:
// 1. Round the original FP value to zero, store in `rounded`;
// 2. Convert the original FP value to integer;
// 3. Convert the integer value back to floating point, store in
// `converted_back`;
// 4. If `rounded` == `converted_back`:
//      Set Projection(1) := 1;   -- the value was in range
//    Else:
//      Set Projection(1) := 0;   -- the value was out of range
void InstructionSelector::VisitTryTruncateFloat32ToInt64(Node* node) {
  X64OperandGenerator g(this);
  InstructionOperand inputs[] = {g.UseRegister(node->InputAt(0))};
  InstructionOperand outputs[2];
  InstructionOperand temps[1];
  size_t output_count = 0;
  size_t temp_count = 0;
  outputs[output_count++] = g.DefineAsRegister(node);

  Node* success_output = NodeProperties::FindProjection(node, 1);
  if (success_output) {
    outputs[output_count++] = g.DefineAsRegister(success_output);
    temps[temp_count++] = g.TempSimd128Register();
  }

  Emit(kSSEFloat32ToInt64, output_count, outputs, 1, inputs, temp_count, temps);
}

// TryTruncateFloatNNToUintDD operations attempt truncation from NN-bit
// float to DD-bit integer by using ConvertFloatToUintDD macro instructions.
// It performs a float-to-int instruction, rounding to zero and tests whether
// the result is positive integer (the default, fast case), which means the
// value is in range. Then, we set Projection(1) := 1. Else, we perform
// additional subtraction, conversion and (in case the value was originally
// negative, but still within range) we restore it and set Projection(1) := 1.
// In all other cases we set Projection(1) := 0, denoting value out of range.
void InstructionSelector::VisitTryTruncateFloat64ToUint32(Node* node) {
  X64OperandGenerator g(this);
  InstructionOperand inputs[] = {g.UseRegister(node->InputAt(0))};
  InstructionOperand outputs[2];
  size_t output_count = 0;
  outputs[output_count++] = g.DefineAsRegister(node);

  Node* success_output = NodeProperties::FindProjection(node, 1);
  if (success_output) {
    outputs[output_count++] = g.DefineAsRegister(success_output);
  }

  Emit(kSSEFloat64ToUint32, output_count, outputs, 1, inputs);
}

void InstructionSelector::VisitTryTruncateFloat32ToUint64(Node* node) {
  X64OperandGenerator g(this);
  InstructionOperand inputs[] = {g.UseRegister(node->InputAt(0))};
  InstructionOperand outputs[2];
  size_t output_count = 0;
  outputs[output_count++] = g.DefineAsRegister(node);

  Node* success_output = NodeProperties::FindProjection(node, 1);
  if (success_output) {
    outputs[output_count++] = g.DefineAsRegister(success_output);
  }

  Emit(kSSEFloat32ToUint64, output_count, outputs, 1, inputs);
}

void InstructionSelector::VisitTryTruncateFloat64ToUint64(Node* node) {
  X64OperandGenerator g(this);
  InstructionOperand inputs[] = {g.UseRegister(node->InputAt(0))};
  InstructionOperand outputs[2];
  size_t output_count = 0;
  outputs[output_count++] = g.DefineAsRegister(node);

  Node* success_output = NodeProperties::FindProjection(node, 1);
  if (success_output) {
    outputs[output_count++] = g.DefineAsRegister(success_output);
  }

  Emit(kSSEFloat64ToUint64, output_count, outputs, 1, inputs);
}

void InstructionSelector::VisitTryTruncateFloat64ToInt64(Node* node) {
  X64OperandGenerator g(this);
  InstructionOperand inputs[] = {g.UseRegister(node->InputAt(0))};
  InstructionOperand outputs[2];
  InstructionOperand temps[1];
  size_t output_count = 0;
  size_t temp_count = 0;
  outputs[output_count++] = g.DefineAsRegister(node);

  Node* success_output = NodeProperties::FindProjection(node, 1);
  if (success_output) {
    outputs[output_count++] = g.DefineAsRegister(success_output);
    temps[temp_count++] = g.TempSimd128Register();
  }

  Emit(kSSEFloat64ToInt64, output_count, outputs, 1, inputs, temp_count, temps);
}

void InstructionSelector::VisitTryTruncateFloat64ToInt32(Node* node) {
  X64OperandGenerator g(this);
  InstructionOperand inputs[] = {g.UseRegister(node->InputAt(0))};
  InstructionOperand outputs[2];
  InstructionOperand temps[1];
  size_t output_count = 0;
  size_t temp_count = 0;
  outputs[output_count++] = g.DefineAsRegister(node);

  Node* success_output = NodeProperties::FindProjection(node, 1);
  if (success_output) {
    outputs[output_count++] = g.DefineAsRegister(success_output);
    temps[temp_count++] = g.TempSimd128Register();
  }

  Emit(kSSEFloat64ToInt32, output_count, outputs, 1, inputs, temp_count, temps);
}

void InstructionSelector::VisitBitcastWord32ToWord64(Node* node) {
  DCHECK(SmiValuesAre31Bits());
  DCHECK(COMPRESS_POINTERS_BOOL);
  EmitIdentity(node);
}

void InstructionSelector::VisitChangeInt32ToInt64(Node* node) {
  DCHECK_EQ(node->InputCount(), 1);

  X64OperandGenerator g(this);
  Node* const value = node->InputAt(0);
  if ((value->opcode() == IrOpcode::kLoad ||
       value->opcode() == IrOpcode::kLoadImmutable) &&
      CanCover(node, value)) {
    LoadRepresentation load_rep = LoadRepresentationOf(value->op());
    MachineRepresentation rep = load_rep.representation();
    InstructionCode opcode;
    switch (rep) {
      case MachineRepresentation::kBit:  // Fall through.
      case MachineRepresentation::kWord8:
        opcode = load_rep.IsSigned() ? kX64Movsxbq : kX64Movzxbq;
        break;
      case MachineRepresentation::kWord16:
        opcode = load_rep.IsSigned() ? kX64Movsxwq : kX64Movzxwq;
        break;
      case MachineRepresentation::kWord32:
      case MachineRepresentation::kWord64:
        // Since BitcastElider may remove nodes of
        // IrOpcode::kTruncateInt64ToInt32 and directly use the inputs, values
        // with kWord64 can also reach this line.
      case MachineRepresentation::kTaggedSigned:
      case MachineRepresentation::kTagged:
        // ChangeInt32ToInt64 must interpret its input as a _signed_ 32-bit
        // integer, so here we must sign-extend the loaded value in any case.
        opcode = kX64Movsxlq;
        break;
      default:
        UNREACHABLE();
    }
    InstructionOperand outputs[] = {g.DefineAsRegister(node)};
    size_t input_count = 0;
    InstructionOperand inputs[3];
    AddressingMode mode = g.GetEffectiveAddressMemoryOperand(
        node->InputAt(0), inputs, &input_count);
    opcode |= AddressingModeField::encode(mode);
    Emit(opcode, 1, outputs, input_count, inputs);
  } else {
    Emit(kX64Movsxlq, g.DefineAsRegister(node), g.Use(node->InputAt(0)));
  }
}

bool InstructionSelector::ZeroExtendsWord32ToWord64NoPhis(Node* node) {
  X64OperandGenerator g(this);
  DCHECK_NE(node->opcode(), IrOpcode::kPhi);
  switch (node->opcode()) {
    case IrOpcode::kWord32And:
    case IrOpcode::kWord32Or:
    case IrOpcode::kWord32Xor:
    case IrOpcode::kWord32Shl:
    case IrOpcode::kWord32Shr:
    case IrOpcode::kWord32Sar:
    case IrOpcode::kWord32Rol:
    case IrOpcode::kWord32Ror:
    case IrOpcode::kWord32Equal:
    case IrOpcode::kInt32Add:
    case IrOpcode::kInt32Sub:
    case IrOpcode::kInt32Mul:
    case IrOpcode::kInt32MulHigh:
    case IrOpcode::kInt32Div:
    case IrOpcode::kInt32LessThan:
    case IrOpcode::kInt32LessThanOrEqual:
    case IrOpcode::kInt32Mod:
    case IrOpcode::kUint32Div:
    case IrOpcode::kUint32LessThan:
    case IrOpcode::kUint32LessThanOrEqual:
    case IrOpcode::kUint32Mod:
    case IrOpcode::kUint32MulHigh:
    case IrOpcode::kTruncateInt64ToInt32:
      // These 32-bit operations implicitly zero-extend to 64-bit on x64, so the
      // zero-extension is a no-op.
      return true;
    case IrOpcode::kProjection: {
      Node* const value = node->InputAt(0);
      switch (value->opcode()) {
        case IrOpcode::kInt32AddWithOverflow:
        case IrOpcode::kInt32SubWithOverflow:
        case IrOpcode::kInt32MulWithOverflow:
          return true;
        default:
          return false;
      }
    }
    case IrOpcode::kLoad:
    case IrOpcode::kLoadImmutable:
    case IrOpcode::kProtectedLoad:
    case IrOpcode::kLoadTrapOnNull: {
      // The movzxbl/movsxbl/movzxwl/movsxwl/movl operations implicitly
      // zero-extend to 64-bit on x64, so the zero-extension is a no-op.
      LoadRepresentation load_rep = LoadRepresentationOf(node->op());
      switch (load_rep.representation()) {
        case MachineRepresentation::kWord8:
        case MachineRepresentation::kWord16:
        case MachineRepresentation::kWord32:
          return true;
        default:
          return false;
      }
    }
    case IrOpcode::kInt32Constant:
    case IrOpcode::kInt64Constant:
      // Constants are loaded with movl or movq, or xorl for zero; see
      // CodeGenerator::AssembleMove. So any non-negative constant that fits
      // in a 32-bit signed integer is zero-extended to 64 bits.
      if (g.CanBeImmediate(node)) {
        return g.GetImmediateIntegerValue(node) >= 0;
      }
      return false;
    default:
      return false;
  }
}

void InstructionSelector::VisitChangeUint32ToUint64(Node* node) {
  X64OperandGenerator g(this);
  Node* value = node->InputAt(0);
  if (ZeroExtendsWord32ToWord64(value)) {
    // These 32-bit operations implicitly zero-extend to 64-bit on x64, so the
    // zero-extension is a no-op.
    return EmitIdentity(node);
  }
  Emit(kX64Movl, g.DefineAsRegister(node), g.Use(value));
}

namespace {

void VisitRO(InstructionSelector* selector, Node* node,
             InstructionCode opcode) {
  X64OperandGenerator g(selector);
  selector->Emit(opcode, g.DefineAsRegister(node), g.Use(node->InputAt(0)));
}

void VisitRR(InstructionSelector* selector, Node* node,
             InstructionCode opcode) {
  X64OperandGenerator g(selector);
  selector->Emit(opcode, g.DefineAsRegister(node),
                 g.UseRegister(node->InputAt(0)));
}

void VisitRRO(InstructionSelector* selector, Node* node,
              InstructionCode opcode) {
  X64OperandGenerator g(selector);
  selector->Emit(opcode, g.DefineSameAsFirst(node),
                 g.UseRegister(node->InputAt(0)), g.Use(node->InputAt(1)));
}

void VisitFloatBinop(InstructionSelector* selector, Node* node,
                     InstructionCode avx_opcode, InstructionCode sse_opcode) {
  X64OperandGenerator g(selector);
  Node* left = node->InputAt(0);
  Node* right = node->InputAt(1);
  InstructionOperand inputs[8];
  size_t input_count = 0;
  InstructionOperand outputs[1];
  size_t output_count = 0;

  if (left == right) {
    // If both inputs refer to the same operand, enforce allocating a register
    // for both of them to ensure that we don't end up generating code like
    // this:
    //
    //   movss rax, [rbp-0x10]
    //   addss rax, [rbp-0x10]
    //   jo label
    InstructionOperand const input = g.UseRegister(left);
    inputs[input_count++] = input;
    inputs[input_count++] = input;
  } else {
    int effect_level = selector->GetEffectLevel(node);
    if (node->op()->HasProperty(Operator::kCommutative) &&
        (g.CanBeBetterLeftOperand(right) ||
         g.CanBeMemoryOperand(avx_opcode, node, left, effect_level)) &&
        (!g.CanBeBetterLeftOperand(left) ||
         !g.CanBeMemoryOperand(avx_opcode, node, right, effect_level))) {
      std::swap(left, right);
    }
    if (g.CanBeMemoryOperand(avx_opcode, node, right, effect_level)) {
      inputs[input_count++] = g.UseRegister(left);
      AddressingMode addressing_mode =
          g.GetEffectiveAddressMemoryOperand(right, inputs, &input_count);
      avx_opcode |= AddressingModeField::encode(addressing_mode);
      sse_opcode |= AddressingModeField::encode(addressing_mode);
    } else {
      inputs[input_count++] = g.UseRegister(left);
      inputs[input_count++] = g.Use(right);
    }
  }

  DCHECK_NE(0u, input_count);
  DCHECK_GE(arraysize(inputs), input_count);

  if (selector->IsSupported(AVX)) {
    outputs[output_count++] = g.DefineAsRegister(node);
    DCHECK_EQ(1u, output_count);
    DCHECK_GE(arraysize(outputs), output_count);
    selector->Emit(avx_opcode, output_count, outputs, input_count, inputs);
  } else {
    outputs[output_count++] = g.DefineSameAsFirst(node);
    DCHECK_EQ(1u, output_count);
    DCHECK_GE(arraysize(outputs), output_count);
    selector->Emit(sse_opcode, output_count, outputs, input_count, inputs);
  }
}

void VisitFloatUnop(InstructionSelector* selector, Node* node, Node* input,
                    InstructionCode opcode) {
  X64OperandGenerator g(selector);
  if (selector->IsSupported(AVX)) {
    selector->Emit(opcode, g.DefineAsRegister(node), g.UseRegister(input));
  } else {
    selector->Emit(opcode, g.DefineSameAsFirst(node), g.UseRegister(input));
  }
}

}  // namespace

#define RO_OP_LIST(V)                                                    \
  V(Word64Clz, kX64Lzcnt)                                                \
  V(Word32Clz, kX64Lzcnt32)                                              \
  V(Word64Ctz, kX64Tzcnt)                                                \
  V(Word32Ctz, kX64Tzcnt32)                                              \
  V(Word64Popcnt, kX64Popcnt)                                            \
  V(Word32Popcnt, kX64Popcnt32)                                          \
  V(Float64Sqrt, kSSEFloat64Sqrt)                                        \
  V(Float32Sqrt, kSSEFloat32Sqrt)                                        \
  V(ChangeFloat64ToInt32, kSSEFloat64ToInt32)                            \
  V(ChangeFloat64ToInt64, kSSEFloat64ToInt64)                            \
  V(ChangeFloat64ToUint32, kSSEFloat64ToUint32 | MiscField::encode(1))   \
  V(TruncateFloat64ToInt64, kSSEFloat64ToInt64)                          \
  V(TruncateFloat64ToUint32, kSSEFloat64ToUint32 | MiscField::encode(0)) \
  V(ChangeFloat64ToUint64, kSSEFloat64ToUint64)                          \
  V(TruncateFloat64ToFloat32, kSSEFloat64ToFloat32)                      \
  V(ChangeFloat32ToFloat64, kSSEFloat32ToFloat64)                        \
  V(TruncateFloat32ToInt32, kSSEFloat32ToInt32)                          \
  V(TruncateFloat32ToUint32, kSSEFloat32ToUint32)                        \
  V(ChangeInt32ToFloat64, kSSEInt32ToFloat64)                            \
  V(ChangeInt64ToFloat64, kSSEInt64ToFloat64)                            \
  V(ChangeUint32ToFloat64, kSSEUint32ToFloat64)                          \
  V(RoundFloat64ToInt32, kSSEFloat64ToInt32)                             \
  V(RoundInt32ToFloat32, kSSEInt32ToFloat32)                             \
  V(RoundInt64ToFloat32, kSSEInt64ToFloat32)                             \
  V(RoundUint64ToFloat32, kSSEUint64ToFloat32)                           \
  V(RoundInt64ToFloat64, kSSEInt64ToFloat64)                             \
  V(RoundUint64ToFloat64, kSSEUint64ToFloat64)                           \
  V(RoundUint32ToFloat32, kSSEUint32ToFloat32)                           \
  V(BitcastFloat32ToInt32, kX64BitcastFI)                                \
  V(BitcastFloat64ToInt64, kX64BitcastDL)                                \
  V(BitcastInt32ToFloat32, kX64BitcastIF)                                \
  V(BitcastInt64ToFloat64, kX64BitcastLD)                                \
  V(Float64ExtractLowWord32, kSSEFloat64ExtractLowWord32)                \
  V(Float64ExtractHighWord32, kSSEFloat64ExtractHighWord32)              \
  V(SignExtendWord8ToInt32, kX64Movsxbl)                                 \
  V(SignExtendWord16ToInt32, kX64Movsxwl)                                \
  V(SignExtendWord8ToInt64, kX64Movsxbq)                                 \
  V(SignExtendWord16ToInt64, kX64Movsxwq)                                \
  V(SignExtendWord32ToInt64, kX64Movsxlq)

#define RR_OP_LIST(V)                                                         \
  V(Float32RoundDown, kSSEFloat32Round | MiscField::encode(kRoundDown))       \
  V(Float64RoundDown, kSSEFloat64Round | MiscField::encode(kRoundDown))       \
  V(Float32RoundUp, kSSEFloat32Round | MiscField::encode(kRoundUp))           \
  V(Float64RoundUp, kSSEFloat64Round | MiscField::encode(kRoundUp))           \
  V(Float32RoundTruncate, kSSEFloat32Round | MiscField::encode(kRoundToZero)) \
  V(Float64RoundTruncate, kSSEFloat64Round | MiscField::encode(kRoundToZero)) \
  V(Float32RoundTiesEven,                                                     \
    kSSEFloat32Round | MiscField::encode(kRoundToNearest))                    \
  V(Float64RoundTiesEven,                                                     \
    kSSEFloat64Round | MiscField::encode(kRoundToNearest))                    \
  V(F32x4Ceil, kX64F32x4Round | MiscField::encode(kRoundUp))                  \
  V(F32x4Floor, kX64F32x4Round | MiscField::encode(kRoundDown))               \
  V(F32x4Trunc, kX64F32x4Round | MiscField::encode(kRoundToZero))             \
  V(F32x4NearestInt, kX64F32x4Round | MiscField::encode(kRoundToNearest))     \
  V(F64x2Ceil, kX64F64x2Round | MiscField::encode(kRoundUp))                  \
  V(F64x2Floor, kX64F64x2Round | MiscField::encode(kRoundDown))               \
  V(F64x2Trunc, kX64F64x2Round | MiscField::encode(kRoundToZero))             \
  V(F64x2NearestInt, kX64F64x2Round | MiscField::encode(kRoundToNearest))

#define RO_VISITOR(Name, opcode)                      \
  void InstructionSelector::Visit##Name(Node* node) { \
    VisitRO(this, node, opcode);                      \
  }
RO_OP_LIST(RO_VISITOR)
#undef RO_VISITOR
#undef RO_OP_LIST

#define RR_VISITOR(Name, opcode)                      \
  void InstructionSelector::Visit##Name(Node* node) { \
    VisitRR(this, node, opcode);                      \
  }
RR_OP_LIST(RR_VISITOR)
#undef RR_VISITOR
#undef RR_OP_LIST

void InstructionSelector::VisitTruncateFloat64ToWord32(Node* node) {
  VisitRR(this, node, kArchTruncateDoubleToI);
}

void InstructionSelector::VisitTruncateInt64ToInt32(Node* node) {
  // We rely on the fact that TruncateInt64ToInt32 zero extends the
  // value (see ZeroExtendsWord32ToWord64). So all code paths here
  // have to satisfy that condition.
  X64OperandGenerator g(this);
  Node* value = node->InputAt(0);
  bool can_cover = false;
  if (value->opcode() == IrOpcode::kBitcastTaggedToWordForTagAndSmiBits) {
    can_cover = CanCover(node, value) && CanCover(value, value->InputAt(0));
    value = value->InputAt(0);
  } else {
    can_cover = CanCover(node, value);
  }
  if (can_cover) {
    switch (value->opcode()) {
      case IrOpcode::kWord64Sar:
      case IrOpcode::kWord64Shr: {
        Int64BinopMatcher m(value);
        if (m.right().Is(32)) {
          if (CanCover(value, value->InputAt(0)) &&
              TryMatchLoadWord64AndShiftRight(this, value, kX64Movl)) {
            return EmitIdentity(node);
          }
          Emit(kX64Shr, g.DefineSameAsFirst(node),
               g.UseRegister(m.left().node()), g.TempImmediate(32));
          return;
        }
        break;
      }
      case IrOpcode::kLoad:
      case IrOpcode::kLoadImmutable: {
        TryMergeTruncateInt64ToInt32IntoLoad(this, node, value);
        return;
      }
      default:
        break;
    }
  }
  Emit(kX64Movl, g.DefineAsRegister(node), g.Use(value));
}

void InstructionSelector::VisitFloat32Add(Node* node) {
  VisitFloatBinop(this, node, kAVXFloat32Add, kSSEFloat32Add);
}

void InstructionSelector::VisitFloat32Sub(Node* node) {
  VisitFloatBinop(this, node, kAVXFloat32Sub, kSSEFloat32Sub);
}

void InstructionSelector::VisitFloat32Mul(Node* node) {
  VisitFloatBinop(this, node, kAVXFloat32Mul, kSSEFloat32Mul);
}

void InstructionSelector::VisitFloat32Div(Node* node) {
  VisitFloatBinop(this, node, kAVXFloat32Div, kSSEFloat32Div);
}

void InstructionSelector::VisitFloat32Abs(Node* node) {
  VisitFloatUnop(this, node, node->InputAt(0), kX64Float32Abs);
}

void InstructionSelector::VisitFloat32Max(Node* node) {
  VisitRRO(this, node, kSSEFloat32Max);
}

void InstructionSelector::VisitFloat32Min(Node* node) {
  VisitRRO(this, node, kSSEFloat32Min);
}

void InstructionSelector::VisitFloat64Add(Node* node) {
  VisitFloatBinop(this, node, kAVXFloat64Add, kSSEFloat64Add);
}

void InstructionSelector::VisitFloat64Sub(Node* node) {
  VisitFloatBinop(this, node, kAVXFloat64Sub, kSSEFloat64Sub);
}

void InstructionSelector::VisitFloat64Mul(Node* node) {
  VisitFloatBinop(this, node, kAVXFloat64Mul, kSSEFloat64Mul);
}

void InstructionSelector::VisitFloat64Div(Node* node) {
  VisitFloatBinop(this, node, kAVXFloat64Div, kSSEFloat64Div);
}

void InstructionSelector::VisitFloat64Mod(Node* node) {
  X64OperandGenerator g(this);
  InstructionOperand temps[] = {g.TempRegister(rax)};
  Emit(kSSEFloat64Mod, g.DefineSameAsFirst(node),
       g.UseRegister(node->InputAt(0)), g.UseRegister(node->InputAt(1)), 1,
       temps);
}

void InstructionSelector::VisitFloat64Max(Node* node) {
  VisitRRO(this, node, kSSEFloat64Max);
}

void InstructionSelector::VisitFloat64Min(Node* node) {
  VisitRRO(this, node, kSSEFloat64Min);
}

void InstructionSelector::VisitFloat64Abs(Node* node) {
  VisitFloatUnop(this, node, node->InputAt(0), kX64Float64Abs);
}

void InstructionSelector::VisitFloat64RoundTiesAway(Node* node) {
  UNREACHABLE();
}

void InstructionSelector::VisitFloat32Neg(Node* node) {
  VisitFloatUnop(this, node, node->InputAt(0), kX64Float32Neg);
}

void InstructionSelector::VisitFloat64Neg(Node* node) {
  VisitFloatUnop(this, node, node->InputAt(0), kX64Float64Neg);
}

void InstructionSelector::VisitFloat64Ieee754Binop(Node* node,
                                                   InstructionCode opcode) {
  X64OperandGenerator g(this);
  Emit(opcode, g.DefineAsFixed(node, xmm0), g.UseFixed(node->InputAt(0), xmm0),
       g.UseFixed(node->InputAt(1), xmm1))
      ->MarkAsCall();
}

void InstructionSelector::VisitFloat64Ieee754Unop(Node* node,
                                                  InstructionCode opcode) {
  X64OperandGenerator g(this);
  Emit(opcode, g.DefineAsFixed(node, xmm0), g.UseFixed(node->InputAt(0), xmm0))
      ->MarkAsCall();
}

void InstructionSelector::EmitMoveParamToFPR(Node* node, int index) {}

void InstructionSelector::EmitMoveFPRToParam(InstructionOperand* op,
                                             LinkageLocation location) {}

void InstructionSelector::EmitPrepareArguments(
    ZoneVector<PushParameter>* arguments, const CallDescriptor* call_descriptor,
    Node* node) {
  X64OperandGenerator g(this);

  // Prepare for C function call.
  if (call_descriptor->IsCFunctionCall()) {
    Emit(kArchPrepareCallCFunction | MiscField::encode(static_cast<int>(
                                         call_descriptor->ParameterCount())),
         0, nullptr, 0, nullptr);

    // Poke any stack arguments.
    for (size_t n = 0; n < arguments->size(); ++n) {
      PushParameter input = (*arguments)[n];
      if (input.node) {
        int slot = static_cast<int>(n);
        InstructionOperand value = g.CanBeImmediate(input.node)
                                       ? g.UseImmediate(input.node)
                                       : g.UseRegister(input.node);
        Emit(kX64Poke | MiscField::encode(slot), g.NoOutput(), value);
      }
    }
  } else {
    // Push any stack arguments.
    int effect_level = GetEffectLevel(node);
    int stack_decrement = 0;
    for (PushParameter input : base::Reversed(*arguments)) {
      stack_decrement += kSystemPointerSize;
      // Skip holes in the param array. These represent both extra slots for
      // multi-slot values and padding slots for alignment.
      if (input.node == nullptr) continue;
      InstructionOperand decrement = g.UseImmediate(stack_decrement);
      stack_decrement = 0;
      if (g.CanBeImmediate(input.node)) {
        Emit(kX64Push, g.NoOutput(), decrement, g.UseImmediate(input.node));
      } else if (IsSupported(INTEL_ATOM) ||
                 sequence()->IsFP(GetVirtualRegister(input.node))) {
        // TODO(titzer): X64Push cannot handle stack->stack double moves
        // because there is no way to encode fixed double slots.
        Emit(kX64Push, g.NoOutput(), decrement, g.UseRegister(input.node));
      } else if (g.CanBeMemoryOperand(kX64Push, node, input.node,
                                      effect_level)) {
        InstructionOperand outputs[1];
        InstructionOperand inputs[5];
        size_t input_count = 0;
        inputs[input_count++] = decrement;
        AddressingMode mode = g.GetEffectiveAddressMemoryOperand(
            input.node, inputs, &input_count);
        InstructionCode opcode = kX64Push | AddressingModeField::encode(mode);
        Emit(opcode, 0, outputs, input_count, inputs);
      } else {
        Emit(kX64Push, g.NoOutput(), decrement, g.UseAny(input.node));
      }
    }
  }
}

void InstructionSelector::EmitPrepareResults(
    ZoneVector<PushParameter>* results, const CallDescriptor* call_descriptor,
    Node* node) {
  X64OperandGenerator g(this);
  for (PushParameter output : *results) {
    if (!output.location.IsCallerFrameSlot()) continue;
    // Skip any alignment holes in nodes.
    if (output.node != nullptr) {
      DCHECK(!call_descriptor->IsCFunctionCall());
      if (output.location.GetType() == MachineType::Float32()) {
        MarkAsFloat32(output.node);
      } else if (output.location.GetType() == MachineType::Float64()) {
        MarkAsFloat64(output.node);
      } else if (output.location.GetType() == MachineType::Simd128()) {
        MarkAsSimd128(output.node);
      }
      InstructionOperand result = g.DefineAsRegister(output.node);
      int offset = call_descriptor->GetOffsetToReturns();
      int reverse_slot = -output.location.GetLocation() - offset;
      InstructionOperand slot = g.UseImmediate(reverse_slot);
      Emit(kX64Peek, 1, &result, 1, &slot);
    }
  }
}

bool InstructionSelector::IsTailCallAddressImmediate() { return true; }

namespace {

void VisitCompareWithMemoryOperand(InstructionSelector* selector,
                                   InstructionCode opcode, Node* left,
                                   InstructionOperand right,
                                   FlagsContinuation* cont) {
  DCHECK(IrOpcode::kLoad == left->opcode() ||
         IrOpcode::kLoadImmutable == left->opcode());
  X64OperandGenerator g(selector);
  size_t input_count = 0;
  InstructionOperand inputs[6];
  AddressingMode addressing_mode =
      g.GetEffectiveAddressMemoryOperand(left, inputs, &input_count);
  opcode |= AddressingModeField::encode(addressing_mode);
  inputs[input_count++] = right;
  if (cont->IsSelect()) {
    if (opcode == kUnorderedEqual) {
      cont->Negate();
      inputs[input_count++] = g.UseRegister(cont->true_value());
      inputs[input_count++] = g.Use(cont->false_value());
    } else {
      inputs[input_count++] = g.UseRegister(cont->false_value());
      inputs[input_count++] = g.Use(cont->true_value());
    }
  }

  selector->EmitWithContinuation(opcode, 0, nullptr, input_count, inputs, cont);
}

// Shared routine for multiple compare operations.
void VisitCompare(InstructionSelector* selector, InstructionCode opcode,
                  InstructionOperand left, InstructionOperand right,
                  FlagsContinuation* cont) {
  if (cont->IsSelect()) {
    X64OperandGenerator g(selector);
    InstructionOperand inputs[4] = {left, right};
    if (cont->condition() == kUnorderedEqual) {
      cont->Negate();
      inputs[2] = g.UseRegister(cont->true_value());
      inputs[3] = g.Use(cont->false_value());
    } else {
      inputs[2] = g.UseRegister(cont->false_value());
      inputs[3] = g.Use(cont->true_value());
    }
    selector->EmitWithContinuation(opcode, 0, nullptr, 4, inputs, cont);
    return;
  }
  selector->EmitWithContinuation(opcode, left, right, cont);
}

// Shared routine for multiple compare operations.
void VisitCompare(InstructionSelector* selector, InstructionCode opcode,
                  Node* left, Node* right, FlagsContinuation* cont,
                  bool commutative) {
  X64OperandGenerator g(selector);
  if (commutative && g.CanBeBetterLeftOperand(right)) {
    std::swap(left, right);
  }
  VisitCompare(selector, opcode, g.UseRegister(left), g.Use(right), cont);
}

MachineType MachineTypeForNarrow(Node* node, Node* hint_node) {
  if (hint_node->opcode() == IrOpcode::kLoad ||
      hint_node->opcode() == IrOpcode::kLoadImmutable) {
    MachineType hint = LoadRepresentationOf(hint_node->op());
    if (node->opcode() == IrOpcode::kInt32Constant ||
        node->opcode() == IrOpcode::kInt64Constant) {
      int64_t constant = node->opcode() == IrOpcode::kInt32Constant
                             ? OpParameter<int32_t>(node->op())
                             : OpParameter<int64_t>(node->op());
      if (hint == MachineType::Int8()) {
        if (constant >= std::numeric_limits<int8_t>::min() &&
            constant <= std::numeric_limits<int8_t>::max()) {
          return hint;
        }
      } else if (hint == MachineType::Uint8()) {
        if (constant >= std::numeric_limits<uint8_t>::min() &&
            constant <= std::numeric_limits<uint8_t>::max()) {
          return hint;
        }
      } else if (hint == MachineType::Int16()) {
        if (constant >= std::numeric_limits<int16_t>::min() &&
            constant <= std::numeric_limits<int16_t>::max()) {
          return hint;
        }
      } else if (hint == MachineType::Uint16()) {
        if (constant >= std::numeric_limits<uint16_t>::min() &&
            constant <= std::numeric_limits<uint16_t>::max()) {
          return hint;
        }
      } else if (hint == MachineType::Int32()) {
        if (constant >= std::numeric_limits<int32_t>::min() &&
            constant <= std::numeric_limits<int32_t>::max()) {
          return hint;
        }
      } else if (hint == MachineType::Uint32()) {
        if (constant >= std::numeric_limits<uint32_t>::min() &&
            constant <= std::numeric_limits<uint32_t>::max())
          return hint;
      }
    }
  }
  return node->opcode() == IrOpcode::kLoad ||
                 node->opcode() == IrOpcode::kLoadImmutable
             ? LoadRepresentationOf(node->op())
             : MachineType::None();
}

bool IsIntConstant(Node* node) {
  return node->opcode() == IrOpcode::kInt32Constant ||
         node->opcode() == IrOpcode::kInt64Constant;
}

bool IsWordAnd(Node* node) {
  return node->opcode() == IrOpcode::kWord32And ||
         node->opcode() == IrOpcode::kWord64And;
}

// The result of WordAnd with a positive interger constant in X64 is known to
// be sign(zero)-extended. Comparing this result with another positive interger
// constant can have narrowed operand.
MachineType MachineTypeForNarrowWordAnd(Node* and_node, Node* constant_node) {
  Node* and_left = and_node->InputAt(0);
  Node* and_right = and_node->InputAt(1);
  Node* and_constant_node = IsIntConstant(and_right)
                                ? and_right
                                : IsIntConstant(and_left) ? and_left : nullptr;

  if (and_constant_node != nullptr) {
    int64_t and_constant =
        and_constant_node->opcode() == IrOpcode::kInt32Constant
            ? OpParameter<int32_t>(and_constant_node->op())
            : OpParameter<int64_t>(and_constant_node->op());
    int64_t cmp_constant = constant_node->opcode() == IrOpcode::kInt32Constant
                               ? OpParameter<int32_t>(constant_node->op())
                               : OpParameter<int64_t>(constant_node->op());
    if (and_constant >= 0 && cmp_constant >= 0) {
      int64_t constant =
          and_constant > cmp_constant ? and_constant : cmp_constant;
      if (constant <= std::numeric_limits<int8_t>::max()) {
        return MachineType::Int8();
      } else if (constant <= std::numeric_limits<uint8_t>::max()) {
        return MachineType::Uint8();
      } else if (constant <= std::numeric_limits<int16_t>::max()) {
        return MachineType::Int16();
      } else if (constant <= std::numeric_limits<uint16_t>::max()) {
        return MachineType::Uint16();
      } else if (constant <= std::numeric_limits<int32_t>::max()) {
        return MachineType::Int32();
      } else if (constant <= std::numeric_limits<uint32_t>::max()) {
        return MachineType::Uint32();
      }
    }
  }

  return MachineType::None();
}

// Tries to match the size of the given opcode to that of the operands, if
// possible.
InstructionCode TryNarrowOpcodeSize(InstructionCode opcode, Node* left,
                                    Node* right, FlagsContinuation* cont) {
  MachineType left_type = MachineType::None();
  MachineType right_type = MachineType::None();
  if (IsWordAnd(left) && IsIntConstant(right)) {
    left_type = MachineTypeForNarrowWordAnd(left, right);
    right_type = left_type;
  } else if (IsWordAnd(right) && IsIntConstant(left)) {
    right_type = MachineTypeForNarrowWordAnd(right, left);
    left_type = right_type;
  } else {
    // TODO(epertoso): we can probably get some size information out phi nodes.
    // If the load representations don't match, both operands will be
    // zero/sign-extended to 32bit.
    left_type = MachineTypeForNarrow(left, right);
    right_type = MachineTypeForNarrow(right, left);
  }
  if (left_type == right_type) {
    switch (left_type.representation()) {
      case MachineRepresentation::kBit:
      case MachineRepresentation::kWord8: {
        if (opcode == kX64Test || opcode == kX64Test32) return kX64Test8;
        if (opcode == kX64Cmp || opcode == kX64Cmp32) {
          if (left_type.semantic() == MachineSemantic::kUint32) {
            cont->OverwriteUnsignedIfSigned();
          } else {
            CHECK_EQ(MachineSemantic::kInt32, left_type.semantic());
          }
          return kX64Cmp8;
        }
        break;
      }
      // Cmp16/Test16 may introduce LCP(Length-Changing-Prefixes) stall, use
      // Cmp32/Test32 instead.
      case MachineRepresentation::kWord16:  // Fall through.
      case MachineRepresentation::kWord32:
        if (opcode == kX64Test) return kX64Test32;
        if (opcode == kX64Cmp) {
          if (left_type.semantic() == MachineSemantic::kUint32) {
            cont->OverwriteUnsignedIfSigned();
          } else {
            CHECK_EQ(MachineSemantic::kInt32, left_type.semantic());
          }
          return kX64Cmp32;
        }
        break;
#ifdef V8_COMPRESS_POINTERS
      case MachineRepresentation::kTaggedSigned:
      case MachineRepresentation::kTaggedPointer:
      case MachineRepresentation::kTagged:
        // When pointer compression is enabled the lower 32-bits uniquely
        // identify tagged value.
        if (opcode == kX64Cmp) return kX64Cmp32;
        break;
#endif
      default:
        break;
    }
  }
  return opcode;
}

/*
Remove unnecessary WordAnd
For example:
33:  IfFalse(31)
517: Int32Constant[65535]
518: Word32And(18, 517)
36:  Int32Constant[266]
37:  Int32LessThanOrEqual(36, 518)
38:  Branch[None]

If Int32LessThanOrEqual select cmp16, the above Word32And can be removed:
33:  IfFalse(31)
36:  Int32Constant[266]
37:  Int32LessThanOrEqual(36, 18)
38:  Branch[None]
*/
void RemoveUnnecessaryWordAnd(InstructionCode opcode, Node** and_node) {
  int64_t mask = 0;

  if (opcode == kX64Cmp32 || opcode == kX64Test32) {
    mask = std::numeric_limits<uint32_t>::max();
  } else if (opcode == kX64Cmp16 || opcode == kX64Test16) {
    mask = std::numeric_limits<uint16_t>::max();
  } else if (opcode == kX64Cmp8 || opcode == kX64Test8) {
    mask = std::numeric_limits<uint8_t>::max();
  } else {
    return;
  }

  Node* and_left = (*and_node)->InputAt(0);
  Node* and_right = (*and_node)->InputAt(1);
  Node* and_constant_node = nullptr;
  Node* and_other_node = nullptr;
  if (IsIntConstant(and_left)) {
    and_constant_node = and_left;
    and_other_node = and_right;
  } else if (IsIntConstant(and_right)) {
    and_constant_node = and_right;
    and_other_node = and_left;
  }

  if (and_constant_node) {
    int64_t and_constant;
    if (and_constant_node->opcode() == IrOpcode::kInt32Constant) {
      and_constant = OpParameter<int32_t>(and_constant_node->op());
    } else if (and_constant_node->opcode() == IrOpcode::kInt64Constant) {
      and_constant = OpParameter<int64_t>(and_constant_node->op());
    } else {
      UNREACHABLE();
    }

    if (and_constant == mask) {
      *and_node = and_other_node;
    }
  }
}

// Shared routine for multiple word compare operations.
void VisitWordCompare(InstructionSelector* selector, Node* node,
                      InstructionCode opcode, FlagsContinuation* cont) {
  X64OperandGenerator g(selector);
  Node* left = node->InputAt(0);
  Node* right = node->InputAt(1);

  // The 32-bit comparisons automatically truncate Word64
  // values to Word32 range, no need to do that explicitly.
  if (opcode == kX64Cmp32 || opcode == kX64Test32) {
    if (left->opcode() == IrOpcode::kTruncateInt64ToInt32) {
      left = left->InputAt(0);
    }

    if (right->opcode() == IrOpcode::kTruncateInt64ToInt32) {
      right = right->InputAt(0);
    }
  }

  opcode = TryNarrowOpcodeSize(opcode, left, right, cont);

  // If one of the two inputs is an immediate, make sure it's on the right, or
  // if one of the two inputs is a memory operand, make sure it's on the left.
  int effect_level = selector->GetEffectLevel(node, cont);

  if ((!g.CanBeImmediate(right) && g.CanBeImmediate(left)) ||
      (g.CanBeMemoryOperand(opcode, node, right, effect_level) &&
       !g.CanBeMemoryOperand(opcode, node, left, effect_level))) {
    if (!node->op()->HasProperty(Operator::kCommutative)) cont->Commute();
    std::swap(left, right);
  }

  if (IsWordAnd(left)) {
    RemoveUnnecessaryWordAnd(opcode, &left);
  }

  // Match immediates on right side of comparison.
  if (g.CanBeImmediate(right)) {
    if (g.CanBeMemoryOperand(opcode, node, left, effect_level)) {
      return VisitCompareWithMemoryOperand(selector, opcode, left,
                                           g.UseImmediate(right), cont);
    }
    return VisitCompare(selector, opcode, g.Use(left), g.UseImmediate(right),
                        cont);
  }

  // Match memory operands on left side of comparison.
  if (g.CanBeMemoryOperand(opcode, node, left, effect_level)) {
    return VisitCompareWithMemoryOperand(selector, opcode, left,
                                         g.UseRegister(right), cont);
  }

  return VisitCompare(selector, opcode, left, right, cont,
                      node->op()->HasProperty(Operator::kCommutative));
}

void VisitWord64EqualImpl(InstructionSelector* selector, Node* node,
                          FlagsContinuation* cont) {
  if (selector->CanUseRootsRegister()) {
    X64OperandGenerator g(selector);
    const RootsTable& roots_table = selector->isolate()->roots_table();
    RootIndex root_index;
    HeapObjectBinopMatcher m(node);
    if (m.right().HasResolvedValue() &&
        roots_table.IsRootHandle(m.right().ResolvedValue(), &root_index)) {
      InstructionCode opcode =
          kX64Cmp | AddressingModeField::encode(kMode_Root);
      return VisitCompare(
          selector, opcode,
          g.TempImmediate(
              MacroAssemblerBase::RootRegisterOffsetForRootIndex(root_index)),
          g.UseRegister(m.left().node()), cont);
    }
  }
  VisitWordCompare(selector, node, kX64Cmp, cont);
}

void VisitWord32EqualImpl(InstructionSelector* selector, Node* node,
                          FlagsContinuation* cont) {
  if (COMPRESS_POINTERS_BOOL && selector->isolate()) {
    X64OperandGenerator g(selector);
    const RootsTable& roots_table = selector->isolate()->roots_table();
    RootIndex root_index;
    Node* left = nullptr;
    Handle<HeapObject> right;
    // HeapConstants and CompressedHeapConstants can be treated the same when
    // using them as an input to a 32-bit comparison. Check whether either is
    // present.
    {
      CompressedHeapObjectBinopMatcher m(node);
      if (m.right().HasResolvedValue()) {
        left = m.left().node();
        right = m.right().ResolvedValue();
      } else {
        HeapObjectBinopMatcher m2(node);
        if (m2.right().HasResolvedValue()) {
          left = m2.left().node();
          right = m2.right().ResolvedValue();
        }
      }
    }
    if (!right.is_null() && roots_table.IsRootHandle(right, &root_index)) {
      DCHECK_NE(left, nullptr);
      if (RootsTable::IsReadOnly(root_index) &&
          (V8_STATIC_ROOTS_BOOL || !selector->isolate()->bootstrapper())) {
        return VisitCompare(selector, kX64Cmp32, g.UseRegister(left),
                            g.TempImmediate(MacroAssemblerBase::ReadOnlyRootPtr(
                                root_index, selector->isolate())),
                            cont);
      }
      if (selector->CanUseRootsRegister()) {
        InstructionCode opcode =
            kX64Cmp32 | AddressingModeField::encode(kMode_Root);
        return VisitCompare(
            selector, opcode,
            g.TempImmediate(
                MacroAssemblerBase::RootRegisterOffsetForRootIndex(root_index)),
            g.UseRegister(left), cont);
      }
    }
  }
  VisitWordCompare(selector, node, kX64Cmp32, cont);
}

// Shared routine for comparison with zero.
void VisitCompareZero(InstructionSelector* selector, Node* user, Node* node,
                      InstructionCode opcode, FlagsContinuation* cont) {
  X64OperandGenerator g(selector);
  if (cont->IsBranch() &&
      (cont->condition() == kNotEqual || cont->condition() == kEqual)) {
    switch (node->opcode()) {
#define FLAGS_SET_BINOP_LIST(V)        \
  V(kInt32Add, VisitBinop, kX64Add32)  \
  V(kInt32Sub, VisitBinop, kX64Sub32)  \
  V(kWord32And, VisitBinop, kX64And32) \
  V(kWord32Or, VisitBinop, kX64Or32)   \
  V(kInt64Add, VisitBinop, kX64Add)    \
  V(kInt64Sub, VisitBinop, kX64Sub)    \
  V(kWord64And, VisitBinop, kX64And)   \
  V(kWord64Or, VisitBinop, kX64Or)
#define FLAGS_SET_BINOP(opcode, Visit, archOpcode)           \
  case IrOpcode::opcode:                                     \
    if (selector->IsOnlyUserOfNodeInSameBlock(user, node)) { \
      return Visit(selector, node, archOpcode, cont);        \
    }                                                        \
    break;
      FLAGS_SET_BINOP_LIST(FLAGS_SET_BINOP)
#undef FLAGS_SET_BINOP_LIST
#undef FLAGS_SET_BINOP

#define TRY_VISIT_WORD32_SHIFT TryVisitWordShift<Int32BinopMatcher, 32>
#define TRY_VISIT_WORD64_SHIFT TryVisitWordShift<Int64BinopMatcher, 64>
// Skip Word64Sar/Word32Sar since no instruction reduction in most cases.
#define FLAGS_SET_SHIFT_LIST(V)                    \
  V(kWord32Shl, TRY_VISIT_WORD32_SHIFT, kX64Shl32) \
  V(kWord32Shr, TRY_VISIT_WORD32_SHIFT, kX64Shr32) \
  V(kWord64Shl, TRY_VISIT_WORD64_SHIFT, kX64Shl)   \
  V(kWord64Shr, TRY_VISIT_WORD64_SHIFT, kX64Shr)
#define FLAGS_SET_SHIFT(opcode, TryVisit, archOpcode)         \
  case IrOpcode::opcode:                                      \
    if (selector->IsOnlyUserOfNodeInSameBlock(user, node)) {  \
      if (TryVisit(selector, node, archOpcode, cont)) return; \
    }                                                         \
    break;
      FLAGS_SET_SHIFT_LIST(FLAGS_SET_SHIFT)
#undef TRY_VISIT_WORD32_SHIFT
#undef TRY_VISIT_WORD64_SHIFT
#undef FLAGS_SET_SHIFT_LIST
#undef FLAGS_SET_SHIFT
      default:
        break;
    }
  }
  int effect_level = selector->GetEffectLevel(node, cont);
  if (node->opcode() == IrOpcode::kLoad ||
      node->opcode() == IrOpcode::kLoadImmutable) {
    switch (LoadRepresentationOf(node->op()).representation()) {
      case MachineRepresentation::kWord8:
        if (opcode == kX64Cmp32) {
          opcode = kX64Cmp8;
        } else if (opcode == kX64Test32) {
          opcode = kX64Test8;
        }
        break;
      case MachineRepresentation::kWord16:
        if (opcode == kX64Cmp32) {
          opcode = kX64Cmp16;
        } else if (opcode == kX64Test32) {
          opcode = kX64Test16;
        }
        break;
      default:
        break;
    }
  }
  if (g.CanBeMemoryOperand(opcode, user, node, effect_level)) {
    VisitCompareWithMemoryOperand(selector, opcode, node, g.TempImmediate(0),
                                  cont);
  } else {
    VisitCompare(selector, opcode, g.Use(node), g.TempImmediate(0), cont);
  }
}

// Shared routine for multiple float32 compare operations (inputs commuted).
void VisitFloat32Compare(InstructionSelector* selector, Node* node,
                         FlagsContinuation* cont) {
  Node* const left = node->InputAt(0);
  Node* const right = node->InputAt(1);
  InstructionCode const opcode =
      selector->IsSupported(AVX) ? kAVXFloat32Cmp : kSSEFloat32Cmp;
  VisitCompare(selector, opcode, right, left, cont, false);
}

// Shared routine for multiple float64 compare operations (inputs commuted).
void VisitFloat64Compare(InstructionSelector* selector, Node* node,
                         FlagsContinuation* cont) {
  Node* const left = node->InputAt(0);
  Node* const right = node->InputAt(1);
  InstructionCode const opcode =
      selector->IsSupported(AVX) ? kAVXFloat64Cmp : kSSEFloat64Cmp;
  VisitCompare(selector, opcode, right, left, cont, false);
}

// Shared routine for Word32/Word64 Atomic Binops
void VisitAtomicBinop(InstructionSelector* selector, Node* node,
                      ArchOpcode opcode, AtomicWidth width,
                      MemoryAccessKind access_kind) {
  X64OperandGenerator g(selector);
  Node* base = node->InputAt(0);
  Node* index = node->InputAt(1);
  Node* value = node->InputAt(2);
  AddressingMode addressing_mode;
  InstructionOperand inputs[] = {
      g.UseUniqueRegister(value), g.UseUniqueRegister(base),
      g.GetEffectiveIndexOperand(index, &addressing_mode)};
  InstructionOperand outputs[] = {g.DefineAsFixed(node, rax)};
  InstructionOperand temps[] = {g.TempRegister()};
  InstructionCode code = opcode | AddressingModeField::encode(addressing_mode) |
                         AtomicWidthField::encode(width);
  if (access_kind == MemoryAccessKind::kProtected) {
    code |= AccessModeField::encode(kMemoryAccessProtectedMemOutOfBounds);
  }
  selector->Emit(code, arraysize(outputs), outputs, arraysize(inputs), inputs,
                 arraysize(temps), temps);
}

// Shared routine for Word32/Word64 Atomic CmpExchg
void VisitAtomicCompareExchange(InstructionSelector* selector, Node* node,
                                ArchOpcode opcode, AtomicWidth width,
                                MemoryAccessKind access_kind) {
  X64OperandGenerator g(selector);
  Node* base = node->InputAt(0);
  Node* index = node->InputAt(1);
  Node* old_value = node->InputAt(2);
  Node* new_value = node->InputAt(3);
  AddressingMode addressing_mode;
  InstructionOperand inputs[] = {
      g.UseFixed(old_value, rax), g.UseUniqueRegister(new_value),
      g.UseUniqueRegister(base),
      g.GetEffectiveIndexOperand(index, &addressing_mode)};
  InstructionOperand outputs[] = {g.DefineAsFixed(node, rax)};
  InstructionCode code = opcode | AddressingModeField::encode(addressing_mode) |
                         AtomicWidthField::encode(width);
  if (access_kind == MemoryAccessKind::kProtected) {
    code |= AccessModeField::encode(kMemoryAccessProtectedMemOutOfBounds);
  }
  selector->Emit(code, arraysize(outputs), outputs, arraysize(inputs), inputs);
}

}  // namespace

// Shared routine for word comparison against zero.
void InstructionSelector::VisitWordCompareZero(Node* user, Node* value,
                                               FlagsContinuation* cont) {
  // Try to combine with comparisons against 0 by simply inverting the branch.
  while (value->opcode() == IrOpcode::kWord32Equal && CanCover(user, value)) {
    Int32BinopMatcher m(value);
    if (!m.right().Is(0)) break;

    user = value;
    value = m.left().node();
    cont->Negate();
  }

  if (CanCover(user, value)) {
    switch (value->opcode()) {
      case IrOpcode::kWord32Equal:
        cont->OverwriteAndNegateIfEqual(kEqual);
        return VisitWord32EqualImpl(this, value, cont);
      case IrOpcode::kInt32LessThan:
        cont->OverwriteAndNegateIfEqual(kSignedLessThan);
        return VisitWordCompare(this, value, kX64Cmp32, cont);
      case IrOpcode::kInt32LessThanOrEqual:
        cont->OverwriteAndNegateIfEqual(kSignedLessThanOrEqual);
        return VisitWordCompare(this, value, kX64Cmp32, cont);
      case IrOpcode::kUint32LessThan:
        cont->OverwriteAndNegateIfEqual(kUnsignedLessThan);
        return VisitWordCompare(this, value, kX64Cmp32, cont);
      case IrOpcode::kUint32LessThanOrEqual:
        cont->OverwriteAndNegateIfEqual(kUnsignedLessThanOrEqual);
        return VisitWordCompare(this, value, kX64Cmp32, cont);
      case IrOpcode::kWord64Equal: {
        cont->OverwriteAndNegateIfEqual(kEqual);
        Int64BinopMatcher m(value);
        if (m.right().Is(0)) {
          // Try to combine the branch with a comparison.
          Node* const eq_user = m.node();
          Node* const eq_value = m.left().node();
          if (CanCover(eq_user, eq_value)) {
            switch (eq_value->opcode()) {
              case IrOpcode::kInt64Sub:
                return VisitWordCompare(this, eq_value, kX64Cmp, cont);
              case IrOpcode::kWord64And:
                return VisitWordCompare(this, eq_value, kX64Test, cont);
              default:
                break;
            }
          }
          return VisitCompareZero(this, eq_user, eq_value, kX64Cmp, cont);
        }
        return VisitWord64EqualImpl(this, value, cont);
      }
      case IrOpcode::kInt64LessThan:
        cont->OverwriteAndNegateIfEqual(kSignedLessThan);
        return VisitWordCompare(this, value, kX64Cmp, cont);
      case IrOpcode::kInt64LessThanOrEqual:
        cont->OverwriteAndNegateIfEqual(kSignedLessThanOrEqual);
        return VisitWordCompare(this, value, kX64Cmp, cont);
      case IrOpcode::kUint64LessThan:
        cont->OverwriteAndNegateIfEqual(kUnsignedLessThan);
        return VisitWordCompare(this, value, kX64Cmp, cont);
      case IrOpcode::kUint64LessThanOrEqual:
        cont->OverwriteAndNegateIfEqual(kUnsignedLessThanOrEqual);
        return VisitWordCompare(this, value, kX64Cmp, cont);
      case IrOpcode::kFloat32Equal:
        cont->OverwriteAndNegateIfEqual(kUnorderedEqual);
        return VisitFloat32Compare(this, value, cont);
      case IrOpcode::kFloat32LessThan:
        cont->OverwriteAndNegateIfEqual(kUnsignedGreaterThan);
        return VisitFloat32Compare(this, value, cont);
      case IrOpcode::kFloat32LessThanOrEqual:
        cont->OverwriteAndNegateIfEqual(kUnsignedGreaterThanOrEqual);
        return VisitFloat32Compare(this, value, cont);
      case IrOpcode::kFloat64Equal:
        cont->OverwriteAndNegateIfEqual(kUnorderedEqual);
        return VisitFloat64Compare(this, value, cont);
      case IrOpcode::kFloat64LessThan: {
        Float64BinopMatcher m(value);
        if (m.left().Is(0.0) && m.right().IsFloat64Abs()) {
          // This matches the pattern
          //
          //   Float64LessThan(#0.0, Float64Abs(x))
          //
          // which TurboFan generates for NumberToBoolean in the general case,
          // and which evaluates to false if x is 0, -0 or NaN. We can compile
          // this to a simple (v)ucomisd using not_equal flags condition, which
          // avoids the costly Float64Abs.
          cont->OverwriteAndNegateIfEqual(kNotEqual);
          InstructionCode const opcode =
              IsSupported(AVX) ? kAVXFloat64Cmp : kSSEFloat64Cmp;
          return VisitCompare(this, opcode, m.left().node(),
                              m.right().InputAt(0), cont, false);
        }
        cont->OverwriteAndNegateIfEqual(kUnsignedGreaterThan);
        return VisitFloat64Compare(this, value, cont);
      }
      case IrOpcode::kFloat64LessThanOrEqual:
        cont->OverwriteAndNegateIfEqual(kUnsignedGreaterThanOrEqual);
        return VisitFloat64Compare(this, value, cont);
      case IrOpcode::kProjection:
        // Check if this is the overflow output projection of an
        // <Operation>WithOverflow node.
        if (ProjectionIndexOf(value->op()) == 1u) {
          // We cannot combine the <Operation>WithOverflow with this branch
          // unless the 0th projection (the use of the actual value of the
          // <Operation> is either nullptr, which means there's no use of the
          // actual value, or was already defined, which means it is scheduled
          // *AFTER* this branch).
          Node* const node = value->InputAt(0);
          Node* const result = NodeProperties::FindProjection(node, 0);
          if (result == nullptr || IsDefined(result)) {
            switch (node->opcode()) {
              case IrOpcode::kInt32AddWithOverflow:
                cont->OverwriteAndNegateIfEqual(kOverflow);
                return VisitBinop(this, node, kX64Add32, cont);
              case IrOpcode::kInt32SubWithOverflow:
                cont->OverwriteAndNegateIfEqual(kOverflow);
                return VisitBinop(this, node, kX64Sub32, cont);
              case IrOpcode::kInt32MulWithOverflow:
                cont->OverwriteAndNegateIfEqual(kOverflow);
                return VisitBinop(this, node, kX64Imul32, cont);
              case IrOpcode::kInt64AddWithOverflow:
                cont->OverwriteAndNegateIfEqual(kOverflow);
                return VisitBinop(this, node, kX64Add, cont);
              case IrOpcode::kInt64SubWithOverflow:
                cont->OverwriteAndNegateIfEqual(kOverflow);
                return VisitBinop(this, node, kX64Sub, cont);
              case IrOpcode::kInt64MulWithOverflow:
                cont->OverwriteAndNegateIfEqual(kOverflow);
                return VisitBinop(this, node, kX64Imul, cont);
              default:
                break;
            }
          }
        }
        break;
      case IrOpcode::kInt32Sub:
        return VisitWordCompare(this, value, kX64Cmp32, cont);
      case IrOpcode::kWord32And:
        return VisitWordCompare(this, value, kX64Test32, cont);
      case IrOpcode::kStackPointerGreaterThan:
        cont->OverwriteAndNegateIfEqual(kStackPointerGreaterThanCondition);
        return VisitStackPointerGreaterThan(value, cont);
      default:
        break;
    }
  }

  // Branch could not be combined with a compare, emit compare against 0.
  VisitCompareZero(this, user, value, kX64Cmp32, cont);
}

void InstructionSelector::VisitSwitch(Node* node, const SwitchInfo& sw) {
  X64OperandGenerator g(this);
  InstructionOperand value_operand = g.UseRegister(node->InputAt(0));

  // Emit either ArchTableSwitch or ArchBinarySearchSwitch.
  if (enable_switch_jump_table_ == kEnableSwitchJumpTable) {
    static const size_t kMaxTableSwitchValueRange = 2 << 16;
    size_t table_space_cost = 4 + sw.value_range();
    size_t table_time_cost = 3;
    size_t lookup_space_cost = 3 + 2 * sw.case_count();
    size_t lookup_time_cost = sw.case_count();
    if (sw.case_count() > 4 &&
        table_space_cost + 3 * table_time_cost <=
            lookup_space_cost + 3 * lookup_time_cost &&
        sw.min_value() > std::numeric_limits<int32_t>::min() &&
        sw.value_range() <= kMaxTableSwitchValueRange) {
      InstructionOperand index_operand = g.TempRegister();
      if (sw.min_value()) {
        // The leal automatically zero extends, so result is a valid 64-bit
        // index.
        Emit(kX64Lea32 | AddressingModeField::encode(kMode_MRI), index_operand,
             value_operand, g.TempImmediate(-sw.min_value()));
      } else {
        // Zero extend, because we use it as 64-bit index into the jump table.
        if (ZeroExtendsWord32ToWord64(node->InputAt(0))) {
          // Input value has already been zero-extended.
          index_operand = value_operand;
        } else {
          Emit(kX64Movl, index_operand, value_operand);
        }
      }
      // Generate a table lookup.
      return EmitTableSwitch(sw, index_operand);
    }
  }

  // Generate a tree of conditional jumps.
  return EmitBinarySearchSwitch(sw, value_operand);
}

void InstructionSelector::VisitWord32Equal(Node* const node) {
  Node* user = node;
  FlagsContinuation cont = FlagsContinuation::ForSet(kEqual, node);
  Int32BinopMatcher m(user);
  if (m.right().Is(0)) {
    return VisitWordCompareZero(m.node(), m.left().node(), &cont);
  }
  VisitWord32EqualImpl(this, node, &cont);
}

void InstructionSelector::VisitInt32LessThan(Node* node) {
  FlagsContinuation cont = FlagsContinuation::ForSet(kSignedLessThan, node);
  VisitWordCompare(this, node, kX64Cmp32, &cont);
}

void InstructionSelector::VisitInt32LessThanOrEqual(Node* node) {
  FlagsContinuation cont =
      FlagsContinuation::ForSet(kSignedLessThanOrEqual, node);
  VisitWordCompare(this, node, kX64Cmp32, &cont);
}

void InstructionSelector::VisitUint32LessThan(Node* node) {
  FlagsContinuation cont = FlagsContinuation::ForSet(kUnsignedLessThan, node);
  VisitWordCompare(this, node, kX64Cmp32, &cont);
}

void InstructionSelector::VisitUint32LessThanOrEqual(Node* node) {
  FlagsContinuation cont =
      FlagsContinuation::ForSet(kUnsignedLessThanOrEqual, node);
  VisitWordCompare(this, node, kX64Cmp32, &cont);
}

void InstructionSelector::VisitWord64Equal(Node* node) {
  FlagsContinuation cont = FlagsContinuation::ForSet(kEqual, node);
  Int64BinopMatcher m(node);
  if (m.right().Is(0)) {
    // Try to combine the equality check with a comparison.
    Node* const user = m.node();
    Node* const value = m.left().node();
    if (CanCover(user, value)) {
      switch (value->opcode()) {
        case IrOpcode::kInt64Sub:
          return VisitWordCompare(this, value, kX64Cmp, &cont);
        case IrOpcode::kWord64And:
          return VisitWordCompare(this, value, kX64Test, &cont);
        default:
          break;
      }
    }
  }
  VisitWord64EqualImpl(this, node, &cont);
}

void InstructionSelector::VisitInt32AddWithOverflow(Node* node) {
  if (Node* ovf = NodeProperties::FindProjection(node, 1)) {
    FlagsContinuation cont = FlagsContinuation::ForSet(kOverflow, ovf);
    return VisitBinop(this, node, kX64Add32, &cont);
  }
  FlagsContinuation cont;
  VisitBinop(this, node, kX64Add32, &cont);
}

void InstructionSelector::VisitInt32SubWithOverflow(Node* node) {
  if (Node* ovf = NodeProperties::FindProjection(node, 1)) {
    FlagsContinuation cont = FlagsContinuation::ForSet(kOverflow, ovf);
    return VisitBinop(this, node, kX64Sub32, &cont);
  }
  FlagsContinuation cont;
  VisitBinop(this, node, kX64Sub32, &cont);
}

void InstructionSelector::VisitInt64LessThan(Node* node) {
  FlagsContinuation cont = FlagsContinuation::ForSet(kSignedLessThan, node);
  VisitWordCompare(this, node, kX64Cmp, &cont);
}

void InstructionSelector::VisitInt64LessThanOrEqual(Node* node) {
  FlagsContinuation cont =
      FlagsContinuation::ForSet(kSignedLessThanOrEqual, node);
  VisitWordCompare(this, node, kX64Cmp, &cont);
}

void InstructionSelector::VisitUint64LessThan(Node* node) {
  FlagsContinuation cont = FlagsContinuation::ForSet(kUnsignedLessThan, node);
  VisitWordCompare(this, node, kX64Cmp, &cont);
}

void InstructionSelector::VisitUint64LessThanOrEqual(Node* node) {
  FlagsContinuation cont =
      FlagsContinuation::ForSet(kUnsignedLessThanOrEqual, node);
  VisitWordCompare(this, node, kX64Cmp, &cont);
}

void InstructionSelector::VisitFloat32Equal(Node* node) {
  FlagsContinuation cont = FlagsContinuation::ForSet(kUnorderedEqual, node);
  VisitFloat32Compare(this, node, &cont);
}

void InstructionSelector::VisitFloat32LessThan(Node* node) {
  FlagsContinuation cont =
      FlagsContinuation::ForSet(kUnsignedGreaterThan, node);
  VisitFloat32Compare(this, node, &cont);
}

void InstructionSelector::VisitFloat32LessThanOrEqual(Node* node) {
  FlagsContinuation cont =
      FlagsContinuation::ForSet(kUnsignedGreaterThanOrEqual, node);
  VisitFloat32Compare(this, node, &cont);
}

void InstructionSelector::VisitFloat64Equal(Node* node) {
  FlagsContinuation cont = FlagsContinuation::ForSet(kUnorderedEqual, node);
  VisitFloat64Compare(this, node, &cont);
}

void InstructionSelector::VisitFloat64LessThan(Node* node) {
  Float64BinopMatcher m(node);
  if (m.left().Is(0.0) && m.right().IsFloat64Abs()) {
    // This matches the pattern
    //
    //   Float64LessThan(#0.0, Float64Abs(x))
    //
    // which TurboFan generates for NumberToBoolean in the general case,
    // and which evaluates to false if x is 0, -0 or NaN. We can compile
    // this to a simple (v)ucomisd using not_equal flags condition, which
    // avoids the costly Float64Abs.
    FlagsContinuation cont = FlagsContinuation::ForSet(kNotEqual, node);
    InstructionCode const opcode =
        IsSupported(AVX) ? kAVXFloat64Cmp : kSSEFloat64Cmp;
    return VisitCompare(this, opcode, m.left().node(), m.right().InputAt(0),
                        &cont, false);
  }
  FlagsContinuation cont =
      FlagsContinuation::ForSet(kUnsignedGreaterThan, node);
  VisitFloat64Compare(this, node, &cont);
}

void InstructionSelector::VisitFloat64LessThanOrEqual(Node* node) {
  FlagsContinuation cont =
      FlagsContinuation::ForSet(kUnsignedGreaterThanOrEqual, node);
  VisitFloat64Compare(this, node, &cont);
}

void InstructionSelector::VisitFloat64InsertLowWord32(Node* node) {
  X64OperandGenerator g(this);
  Node* left = node->InputAt(0);
  Node* right = node->InputAt(1);
  Float64Matcher mleft(left);
  if (mleft.HasResolvedValue() &&
      (base::bit_cast<uint64_t>(mleft.ResolvedValue()) >> 32) == 0u) {
    Emit(kSSEFloat64LoadLowWord32, g.DefineAsRegister(node), g.Use(right));
    return;
  }
  Emit(kSSEFloat64InsertLowWord32, g.DefineSameAsFirst(node),
       g.UseRegister(left), g.Use(right));
}

void InstructionSelector::VisitFloat64InsertHighWord32(Node* node) {
  X64OperandGenerator g(this);
  Node* left = node->InputAt(0);
  Node* right = node->InputAt(1);
  Emit(kSSEFloat64InsertHighWord32, g.DefineSameAsFirst(node),
       g.UseRegister(left), g.Use(right));
}

void InstructionSelector::VisitFloat64SilenceNaN(Node* node) {
  X64OperandGenerator g(this);
  Emit(kSSEFloat64SilenceNaN, g.DefineSameAsFirst(node),
       g.UseRegister(node->InputAt(0)));
}

void InstructionSelector::VisitMemoryBarrier(Node* node) {
  // x64 is no weaker than release-acquire and only needs to emit an instruction
  // for SeqCst memory barriers.
  AtomicMemoryOrder order = OpParameter<AtomicMemoryOrder>(node->op());
  if (order == AtomicMemoryOrder::kSeqCst) {
    X64OperandGenerator g(this);
    Emit(kX64MFence, g.NoOutput());
    return;
  }
  DCHECK_EQ(AtomicMemoryOrder::kAcqRel, order);
}

void InstructionSelector::VisitWord32AtomicLoad(Node* node) {
  AtomicLoadParameters atomic_load_params = AtomicLoadParametersOf(node->op());
  LoadRepresentation load_rep = atomic_load_params.representation();
  DCHECK(IsIntegral(load_rep.representation()) ||
         IsAnyTagged(load_rep.representation()) ||
         (COMPRESS_POINTERS_BOOL &&
          CanBeCompressedPointer(load_rep.representation())));
  DCHECK_NE(load_rep.representation(), MachineRepresentation::kWord64);
  DCHECK(!load_rep.IsMapWord());
  // The memory order is ignored as both acquire and sequentially consistent
  // loads can emit MOV.
  // https://www.cl.cam.ac.uk/~pes20/cpp/cpp0xmappings.html
  VisitLoad(node, node, GetLoadOpcode(load_rep));
}

void InstructionSelector::VisitWord64AtomicLoad(Node* node) {
  AtomicLoadParameters atomic_load_params = AtomicLoadParametersOf(node->op());
  DCHECK(!atomic_load_params.representation().IsMapWord());
  // The memory order is ignored as both acquire and sequentially consistent
  // loads can emit MOV.
  // https://www.cl.cam.ac.uk/~pes20/cpp/cpp0xmappings.html
  VisitLoad(node, node, GetLoadOpcode(atomic_load_params.representation()));
}

void InstructionSelector::VisitWord32AtomicStore(Node* node) {
  AtomicStoreParameters params = AtomicStoreParametersOf(node->op());
  DCHECK_NE(params.representation(), MachineRepresentation::kWord64);
  DCHECK_IMPLIES(CanBeTaggedOrCompressedPointer(params.representation()),
                 kTaggedSize == 4);
  VisitStoreCommon(this, node, params.store_representation(), params.order(),
                   params.kind());
}

void InstructionSelector::VisitWord64AtomicStore(Node* node) {
  AtomicStoreParameters params = AtomicStoreParametersOf(node->op());
  DCHECK_IMPLIES(CanBeTaggedOrCompressedPointer(params.representation()),
                 kTaggedSize == 8);
  VisitStoreCommon(this, node, params.store_representation(), params.order(),
                   params.kind());
}

void InstructionSelector::VisitWord32AtomicExchange(Node* node) {
  AtomicOpParameters params = AtomicOpParametersOf(node->op());
  ArchOpcode opcode;
  if (params.type() == MachineType::Int8()) {
    opcode = kAtomicExchangeInt8;
  } else if (params.type() == MachineType::Uint8()) {
    opcode = kAtomicExchangeUint8;
  } else if (params.type() == MachineType::Int16()) {
    opcode = kAtomicExchangeInt16;
  } else if (params.type() == MachineType::Uint16()) {
    opcode = kAtomicExchangeUint16;
  } else if (params.type() == MachineType::Int32()
    || params.type() == MachineType::Uint32()) {
    opcode = kAtomicExchangeWord32;
  } else {
    UNREACHABLE();
  }
  VisitAtomicExchange(this, node, opcode, AtomicWidth::kWord32, params.kind());
}

void InstructionSelector::VisitWord64AtomicExchange(Node* node) {
  AtomicOpParameters params = AtomicOpParametersOf(node->op());
  ArchOpcode opcode;
  if (params.type() == MachineType::Uint8()) {
    opcode = kAtomicExchangeUint8;
  } else if (params.type() == MachineType::Uint16()) {
    opcode = kAtomicExchangeUint16;
  } else if (params.type() == MachineType::Uint32()) {
    opcode = kAtomicExchangeWord32;
  } else if (params.type() == MachineType::Uint64()) {
    opcode = kX64Word64AtomicExchangeUint64;
  } else {
    UNREACHABLE();
  }
  VisitAtomicExchange(this, node, opcode, AtomicWidth::kWord64, params.kind());
}

void InstructionSelector::VisitWord32AtomicCompareExchange(Node* node) {
  AtomicOpParameters params = AtomicOpParametersOf(node->op());
  ArchOpcode opcode;
  if (params.type() == MachineType::Int8()) {
    opcode = kAtomicCompareExchangeInt8;
  } else if (params.type() == MachineType::Uint8()) {
    opcode = kAtomicCompareExchangeUint8;
  } else if (params.type() == MachineType::Int16()) {
    opcode = kAtomicCompareExchangeInt16;
  } else if (params.type() == MachineType::Uint16()) {
    opcode = kAtomicCompareExchangeUint16;
  } else if (params.type() == MachineType::Int32()
    || params.type() == MachineType::Uint32()) {
    opcode = kAtomicCompareExchangeWord32;
  } else {
    UNREACHABLE();
  }
  VisitAtomicCompareExchange(this, node, opcode, AtomicWidth::kWord32,
                             params.kind());
}

void InstructionSelector::VisitWord64AtomicCompareExchange(Node* node) {
  AtomicOpParameters params = AtomicOpParametersOf(node->op());
  ArchOpcode opcode;
  if (params.type() == MachineType::Uint8()) {
    opcode = kAtomicCompareExchangeUint8;
  } else if (params.type() == MachineType::Uint16()) {
    opcode = kAtomicCompareExchangeUint16;
  } else if (params.type() == MachineType::Uint32()) {
    opcode = kAtomicCompareExchangeWord32;
  } else if (params.type() == MachineType::Uint64()) {
    opcode = kX64Word64AtomicCompareExchangeUint64;
  } else {
    UNREACHABLE();
  }
  VisitAtomicCompareExchange(this, node, opcode, AtomicWidth::kWord64,
                             params.kind());
}

void InstructionSelector::VisitWord32AtomicBinaryOperation(
    Node* node, ArchOpcode int8_op, ArchOpcode uint8_op, ArchOpcode int16_op,
    ArchOpcode uint16_op, ArchOpcode word32_op) {
  AtomicOpParameters params = AtomicOpParametersOf(node->op());
  ArchOpcode opcode;
  if (params.type() == MachineType::Int8()) {
    opcode = int8_op;
  } else if (params.type() == MachineType::Uint8()) {
    opcode = uint8_op;
  } else if (params.type() == MachineType::Int16()) {
    opcode = int16_op;
  } else if (params.type() == MachineType::Uint16()) {
    opcode = uint16_op;
  } else if (params.type() == MachineType::Int32()
    || params.type() == MachineType::Uint32()) {
    opcode = word32_op;
  } else {
    UNREACHABLE();
  }
  VisitAtomicBinop(this, node, opcode, AtomicWidth::kWord32, params.kind());
}

#define VISIT_ATOMIC_BINOP(op)                                           \
  void InstructionSelector::VisitWord32Atomic##op(Node* node) {          \
    VisitWord32AtomicBinaryOperation(                                    \
        node, kAtomic##op##Int8, kAtomic##op##Uint8, kAtomic##op##Int16, \
        kAtomic##op##Uint16, kAtomic##op##Word32);                       \
  }
VISIT_ATOMIC_BINOP(Add)
VISIT_ATOMIC_BINOP(Sub)
VISIT_ATOMIC_BINOP(And)
VISIT_ATOMIC_BINOP(Or)
VISIT_ATOMIC_BINOP(Xor)
#undef VISIT_ATOMIC_BINOP

void InstructionSelector::VisitWord64AtomicBinaryOperation(
    Node* node, ArchOpcode uint8_op, ArchOpcode uint16_op, ArchOpcode uint32_op,
    ArchOpcode word64_op) {
  AtomicOpParameters params = AtomicOpParametersOf(node->op());
  ArchOpcode opcode;
  if (params.type() == MachineType::Uint8()) {
    opcode = uint8_op;
  } else if (params.type() == MachineType::Uint16()) {
    opcode = uint16_op;
  } else if (params.type() == MachineType::Uint32()) {
    opcode = uint32_op;
  } else if (params.type() == MachineType::Uint64()) {
    opcode = word64_op;
  } else {
    UNREACHABLE();
  }
  VisitAtomicBinop(this, node, opcode, AtomicWidth::kWord64, params.kind());
}

#define VISIT_ATOMIC_BINOP(op)                                                 \
  void InstructionSelector::VisitWord64Atomic##op(Node* node) {                \
    VisitWord64AtomicBinaryOperation(node, kAtomic##op##Uint8,                 \
                                     kAtomic##op##Uint16, kAtomic##op##Word32, \
                                     kX64Word64Atomic##op##Uint64);            \
  }
VISIT_ATOMIC_BINOP(Add)
VISIT_ATOMIC_BINOP(Sub)
VISIT_ATOMIC_BINOP(And)
VISIT_ATOMIC_BINOP(Or)
VISIT_ATOMIC_BINOP(Xor)
#undef VISIT_ATOMIC_BINOP

#define SIMD_BINOP_SSE_AVX_LIST(V) \
  V(I64x2ExtMulLowI32x4S)          \
  V(I64x2ExtMulHighI32x4S)         \
  V(I64x2ExtMulLowI32x4U)          \
  V(I64x2ExtMulHighI32x4U)         \
  V(I32x4DotI16x8S)                \
  V(I32x4ExtMulLowI16x8S)          \
  V(I32x4ExtMulHighI16x8S)         \
  V(I32x4ExtMulLowI16x8U)          \
  V(I32x4ExtMulHighI16x8U)         \
  V(I16x8SConvertI32x4)            \
  V(I16x8UConvertI32x4)            \
  V(I16x8RoundingAverageU)         \
  V(I16x8ExtMulLowI8x16S)          \
  V(I16x8ExtMulHighI8x16S)         \
  V(I16x8ExtMulLowI8x16U)          \
  V(I16x8ExtMulHighI8x16U)         \
  V(I16x8Q15MulRSatS)              \
  V(I16x8RelaxedQ15MulRS)          \
  V(I8x16SConvertI16x8)            \
  V(I8x16UConvertI16x8)            \
  V(I8x16RoundingAverageU)         \
  V(S128And)                       \
  V(S128Or)                        \
  V(S128Xor)

#define SIMD_BINOP_SSE_AVX_LANE_SIZE_VECTOR_LENGTH_LIST(V) \
  V(F64x2Add, FAdd, kL64, kV128)                           \
  V(F64x4Add, FAdd, kL64, kV256)                           \
  V(F32x4Add, FAdd, kL32, kV128)                           \
  V(F32x8Add, FAdd, kL32, kV256)                           \
  V(I64x2Add, IAdd, kL64, kV128)                           \
  V(I64x4Add, IAdd, kL64, kV256)                           \
  V(I32x8Add, IAdd, kL32, kV256)                           \
  V(I16x16Add, IAdd, kL16, kV256)                          \
  V(I8x32Add, IAdd, kL8, kV256)                            \
  V(I32x4Add, IAdd, kL32, kV128)                           \
  V(I16x8Add, IAdd, kL16, kV128)                           \
  V(I8x16Add, IAdd, kL8, kV128)                            \
  V(F64x4Sub, FSub, kL64, kV256)                           \
  V(F64x2Sub, FSub, kL64, kV128)                           \
  V(F32x4Sub, FSub, kL32, kV128)                           \
  V(F32x8Sub, FSub, kL32, kV256)                           \
  V(I64x2Sub, ISub, kL64, kV128)                           \
  V(I64x4Sub, ISub, kL64, kV256)                           \
  V(I32x8Sub, ISub, kL32, kV256)                           \
  V(I16x16Sub, ISub, kL16, kV256)                          \
  V(I8x32Sub, ISub, kL8, kV256)                            \
  V(I32x4Sub, ISub, kL32, kV128)                           \
  V(I16x8Sub, ISub, kL16, kV128)                           \
  V(I8x16Sub, ISub, kL8, kV128)                            \
  V(F64x2Mul, FMul, kL64, kV128)                           \
  V(F32x4Mul, FMul, kL32, kV128)                           \
  V(F64x4Mul, FMul, kL64, kV256)                           \
  V(F32x8Mul, FMul, kL32, kV256)                           \
  V(I32x8Mul, IMul, kL32, kV256)                           \
  V(I16x16Mul, IMul, kL16, kV256)                          \
  V(I32x4Mul, IMul, kL32, kV128)                           \
  V(I16x8Mul, IMul, kL16, kV128)                           \
  V(F64x2Div, FDiv, kL64, kV128)                           \
  V(F32x4Div, FDiv, kL32, kV128)                           \
  V(F64x4Div, FDiv, kL64, kV256)                           \
  V(F32x8Div, FDiv, kL32, kV256)                           \
  V(I16x8AddSatS, IAddSatS, kL16, kV128)                   \
  V(I16x16AddSatS, IAddSatS, kL16, kV256)                  \
  V(I8x16AddSatS, IAddSatS, kL8, kV128)                    \
  V(I8x32AddSatS, IAddSatS, kL8, kV256)                    \
  V(I16x8SubSatS, ISubSatS, kL16, kV128)                   \
  V(I16x16SubSatS, ISubSatS, kL16, kV256)                  \
  V(I8x16SubSatS, ISubSatS, kL8, kV128)                    \
  V(I8x32SubSatS, ISubSatS, kL8, kV256)                    \
  V(I16x8AddSatU, IAddSatU, kL16, kV128)                   \
  V(I16x16AddSatU, IAddSatU, kL16, kV256)                  \
  V(I8x16AddSatU, IAddSatU, kL8, kV128)                    \
  V(I8x32AddSatU, IAddSatU, kL8, kV256)                    \
  V(I16x8SubSatU, ISubSatU, kL16, kV128)                   \
  V(I16x16SubSatU, ISubSatU, kL16, kV256)                  \
  V(I8x16SubSatU, ISubSatU, kL8, kV128)                    \
  V(I8x32SubSatU, ISubSatU, kL8, kV256)                    \
  V(F64x2Eq, FEq, kL64, kV128)                             \
  V(F32x4Eq, FEq, kL32, kV128)                             \
  V(I64x2Eq, IEq, kL64, kV128)                             \
  V(I32x4Eq, IEq, kL32, kV128)                             \
  V(I16x8Eq, IEq, kL16, kV128)                             \
  V(I8x16Eq, IEq, kL8, kV128)                              \
  V(F64x2Ne, FNe, kL64, kV128)                             \
  V(F32x4Ne, FNe, kL32, kV128)                             \
  V(I32x4GtS, IGtS, kL32, kV128)                           \
  V(I16x8GtS, IGtS, kL16, kV128)                           \
  V(I8x16GtS, IGtS, kL8, kV128)                            \
  V(F64x2Lt, FLt, kL64, kV128)                             \
  V(F32x4Lt, FLt, kL32, kV128)                             \
  V(F64x2Le, FLe, kL64, kV128)                             \
  V(F32x4Le, FLe, kL32, kV128)                             \
  V(I32x4MinS, IMinS, kL32, kV128)                         \
  V(I16x8MinS, IMinS, kL16, kV128)                         \
  V(I8x16MinS, IMinS, kL8, kV128)                          \
  V(I32x4MinU, IMinU, kL32, kV128)                         \
  V(I16x8MinU, IMinU, kL16, kV128)                         \
  V(I8x16MinU, IMinU, kL8, kV128)                          \
  V(I32x4MaxS, IMaxS, kL32, kV128)                         \
  V(I16x8MaxS, IMaxS, kL16, kV128)                         \
  V(I8x16MaxS, IMaxS, kL8, kV128)                          \
  V(I32x4MaxU, IMaxU, kL32, kV128)                         \
  V(I16x8MaxU, IMaxU, kL16, kV128)                         \
  V(I8x16MaxU, IMaxU, kL8, kV128)

#define SIMD_BINOP_LANE_SIZE_VECTOR_LENGTH_LIST(V) \
  V(F64x2Min, FMin, kL64, kV128)                   \
  V(F32x4Min, FMin, kL32, kV128)                   \
  V(F64x2Max, FMax, kL64, kV128)                   \
  V(F32x4Max, FMax, kL32, kV128)                   \
  V(I64x2Ne, INe, kL64, kV128)                     \
  V(I32x4Ne, INe, kL32, kV128)                     \
  V(I16x8Ne, INe, kL16, kV128)                     \
  V(I8x16Ne, INe, kL8, kV128)                      \
  V(I32x4GtU, IGtU, kL32, kV128)                   \
  V(I16x8GtU, IGtU, kL16, kV128)                   \
  V(I8x16GtU, IGtU, kL8, kV128)                    \
  V(I32x4GeS, IGeS, kL32, kV128)                   \
  V(I16x8GeS, IGeS, kL16, kV128)                   \
  V(I8x16GeS, IGeS, kL8, kV128)                    \
  V(I32x4GeU, IGeU, kL32, kV128)                   \
  V(I16x8GeU, IGeU, kL16, kV128)                   \
  V(I8x16GeU, IGeU, kL8, kV128)

#define SIMD_UNOP_LIST(V)   \
  V(F64x2ConvertLowI32x4S)  \
  V(F32x4SConvertI32x4)     \
  V(F32x4DemoteF64x2Zero)   \
  V(I64x2SConvertI32x4Low)  \
  V(I64x2SConvertI32x4High) \
  V(I64x2UConvertI32x4Low)  \
  V(I64x2UConvertI32x4High) \
  V(I32x4SConvertI16x8Low)  \
  V(I32x4SConvertI16x8High) \
  V(I32x4UConvertI16x8Low)  \
  V(I32x4UConvertI16x8High) \
  V(I16x8SConvertI8x16Low)  \
  V(I16x8SConvertI8x16High) \
  V(I16x8UConvertI8x16Low)  \
  V(I16x8UConvertI8x16High) \
  V(S128Not)

#define SIMD_UNOP_LANE_SIZE_VECTOR_LENGTH_LIST(V) \
  V(F32x4Abs, FAbs, kL32, kV128)                  \
  V(I32x4Abs, IAbs, kL32, kV128)                  \
  V(I16x8Abs, IAbs, kL16, kV128)                  \
  V(I8x16Abs, IAbs, kL8, kV128)                   \
  V(F32x4Neg, FNeg, kL32, kV128)                  \
  V(I32x4Neg, INeg, kL32, kV128)                  \
  V(I16x8Neg, INeg, kL16, kV128)                  \
  V(I8x16Neg, INeg, kL8, kV128)                   \
  V(F64x2Sqrt, FSqrt, kL64, kV128)                \
  V(F32x4Sqrt, FSqrt, kL32, kV128)                \
  V(I64x2BitMask, IBitMask, kL64, kV128)          \
  V(I32x4BitMask, IBitMask, kL32, kV128)          \
  V(I16x8BitMask, IBitMask, kL16, kV128)          \
  V(I8x16BitMask, IBitMask, kL8, kV128)           \
  V(I64x2AllTrue, IAllTrue, kL64, kV128)          \
  V(I32x4AllTrue, IAllTrue, kL32, kV128)          \
  V(I16x8AllTrue, IAllTrue, kL16, kV128)          \
  V(I8x16AllTrue, IAllTrue, kL8, kV128)

#define SIMD_SHIFT_LANE_SIZE_VECTOR_LENGTH_OPCODES(V) \
  V(I64x2Shl, IShl, kL64, kV128)                      \
  V(I32x4Shl, IShl, kL32, kV128)                      \
  V(I16x8Shl, IShl, kL16, kV128)                      \
  V(I32x4ShrS, IShrS, kL32, kV128)                    \
  V(I16x8ShrS, IShrS, kL16, kV128)                    \
  V(I64x2ShrU, IShrU, kL64, kV128)                    \
  V(I32x4ShrU, IShrU, kL32, kV128)                    \
  V(I16x8ShrU, IShrU, kL16, kV128)

#define SIMD_NARROW_SHIFT_LANE_SIZE_VECTOR_LENGTH_OPCODES(V) \
  V(I8x16Shl, IShl, kL8, kV128)                              \
  V(I8x16ShrS, IShrS, kL8, kV128)                            \
  V(I8x16ShrU, IShrU, kL8, kV128)

void InstructionSelector::VisitS128Const(Node* node) {
  X64OperandGenerator g(this);
  static const int kUint32Immediates = kSimd128Size / sizeof(uint32_t);
  uint32_t val[kUint32Immediates];
  memcpy(val, S128ImmediateParameterOf(node->op()).data(), kSimd128Size);
  // If all bytes are zeros or ones, avoid emitting code for generic constants
  bool all_zeros = !(val[0] || val[1] || val[2] || val[3]);
  bool all_ones = val[0] == UINT32_MAX && val[1] == UINT32_MAX &&
                  val[2] == UINT32_MAX && val[3] == UINT32_MAX;
  InstructionOperand dst = g.DefineAsRegister(node);
  if (all_zeros) {
    Emit(kX64S128Zero, dst);
  } else if (all_ones) {
    Emit(kX64S128AllOnes, dst);
  } else {
    Emit(kX64S128Const, dst, g.UseImmediate(val[0]), g.UseImmediate(val[1]),
         g.UseImmediate(val[2]), g.UseImmediate(val[3]));
  }
}

void InstructionSelector::VisitS128Zero(Node* node) {
  X64OperandGenerator g(this);
  Emit(kX64S128Zero, g.DefineAsRegister(node));
}

// Name, LaneSize, VectorLength
#define SIMD_INT_TYPES_FOR_SPLAT(V) \
  V(I64x2, kL64, kV128)             \
  V(I32x4, kL32, kV128)             \
  V(I16x8, kL16, kV128)             \
  V(I8x16, kL8, kV128)

// Splat with an optimization for const 0.
#define VISIT_INT_SIMD_SPLAT(Type, LaneSize, VectorLength)                   \
  void InstructionSelector::Visit##Type##Splat(Node* node) {                 \
    X64OperandGenerator g(this);                                             \
    Node* input = node->InputAt(0);                                          \
    if (g.CanBeImmediate(input) && g.GetImmediateIntegerValue(input) == 0) { \
      Emit(kX64S128Zero, g.DefineAsRegister(node));                          \
    } else {                                                                 \
      Emit(kX64ISplat | LaneSizeField::encode(LaneSize) |                    \
               VectorLengthField::encode(VectorLength),                      \
           g.DefineAsRegister(node), g.Use(input));                          \
    }                                                                        \
  }
SIMD_INT_TYPES_FOR_SPLAT(VISIT_INT_SIMD_SPLAT)
#undef VISIT_INT_SIMD_SPLAT
#undef SIMD_INT_TYPES_FOR_SPLAT

void InstructionSelector::VisitF64x2Splat(Node* node) {
  X64OperandGenerator g(this);
  Emit(kX64FSplat | LaneSizeField::encode(kL64) |
           VectorLengthField::encode(kV128),
       g.DefineAsRegister(node), g.Use(node->InputAt(0)));
}

void InstructionSelector::VisitF32x4Splat(Node* node) {
  X64OperandGenerator g(this);
  Emit(kX64FSplat | LaneSizeField::encode(kL32) |
           VectorLengthField::encode(kV128),
       g.DefineAsRegister(node), g.UseRegister(node->InputAt(0)));
}

#define SIMD_VISIT_EXTRACT_LANE(IF, Type, Sign, LaneSize, VectorLength)  \
  void InstructionSelector::Visit##Type##ExtractLane##Sign(Node* node) { \
    X64OperandGenerator g(this);                                         \
    int32_t lane = OpParameter<int32_t>(node->op());                     \
    Emit(kX64##IF##ExtractLane##Sign | LaneSizeField::encode(LaneSize) | \
             VectorLengthField::encode(VectorLength),                    \
         g.DefineAsRegister(node), g.UseRegister(node->InputAt(0)),      \
         g.UseImmediate(lane));                                          \
  }
SIMD_VISIT_EXTRACT_LANE(F, F64x2, , kL64, kV128)
SIMD_VISIT_EXTRACT_LANE(F, F32x4, , kL32, kV128)
SIMD_VISIT_EXTRACT_LANE(I, I64x2, , kL64, kV128)
SIMD_VISIT_EXTRACT_LANE(I, I32x4, , kL32, kV128)
SIMD_VISIT_EXTRACT_LANE(I, I16x8, S, kL16, kV128)
SIMD_VISIT_EXTRACT_LANE(I, I8x16, S, kL8, kV128)
#undef SIMD_VISIT_EXTRACT_LANE

void InstructionSelector::VisitI16x8ExtractLaneU(Node* node) {
  X64OperandGenerator g(this);
  int32_t lane = OpParameter<int32_t>(node->op());
  Emit(kX64Pextrw, g.DefineAsRegister(node), g.UseRegister(node->InputAt(0)),
       g.UseImmediate(lane));
}

void InstructionSelector::VisitI8x16ExtractLaneU(Node* node) {
  X64OperandGenerator g(this);
  int32_t lane = OpParameter<int32_t>(node->op());
  Emit(kX64Pextrb, g.DefineAsRegister(node), g.UseRegister(node->InputAt(0)),
       g.UseImmediate(lane));
}

void InstructionSelector::VisitF32x4ReplaceLane(Node* node) {
  X64OperandGenerator g(this);
  int32_t lane = OpParameter<int32_t>(node->op());
  Emit(kX64FReplaceLane | LaneSizeField::encode(kL32) |
           VectorLengthField::encode(kV128),
       g.DefineSameAsFirst(node), g.UseRegister(node->InputAt(0)),
       g.UseImmediate(lane), g.Use(node->InputAt(1)));
}

void InstructionSelector::VisitF64x2ReplaceLane(Node* node) {
  X64OperandGenerator g(this);
  int32_t lane = OpParameter<int32_t>(node->op());
  // When no-AVX, define dst == src to save a move.
  InstructionOperand dst =
      IsSupported(AVX) ? g.DefineAsRegister(node) : g.DefineSameAsFirst(node);
  Emit(kX64FReplaceLane | LaneSizeField::encode(kL64) |
           VectorLengthField::encode(kV128),
       dst, g.UseRegister(node->InputAt(0)), g.UseImmediate(lane),
       g.UseRegister(node->InputAt(1)));
}

#define VISIT_SIMD_REPLACE_LANE(TYPE, OPCODE)                               \
  void InstructionSelector::Visit##TYPE##ReplaceLane(Node* node) {          \
    X64OperandGenerator g(this);                                            \
    int32_t lane = OpParameter<int32_t>(node->op());                        \
    Emit(OPCODE, g.DefineAsRegister(node), g.UseRegister(node->InputAt(0)), \
         g.UseImmediate(lane), g.Use(node->InputAt(1)));                    \
  }

#define SIMD_TYPES_FOR_REPLACE_LANE(V) \
  V(I64x2, kX64Pinsrq)                 \
  V(I32x4, kX64Pinsrd)                 \
  V(I16x8, kX64Pinsrw)                 \
  V(I8x16, kX64Pinsrb)

SIMD_TYPES_FOR_REPLACE_LANE(VISIT_SIMD_REPLACE_LANE)
#undef SIMD_TYPES_FOR_REPLACE_LANE
#undef VISIT_SIMD_REPLACE_LANE

#define VISIT_SIMD_SHIFT_LANE_SIZE_VECTOR_LENGTH_OPCODES(                  \
    Name, Opcode, LaneSize, VectorLength)                                  \
  void InstructionSelector::Visit##Name(Node* node) {                      \
    X64OperandGenerator g(this);                                           \
    InstructionOperand dst = IsSupported(AVX) ? g.DefineAsRegister(node)   \
                                              : g.DefineSameAsFirst(node); \
    if (g.CanBeImmediate(node->InputAt(1))) {                              \
      Emit(kX64##Opcode | LaneSizeField::encode(LaneSize) |                \
               VectorLengthField::encode(VectorLength),                    \
           dst, g.UseRegister(node->InputAt(0)),                           \
           g.UseImmediate(node->InputAt(1)));                              \
    } else {                                                               \
      Emit(kX64##Opcode | LaneSizeField::encode(LaneSize) |                \
               VectorLengthField::encode(VectorLength),                    \
           dst, g.UseRegister(node->InputAt(0)),                           \
           g.UseRegister(node->InputAt(1)));                               \
    }                                                                      \
  }
SIMD_SHIFT_LANE_SIZE_VECTOR_LENGTH_OPCODES(
    VISIT_SIMD_SHIFT_LANE_SIZE_VECTOR_LENGTH_OPCODES)

#undef VISIT_SIMD_SHIFT_LANE_SIZE_VECTOR_LENGTH_OPCODES
#undef SIMD_SHIFT_LANE_SIZE_VECTOR_LENGTH_OPCODES

#define VISIT_SIMD_NARROW_SHIFT_LANE_SIZE_VECTOR_LENGTH_OPCODES(            \
    Name, Opcode, LaneSize, VectorLength)                                   \
  void InstructionSelector::Visit##Name(Node* node) {                       \
    X64OperandGenerator g(this);                                            \
    InstructionOperand output =                                             \
        IsSupported(AVX) ? g.UseRegister(node) : g.DefineSameAsFirst(node); \
    if (g.CanBeImmediate(node->InputAt(1))) {                               \
      Emit(kX64##Opcode | LaneSizeField::encode(LaneSize) |                 \
               VectorLengthField::encode(VectorLength),                     \
           output, g.UseRegister(node->InputAt(0)),                         \
           g.UseImmediate(node->InputAt(1)));                               \
    } else {                                                                \
      InstructionOperand temps[] = {g.TempSimd128Register()};               \
      Emit(kX64##Opcode | LaneSizeField::encode(LaneSize) |                 \
               VectorLengthField::encode(VectorLength),                     \
           output, g.UseUniqueRegister(node->InputAt(0)),                   \
           g.UseUniqueRegister(node->InputAt(1)), arraysize(temps), temps); \
    }                                                                       \
  }
SIMD_NARROW_SHIFT_LANE_SIZE_VECTOR_LENGTH_OPCODES(
    VISIT_SIMD_NARROW_SHIFT_LANE_SIZE_VECTOR_LENGTH_OPCODES)
#undef VISIT_SIMD_NARROW_SHIFT_LANE_SIZE_VECTOR_LENGTH_OPCODES
#undef SIMD_NARROW_SHIFT_LANE_SIZE_VECTOR_LENGTH_OPCODES

#define VISIT_SIMD_UNOP(Opcode)                         \
  void InstructionSelector::Visit##Opcode(Node* node) { \
    X64OperandGenerator g(this);                        \
    Emit(kX64##Opcode, g.DefineAsRegister(node),        \
         g.UseRegister(node->InputAt(0)));              \
  }
SIMD_UNOP_LIST(VISIT_SIMD_UNOP)
#undef VISIT_SIMD_UNOP
#undef SIMD_UNOP_LIST

#define VISIT_SIMD_UNOP_LANE_SIZE_VECTOR_LENGTH(Name, Opcode, LaneSize, \
                                                VectorLength)           \
  void InstructionSelector::Visit##Name(Node* node) {                   \
    X64OperandGenerator g(this);                                        \
    Emit(kX64##Opcode | LaneSizeField::encode(LaneSize) |               \
             VectorLengthField::encode(VectorLength),                   \
         g.DefineAsRegister(node), g.UseRegister(node->InputAt(0)));    \
  }

SIMD_UNOP_LANE_SIZE_VECTOR_LENGTH_LIST(VISIT_SIMD_UNOP_LANE_SIZE_VECTOR_LENGTH)

#undef VISIT_SIMD_UNOP_LANE_SIZE_VECTOR_LENGTH
#undef SIMD_UNOP_LANE_SIZE_VECTOR_LENGTH_LIST

#define VISIT_SIMD_BINOP_LANE_SIZE_VECTOR_LENGTH(Name, Opcode, LaneSize, \
                                                 VectorLength)           \
  void InstructionSelector::Visit##Name(Node* node) {                    \
    X64OperandGenerator g(this);                                         \
    Emit(kX64##Opcode | LaneSizeField::encode(LaneSize) |                \
             VectorLengthField::encode(VectorLength),                    \
         g.DefineSameAsFirst(node), g.UseRegister(node->InputAt(0)),     \
         g.UseRegister(node->InputAt(1)));                               \
  }

SIMD_BINOP_LANE_SIZE_VECTOR_LENGTH_LIST(
    VISIT_SIMD_BINOP_LANE_SIZE_VECTOR_LENGTH)

#undef VISIT_SIMD_BINOP_LANE_SIZE_VECTOR_LENGTH
#undef SIMD_BINOP_LANE_SIZE_VECTOR_LENGTH_LIST

#define VISIT_SIMD_BINOP(Opcode)                                              \
  void InstructionSelector::Visit##Opcode(Node* node) {                       \
    X64OperandGenerator g(this);                                              \
    if (IsSupported(AVX)) {                                                   \
      Emit(kX64##Opcode, g.DefineAsRegister(node),                            \
           g.UseRegister(node->InputAt(0)), g.UseRegister(node->InputAt(1))); \
    } else {                                                                  \
      Emit(kX64##Opcode, g.DefineSameAsFirst(node),                           \
           g.UseRegister(node->InputAt(0)), g.UseRegister(node->InputAt(1))); \
    }                                                                         \
  }

SIMD_BINOP_SSE_AVX_LIST(VISIT_SIMD_BINOP)
#undef VISIT_SIMD_BINOP
#undef SIMD_BINOP_SSE_AVX_LIST

#define VISIT_SIMD_BINOP_LANE_SIZE_VECTOR_LENGTH(Name, Opcode, LaneSize, \
                                                 VectorLength)           \
  void InstructionSelector::Visit##Name(Node* node) {                    \
    X64OperandGenerator g(this);                                         \
    if (IsSupported(AVX)) {                                              \
      Emit(kX64##Opcode | LaneSizeField::encode(LaneSize) |              \
               VectorLengthField::encode(VectorLength),                  \
           g.DefineAsRegister(node), g.UseRegister(node->InputAt(0)),    \
           g.UseRegister(node->InputAt(1)));                             \
    } else {                                                             \
      Emit(kX64##Opcode | LaneSizeField::encode(LaneSize) |              \
               VectorLengthField::encode(VectorLength),                  \
           g.DefineSameAsFirst(node), g.UseRegister(node->InputAt(0)),   \
           g.UseRegister(node->InputAt(1)));                             \
    }                                                                    \
  }

SIMD_BINOP_SSE_AVX_LANE_SIZE_VECTOR_LENGTH_LIST(
    VISIT_SIMD_BINOP_LANE_SIZE_VECTOR_LENGTH)
#undef VISIT_SIMD_BINOP_LANE_SIZE_VECTOR_LENGTH
#undef SIMD_BINOP_SSE_AVX_LANE_SIZE_VECTOR_LENGTH_LIST

void InstructionSelector::VisitV128AnyTrue(Node* node) {
  X64OperandGenerator g(this);
  Emit(kX64V128AnyTrue, g.DefineAsRegister(node),
       g.UseUniqueRegister(node->InputAt(0)));
}

void InstructionSelector::VisitS128Select(Node* node) {
  X64OperandGenerator g(this);
  InstructionOperand dst =
      IsSupported(AVX) ? g.DefineAsRegister(node) : g.DefineSameAsFirst(node);
  Emit(kX64S128Select, dst, g.UseRegister(node->InputAt(0)),
       g.UseRegister(node->InputAt(1)), g.UseRegister(node->InputAt(2)));
}

void InstructionSelector::VisitS128AndNot(Node* node) {
  X64OperandGenerator g(this);
  // andnps a b does ~a & b, but we want a & !b, so flip the input.
  Emit(kX64S128AndNot, g.DefineSameAsFirst(node),
       g.UseRegister(node->InputAt(1)), g.UseRegister(node->InputAt(0)));
}

void InstructionSelector::VisitF64x2Abs(Node* node) {
  VisitFloatUnop(this, node, node->InputAt(0),
                 kX64FAbs | LaneSizeField::encode(kL64) |
                     VectorLengthField::encode(kV128));
}

void InstructionSelector::VisitF64x2Neg(Node* node) {
  VisitFloatUnop(this, node, node->InputAt(0),
                 kX64FNeg | LaneSizeField::encode(kL64) |
                     VectorLengthField::encode(kV128));
}

void InstructionSelector::VisitF32x4UConvertI32x4(Node* node) {
  X64OperandGenerator g(this);
  Emit(kX64F32x4UConvertI32x4, g.DefineSameAsFirst(node),
       g.UseRegister(node->InputAt(0)));
}

#define VISIT_SIMD_QFMOP(Opcode)                                             \
  void InstructionSelector::Visit##Opcode(Node* node) {                      \
    X64OperandGenerator g(this);                                             \
    Emit(kX64##Opcode, g.UseRegister(node), g.UseRegister(node->InputAt(0)), \
         g.UseRegister(node->InputAt(1)), g.UseRegister(node->InputAt(2)));  \
  }
VISIT_SIMD_QFMOP(F64x2Qfma)
VISIT_SIMD_QFMOP(F64x2Qfms)
VISIT_SIMD_QFMOP(F32x4Qfma)
VISIT_SIMD_QFMOP(F32x4Qfms)
#undef VISIT_SIMD_QFMOP

void InstructionSelector::VisitI64x2Neg(Node* node) {
  X64OperandGenerator g(this);
  // If AVX unsupported, make sure dst != src to avoid a move.
  InstructionOperand operand0 = IsSupported(AVX)
                                    ? g.UseRegister(node->InputAt(0))
                                    : g.UseUnique(node->InputAt(0));
  Emit(
      kX64INeg | LaneSizeField::encode(kL64) | VectorLengthField::encode(kV128),
      g.DefineAsRegister(node), operand0);
}

void InstructionSelector::VisitI64x2ShrS(Node* node) {
  X64OperandGenerator g(this);
  InstructionOperand dst =
      IsSupported(AVX) ? g.DefineAsRegister(node) : g.DefineSameAsFirst(node);

  if (g.CanBeImmediate(node->InputAt(1))) {
    Emit(kX64IShrS | LaneSizeField::encode(kL64) |
             VectorLengthField::encode(kV128),
         dst, g.UseRegister(node->InputAt(0)),
         g.UseImmediate(node->InputAt(1)));
  } else {
    InstructionOperand temps[] = {g.TempSimd128Register()};
    Emit(kX64IShrS | LaneSizeField::encode(kL64) |
             VectorLengthField::encode(kV128),
         dst, g.UseUniqueRegister(node->InputAt(0)),
         g.UseRegister(node->InputAt(1)), arraysize(temps), temps);
  }
}

void InstructionSelector::VisitI64x2Mul(Node* node) {
  X64OperandGenerator g(this);
  InstructionOperand temps[] = {g.TempSimd128Register()};
  Emit(
      kX64IMul | LaneSizeField::encode(kL64) | VectorLengthField::encode(kV128),
      g.DefineAsRegister(node), g.UseUniqueRegister(node->InputAt(0)),
      g.UseUniqueRegister(node->InputAt(1)), arraysize(temps), temps);
}

void InstructionSelector::VisitI64x4Mul(Node* node) {
  X64OperandGenerator g(this);
  InstructionOperand temps[] = {g.TempSimd256Register()};
  Emit(
      kX64IMul | LaneSizeField::encode(kL64) | VectorLengthField::encode(kV256),
      g.DefineAsRegister(node), g.UseUniqueRegister(node->InputAt(0)),
      g.UseUniqueRegister(node->InputAt(1)), arraysize(temps), temps);
}

void InstructionSelector::VisitI32x4SConvertF32x4(Node* node) {
  X64OperandGenerator g(this);
  Emit(kX64I32x4SConvertF32x4,
       IsSupported(AVX) ? g.DefineAsRegister(node) : g.DefineSameAsFirst(node),
       g.UseRegister(node->InputAt(0)));
}

void InstructionSelector::VisitI32x4UConvertF32x4(Node* node) {
  X64OperandGenerator g(this);
  InstructionOperand temps[] = {g.TempSimd128Register(),
                                g.TempSimd128Register()};
  Emit(kX64I32x4UConvertF32x4, g.DefineSameAsFirst(node),
       g.UseRegister(node->InputAt(0)), arraysize(temps), temps);
}

void InstructionSelector::VisitInt32AbsWithOverflow(Node* node) {
  UNREACHABLE();
}

void InstructionSelector::VisitInt64AbsWithOverflow(Node* node) {
  UNREACHABLE();
}

#if V8_ENABLE_WEBASSEMBLY
namespace {

// Returns true if shuffle can be decomposed into two 16x4 half shuffles
// followed by a 16x8 blend.
// E.g. [3 2 1 0 15 14 13 12].
bool TryMatch16x8HalfShuffle(uint8_t* shuffle16x8, uint8_t* blend_mask) {
  *blend_mask = 0;
  for (int i = 0; i < 8; i++) {
    if ((shuffle16x8[i] & 0x4) != (i & 0x4)) return false;
    *blend_mask |= (shuffle16x8[i] > 7 ? 1 : 0) << i;
  }
  return true;
}

struct ShuffleEntry {
  uint8_t shuffle[kSimd128Size];
  ArchOpcode opcode;
  bool src0_needs_reg;
  bool src1_needs_reg;
  // If AVX is supported, this shuffle can use AVX's three-operand encoding, so
  // does not require same as first. We conservatively set this to false
  // (original behavior), and selectively enable for specific arch shuffles.
  bool no_same_as_first_if_avx = false;
};

// Shuffles that map to architecture-specific instruction sequences. These are
// matched very early, so we shouldn't include shuffles that match better in
// later tests, like 32x4 and 16x8 shuffles. In general, these patterns should
// map to either a single instruction, or be finer grained, such as zip/unzip or
// transpose patterns.
static const ShuffleEntry arch_shuffles[] = {
    {{0, 1, 2, 3, 4, 5, 6, 7, 16, 17, 18, 19, 20, 21, 22, 23},
     kX64S64x2UnpackLow,
     true,
     true,
     true},
    {{8, 9, 10, 11, 12, 13, 14, 15, 24, 25, 26, 27, 28, 29, 30, 31},
     kX64S64x2UnpackHigh,
     true,
     true,
     true},
    {{0, 1, 2, 3, 16, 17, 18, 19, 4, 5, 6, 7, 20, 21, 22, 23},
     kX64S32x4UnpackLow,
     true,
     true,
     true},
    {{8, 9, 10, 11, 24, 25, 26, 27, 12, 13, 14, 15, 28, 29, 30, 31},
     kX64S32x4UnpackHigh,
     true,
     true,
     true},
    {{0, 1, 16, 17, 2, 3, 18, 19, 4, 5, 20, 21, 6, 7, 22, 23},
     kX64S16x8UnpackLow,
     true,
     true,
     true},
    {{8, 9, 24, 25, 10, 11, 26, 27, 12, 13, 28, 29, 14, 15, 30, 31},
     kX64S16x8UnpackHigh,
     true,
     true,
     true},
    {{0, 16, 1, 17, 2, 18, 3, 19, 4, 20, 5, 21, 6, 22, 7, 23},
     kX64S8x16UnpackLow,
     true,
     true,
     true},
    {{8, 24, 9, 25, 10, 26, 11, 27, 12, 28, 13, 29, 14, 30, 15, 31},
     kX64S8x16UnpackHigh,
     true,
     true,
     true},

    {{0, 1, 4, 5, 8, 9, 12, 13, 16, 17, 20, 21, 24, 25, 28, 29},
     kX64S16x8UnzipLow,
     true,
     true},
    {{2, 3, 6, 7, 10, 11, 14, 15, 18, 19, 22, 23, 26, 27, 30, 31},
     kX64S16x8UnzipHigh,
     true,
     true},
    {{0, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 22, 24, 26, 28, 30},
     kX64S8x16UnzipLow,
     true,
     true},
    {{1, 3, 5, 7, 9, 11, 13, 15, 17, 19, 21, 23, 25, 27, 29, 31},
     kX64S8x16UnzipHigh,
     true,
     true},
    {{0, 16, 2, 18, 4, 20, 6, 22, 8, 24, 10, 26, 12, 28, 14, 30},
     kX64S8x16TransposeLow,
     true,
     true},
    {{1, 17, 3, 19, 5, 21, 7, 23, 9, 25, 11, 27, 13, 29, 15, 31},
     kX64S8x16TransposeHigh,
     true,
     true},
    {{7, 6, 5, 4, 3, 2, 1, 0, 15, 14, 13, 12, 11, 10, 9, 8},
     kX64S8x8Reverse,
     true,
     true},
    {{3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12},
     kX64S8x4Reverse,
     true,
     true},
    {{1, 0, 3, 2, 5, 4, 7, 6, 9, 8, 11, 10, 13, 12, 15, 14},
     kX64S8x2Reverse,
     true,
     true}};

bool TryMatchArchShuffle(const uint8_t* shuffle, const ShuffleEntry* table,
                         size_t num_entries, bool is_swizzle,
                         const ShuffleEntry** arch_shuffle) {
  uint8_t mask = is_swizzle ? kSimd128Size - 1 : 2 * kSimd128Size - 1;
  for (size_t i = 0; i < num_entries; ++i) {
    const ShuffleEntry& entry = table[i];
    int j = 0;
    for (; j < kSimd128Size; ++j) {
      if ((entry.shuffle[j] & mask) != (shuffle[j] & mask)) {
        break;
      }
    }
    if (j == kSimd128Size) {
      *arch_shuffle = &entry;
      return true;
    }
  }
  return false;
}

bool TryMatchShufps(const uint8_t* shuffle32x4) {
  DCHECK_GT(8, shuffle32x4[2]);
  DCHECK_GT(8, shuffle32x4[3]);
  // shufps can be used if the first 2 indices select the first input [0-3], and
  // the other 2 indices select the second input [4-7].
  return shuffle32x4[0] < 4 && shuffle32x4[1] < 4 && shuffle32x4[2] > 3 &&
         shuffle32x4[3] > 3;
}

static bool IsV128ZeroConst(Node* node) {
  if (node->opcode() == IrOpcode::kS128Zero) {
    return true;
  }
  // If the node is a V128 const, check all the elements
  auto m = V128ConstMatcher(node);
  if (m.HasResolvedValue()) {
    auto imms = m.ResolvedValue().immediate();
    return std::all_of(imms.begin(), imms.end(), [](auto i) { return i == 0; });
  }
  return false;
}

static bool TryMatchOneInputIsZeros(Node* node, uint8_t* shuffle,
                                    bool* needs_swap) {
  *needs_swap = false;
  bool input0_is_zero = IsV128ZeroConst(node->InputAt(0));
  bool input1_is_zero = IsV128ZeroConst(node->InputAt(1));
  if (!input0_is_zero && !input1_is_zero) {
    return false;
  }

  if (input0_is_zero) {
    *needs_swap = true;
  }
  return true;
}

}  // namespace

void InstructionSelector::VisitI8x16Shuffle(Node* node) {
  uint8_t shuffle[kSimd128Size];
  bool is_swizzle;
  CanonicalizeShuffle(node, shuffle, &is_swizzle);

  int imm_count = 0;
  static const int kMaxImms = 6;
  uint32_t imms[kMaxImms];
  int temp_count = 0;
  static const int kMaxTemps = 2;
  InstructionOperand temps[kMaxTemps];

  X64OperandGenerator g(this);
  // Swizzles don't generally need DefineSameAsFirst to avoid a move.
  bool no_same_as_first = is_swizzle;
  // We generally need UseRegister for input0, Use for input1.
  // TODO(v8:9198): We don't have 16-byte alignment for SIMD operands yet, but
  // we retain this logic (continue setting these in the various shuffle match
  // clauses), but ignore it when selecting registers or slots.
  bool src0_needs_reg = true;
  bool src1_needs_reg = false;
  ArchOpcode opcode = kX64I8x16Shuffle;  // general shuffle is the default

  uint8_t offset;
  uint8_t shuffle32x4[4];
  uint8_t shuffle16x8[8];
  int index;
  const ShuffleEntry* arch_shuffle;
  bool needs_swap;
  if (wasm::SimdShuffle::TryMatchConcat(shuffle, &offset)) {
    if (wasm::SimdShuffle::TryMatch32x4Rotate(shuffle, shuffle32x4,
                                              is_swizzle)) {
      uint8_t shuffle_mask = wasm::SimdShuffle::PackShuffle4(shuffle32x4);
      opcode = kX64S32x4Rotate;
      imms[imm_count++] = shuffle_mask;
    } else {
      // Swap inputs from the normal order for (v)palignr.
      SwapShuffleInputs(node);
      is_swizzle = false;  // It's simpler to just handle the general case.
      no_same_as_first = CpuFeatures::IsSupported(AVX);
      // TODO(v8:9608): also see v8:9083
      src1_needs_reg = true;
      opcode = kX64S8x16Alignr;
      // palignr takes a single imm8 offset.
      imms[imm_count++] = offset;
    }
  } else if (TryMatchArchShuffle(shuffle, arch_shuffles,
                                 arraysize(arch_shuffles), is_swizzle,
                                 &arch_shuffle)) {
    opcode = arch_shuffle->opcode;
    src0_needs_reg = arch_shuffle->src0_needs_reg;
    // SSE can't take advantage of both operands in registers and needs
    // same-as-first.
    src1_needs_reg = arch_shuffle->src1_needs_reg;
    no_same_as_first =
        IsSupported(AVX) && arch_shuffle->no_same_as_first_if_avx;
  } else if (wasm::SimdShuffle::TryMatch32x4Shuffle(shuffle, shuffle32x4)) {
    uint8_t shuffle_mask = wasm::SimdShuffle::PackShuffle4(shuffle32x4);
    if (is_swizzle) {
      if (wasm::SimdShuffle::TryMatchIdentity(shuffle)) {
        // Bypass normal shuffle code generation in this case.
        EmitIdentity(node);
        return;
      } else {
        // pshufd takes a single imm8 shuffle mask.
        opcode = kX64S32x4Swizzle;
        no_same_as_first = true;
        // TODO(v8:9083): This doesn't strictly require a register, forcing the
        // swizzles to always use registers until generation of incorrect memory
        // operands can be fixed.
        src0_needs_reg = true;
        imms[imm_count++] = shuffle_mask;
      }
    } else {
      // 2 operand shuffle
      // A blend is more efficient than a general 32x4 shuffle; try it first.
      if (wasm::SimdShuffle::TryMatchBlend(shuffle)) {
        opcode = kX64S16x8Blend;
        uint8_t blend_mask = wasm::SimdShuffle::PackBlend4(shuffle32x4);
        imms[imm_count++] = blend_mask;
        no_same_as_first = CpuFeatures::IsSupported(AVX);
      } else if (TryMatchShufps(shuffle32x4)) {
        opcode = kX64Shufps;
        uint8_t mask = wasm::SimdShuffle::PackShuffle4(shuffle32x4);
        imms[imm_count++] = mask;
        src1_needs_reg = true;
        no_same_as_first = IsSupported(AVX);
      } else {
        opcode = kX64S32x4Shuffle;
        no_same_as_first = true;
        // TODO(v8:9083): src0 and src1 is used by pshufd in codegen, which
        // requires memory to be 16-byte aligned, since we cannot guarantee that
        // yet, force using a register here.
        src0_needs_reg = true;
        src1_needs_reg = true;
        imms[imm_count++] = shuffle_mask;
        uint8_t blend_mask = wasm::SimdShuffle::PackBlend4(shuffle32x4);
        imms[imm_count++] = blend_mask;
      }
    }
  } else if (wasm::SimdShuffle::TryMatch16x8Shuffle(shuffle, shuffle16x8)) {
    uint8_t blend_mask;
    if (wasm::SimdShuffle::TryMatchBlend(shuffle)) {
      opcode = kX64S16x8Blend;
      blend_mask = wasm::SimdShuffle::PackBlend8(shuffle16x8);
      imms[imm_count++] = blend_mask;
      no_same_as_first = CpuFeatures::IsSupported(AVX);
    } else if (wasm::SimdShuffle::TryMatchSplat<8>(shuffle, &index)) {
      opcode = kX64S16x8Dup;
      src0_needs_reg = false;
      imms[imm_count++] = index;
    } else if (TryMatch16x8HalfShuffle(shuffle16x8, &blend_mask)) {
      opcode = is_swizzle ? kX64S16x8HalfShuffle1 : kX64S16x8HalfShuffle2;
      // Half-shuffles don't need DefineSameAsFirst or UseRegister(src0).
      no_same_as_first = true;
      src0_needs_reg = false;
      uint8_t mask_lo = wasm::SimdShuffle::PackShuffle4(shuffle16x8);
      uint8_t mask_hi = wasm::SimdShuffle::PackShuffle4(shuffle16x8 + 4);
      imms[imm_count++] = mask_lo;
      imms[imm_count++] = mask_hi;
      if (!is_swizzle) imms[imm_count++] = blend_mask;
    }
  } else if (wasm::SimdShuffle::TryMatchSplat<16>(shuffle, &index)) {
    opcode = kX64S8x16Dup;
    no_same_as_first = false;
    src0_needs_reg = true;
    imms[imm_count++] = index;
  } else if (TryMatchOneInputIsZeros(node, shuffle, &needs_swap)) {
    is_swizzle = true;
    // Swap zeros to input1
    if (needs_swap) {
      SwapShuffleInputs(node);
      for (int i = 0; i < kSimd128Size; ++i) {
        shuffle[i] ^= kSimd128Size;
      }
    }
    if (wasm::SimdShuffle::TryMatchByteToDwordZeroExtend(shuffle)) {
      opcode = kX64I32X4ShiftZeroExtendI8x16;
      no_same_as_first = true;
      src0_needs_reg = true;
      imms[imm_count++] = shuffle[0];
    } else {
      // If the most significant bit (bit 7) of each byte of the shuffle control
      // mask is set, then constant zero is written in the result byte. Input1
      // is zeros now, we can avoid using input1 by setting bit 7 of shuffle[i]
      // to 1.
      for (int i = 0; i < kSimd128Size; ++i) {
        if (shuffle[i] >= kSimd128Size) {
          shuffle[i] = 0x80;
        }
      }
    }
  }
  if (opcode == kX64I8x16Shuffle) {
    // Use same-as-first for general swizzle, but not shuffle.
    no_same_as_first = !is_swizzle;
    src0_needs_reg = !no_same_as_first;
    imms[imm_count++] = wasm::SimdShuffle::Pack4Lanes(shuffle);
    imms[imm_count++] = wasm::SimdShuffle::Pack4Lanes(shuffle + 4);
    imms[imm_count++] = wasm::SimdShuffle::Pack4Lanes(shuffle + 8);
    imms[imm_count++] = wasm::SimdShuffle::Pack4Lanes(shuffle + 12);
    temps[temp_count++] = g.TempSimd128Register();
  }

  // Use DefineAsRegister(node) and Use(src0) if we can without forcing an extra
  // move instruction in the CodeGenerator.
  Node* input0 = node->InputAt(0);
  InstructionOperand dst =
      no_same_as_first ? g.DefineAsRegister(node) : g.DefineSameAsFirst(node);
  // TODO(v8:9198): Use src0_needs_reg when we have memory alignment for SIMD.
  // We only need a unique register for input0 if we use temp registers.
  InstructionOperand src0 =
      temp_count ? g.UseUniqueRegister(input0) : g.UseRegister(input0);
  USE(src0_needs_reg);

  int input_count = 0;
  InstructionOperand inputs[2 + kMaxImms + kMaxTemps];
  inputs[input_count++] = src0;
  if (!is_swizzle) {
    Node* input1 = node->InputAt(1);
    // TODO(v8:9198): Use src1_needs_reg when we have memory alignment for SIMD.
    // We only need a unique register for input1 if we use temp registers.
    inputs[input_count++] =
        temp_count ? g.UseUniqueRegister(input1) : g.UseRegister(input1);
    USE(src1_needs_reg);
  }
  for (int i = 0; i < imm_count; ++i) {
    inputs[input_count++] = g.UseImmediate(imms[i]);
  }
  Emit(opcode, 1, &dst, input_count, inputs, temp_count, temps);
}
#else
void InstructionSelector::VisitI8x16Shuffle(Node* node) { UNREACHABLE(); }
#endif  // V8_ENABLE_WEBASSEMBLY

#if V8_ENABLE_WEBASSEMBLY
void InstructionSelector::VisitI8x16Swizzle(Node* node) {
  InstructionCode op = kX64I8x16Swizzle;

  bool relaxed = OpParameter<bool>(node->op());
  if (relaxed) {
    op |= MiscField::encode(true);
  } else {
    auto m = V128ConstMatcher(node->InputAt(1));
    if (m.HasResolvedValue()) {
      // If the indices vector is a const, check if they are in range, or if the
      // top bit is set, then we can avoid the paddusb in the codegen and simply
      // emit a pshufb.
      auto imms = m.ResolvedValue().immediate();
      op |= MiscField::encode(wasm::SimdSwizzle::AllInRangeOrTopBitSet(imms));
    }
  }

  X64OperandGenerator g(this);
  Emit(op,
       IsSupported(AVX) ? g.DefineAsRegister(node) : g.DefineSameAsFirst(node),
       g.UseRegister(node->InputAt(0)), g.UseRegister(node->InputAt(1)));
}

namespace {
void VisitRelaxedLaneSelect(InstructionSelector* selector, Node* node,
                            InstructionCode code = kX64Pblendvb) {
  X64OperandGenerator g(selector);
  // pblendvb/blendvps/blendvpd copies src2 when mask is set, opposite from Wasm
  // semantics. Node's inputs are: mask, lhs, rhs (determined in
  // wasm-compiler.cc).
  if (selector->IsSupported(AVX)) {
    selector->Emit(
        code, g.DefineAsRegister(node), g.UseRegister(node->InputAt(2)),
        g.UseRegister(node->InputAt(1)), g.UseRegister(node->InputAt(0)));
  } else {
    // SSE4.1 pblendvb/blendvps/blendvpd requires xmm0 to hold the mask as an
    // implicit operand.
    selector->Emit(
        code, g.DefineSameAsFirst(node), g.UseRegister(node->InputAt(2)),
        g.UseRegister(node->InputAt(1)), g.UseFixed(node->InputAt(0), xmm0));
  }
}
}  // namespace

void InstructionSelector::VisitI8x16RelaxedLaneSelect(Node* node) {
  VisitRelaxedLaneSelect(this, node);
}
void InstructionSelector::VisitI16x8RelaxedLaneSelect(Node* node) {
  VisitRelaxedLaneSelect(this, node);
}
void InstructionSelector::VisitI32x4RelaxedLaneSelect(Node* node) {
  VisitRelaxedLaneSelect(this, node, kX64Blendvps);
}

void InstructionSelector::VisitI64x2RelaxedLaneSelect(Node* node) {
  VisitRelaxedLaneSelect(this, node, kX64Blendvpd);
}
#else
void InstructionSelector::VisitI8x16Swizzle(Node* node) { UNREACHABLE(); }
void InstructionSelector::VisitI8x16RelaxedLaneSelect(Node* node) {
  UNREACHABLE();
}
void InstructionSelector::VisitI16x8RelaxedLaneSelect(Node* node) {
  UNREACHABLE();
}
void InstructionSelector::VisitI32x4RelaxedLaneSelect(Node* node) {
  UNREACHABLE();
}
void InstructionSelector::VisitI64x2RelaxedLaneSelect(Node* node) {
  UNREACHABLE();
}
#endif  // V8_ENABLE_WEBASSEMBLY

namespace {
// Used for pmin/pmax and relaxed min/max.
void VisitMinOrMax(InstructionSelector* selector, Node* node, ArchOpcode opcode,
                   bool flip_inputs) {
  X64OperandGenerator g(selector);
  InstructionOperand dst = selector->IsSupported(AVX)
                               ? g.DefineAsRegister(node)
                               : g.DefineSameAsFirst(node);
  if (flip_inputs) {
    // Due to the way minps/minpd work, we want the dst to be same as the second
    // input: b = pmin(a, b) directly maps to minps b a.
    selector->Emit(opcode, dst, g.UseRegister(node->InputAt(1)),
                   g.UseRegister(node->InputAt(0)));
  } else {
    selector->Emit(opcode, dst, g.UseRegister(node->InputAt(0)),
                   g.UseRegister(node->InputAt(1)));
  }
}
}  // namespace

void InstructionSelector::VisitF32x4Pmin(Node* node) {
  VisitMinOrMax(this, node, kX64Minps, true);
}

void InstructionSelector::VisitF32x4Pmax(Node* node) {
  VisitMinOrMax(this, node, kX64Maxps, true);
}

void InstructionSelector::VisitF64x2Pmin(Node* node) {
  VisitMinOrMax(this, node, kX64Minpd, true);
}

void InstructionSelector::VisitF64x2Pmax(Node* node) {
  VisitMinOrMax(this, node, kX64Maxpd, true);
}

void InstructionSelector::VisitF32x4RelaxedMin(Node* node) {
  VisitMinOrMax(this, node, kX64Minps, false);
}

void InstructionSelector::VisitF32x4RelaxedMax(Node* node) {
  VisitMinOrMax(this, node, kX64Maxps, false);
}

void InstructionSelector::VisitF64x2RelaxedMin(Node* node) {
  VisitMinOrMax(this, node, kX64Minpd, false);
}

void InstructionSelector::VisitF64x2RelaxedMax(Node* node) {
  VisitMinOrMax(this, node, kX64Maxpd, false);
}

void InstructionSelector::VisitI32x4ExtAddPairwiseI16x8S(Node* node) {
  X64OperandGenerator g(this);
  InstructionOperand dst = CpuFeatures::IsSupported(AVX)
                               ? g.DefineAsRegister(node)
                               : g.DefineSameAsFirst(node);
  Emit(kX64I32x4ExtAddPairwiseI16x8S, dst, g.UseRegister(node->InputAt(0)));
}

void InstructionSelector::VisitI32x4ExtAddPairwiseI16x8U(Node* node) {
  X64OperandGenerator g(this);
  InstructionOperand dst = CpuFeatures::IsSupported(AVX)
                               ? g.DefineAsRegister(node)
                               : g.DefineSameAsFirst(node);
  Emit(kX64I32x4ExtAddPairwiseI16x8U, dst, g.UseRegister(node->InputAt(0)));
}

void InstructionSelector::VisitI16x8ExtAddPairwiseI8x16S(Node* node) {
  X64OperandGenerator g(this);
  // Codegen depends on dst != src.
  Emit(kX64I16x8ExtAddPairwiseI8x16S, g.DefineAsRegister(node),
       g.UseUniqueRegister(node->InputAt(0)));
}

void InstructionSelector::VisitI16x8ExtAddPairwiseI8x16U(Node* node) {
  X64OperandGenerator g(this);
  InstructionOperand dst = CpuFeatures::IsSupported(AVX)
                               ? g.DefineAsRegister(node)
                               : g.DefineSameAsFirst(node);
  Emit(kX64I16x8ExtAddPairwiseI8x16U, dst, g.UseRegister(node->InputAt(0)));
}

void InstructionSelector::VisitI8x16Popcnt(Node* node) {
  X64OperandGenerator g(this);
  InstructionOperand temps[] = {g.TempSimd128Register()};
  Emit(kX64I8x16Popcnt, g.DefineAsRegister(node),
       g.UseUniqueRegister(node->InputAt(0)), arraysize(temps), temps);
}

void InstructionSelector::VisitF64x2ConvertLowI32x4U(Node* node) {
  X64OperandGenerator g(this);
  InstructionOperand dst =
      IsSupported(AVX) ? g.DefineAsRegister(node) : g.DefineSameAsFirst(node);
  Emit(kX64F64x2ConvertLowI32x4U, dst, g.UseRegister(node->InputAt(0)));
}

void InstructionSelector::VisitI32x4TruncSatF64x2SZero(Node* node) {
  X64OperandGenerator g(this);
  if (CpuFeatures::IsSupported(AVX)) {
    // Requires dst != src.
    Emit(kX64I32x4TruncSatF64x2SZero, g.DefineAsRegister(node),
         g.UseUniqueRegister(node->InputAt(0)));
  } else {
    Emit(kX64I32x4TruncSatF64x2SZero, g.DefineSameAsFirst(node),
         g.UseRegister(node->InputAt(0)));
  }
}

void InstructionSelector::VisitI32x4TruncSatF64x2UZero(Node* node) {
  X64OperandGenerator g(this);
  InstructionOperand dst = CpuFeatures::IsSupported(AVX)
                               ? g.DefineAsRegister(node)
                               : g.DefineSameAsFirst(node);
  Emit(kX64I32x4TruncSatF64x2UZero, dst, g.UseRegister(node->InputAt(0)));
}

void InstructionSelector::VisitI32x4RelaxedTruncF64x2SZero(Node* node) {
  VisitFloatUnop(this, node, node->InputAt(0), kX64Cvttpd2dq);
}

void InstructionSelector::VisitI32x4RelaxedTruncF64x2UZero(Node* node) {
  VisitFloatUnop(this, node, node->InputAt(0), kX64I32x4TruncF64x2UZero);
}

void InstructionSelector::VisitI32x4RelaxedTruncF32x4S(Node* node) {
  VisitFloatUnop(this, node, node->InputAt(0), kX64Cvttps2dq);
}

void InstructionSelector::VisitI32x4RelaxedTruncF32x4U(Node* node) {
  VisitFloatUnop(this, node, node->InputAt(0), kX64I32x4TruncF32x4U);
}

void InstructionSelector::VisitI64x2GtS(Node* node) {
  X64OperandGenerator g(this);
  if (CpuFeatures::IsSupported(AVX)) {
    Emit(kX64IGtS | LaneSizeField::encode(kL64) |
             VectorLengthField::encode(kV128),
         g.DefineAsRegister(node), g.UseRegister(node->InputAt(0)),
         g.UseRegister(node->InputAt(1)));
  } else if (CpuFeatures::IsSupported(SSE4_2)) {
    Emit(kX64IGtS | LaneSizeField::encode(kL64) |
             VectorLengthField::encode(kV128),
         g.DefineSameAsFirst(node), g.UseRegister(node->InputAt(0)),
         g.UseRegister(node->InputAt(1)));
  } else {
    Emit(kX64IGtS | LaneSizeField::encode(kL64) |
             VectorLengthField::encode(kV128),
         g.DefineAsRegister(node), g.UseUniqueRegister(node->InputAt(0)),
         g.UseUniqueRegister(node->InputAt(1)));
  }
}

void InstructionSelector::VisitI64x2GeS(Node* node) {
  X64OperandGenerator g(this);
  if (CpuFeatures::IsSupported(AVX)) {
    Emit(kX64IGeS | LaneSizeField::encode(kL64) |
             VectorLengthField::encode(kV128),
         g.DefineAsRegister(node), g.UseRegister(node->InputAt(0)),
         g.UseRegister(node->InputAt(1)));
  } else if (CpuFeatures::IsSupported(SSE4_2)) {
    Emit(kX64IGeS | LaneSizeField::encode(kL64) |
             VectorLengthField::encode(kV128),
         g.DefineAsRegister(node), g.UseUniqueRegister(node->InputAt(0)),
         g.UseRegister(node->InputAt(1)));
  } else {
    Emit(kX64IGeS | LaneSizeField::encode(kL64) |
             VectorLengthField::encode(kV128),
         g.DefineAsRegister(node), g.UseUniqueRegister(node->InputAt(0)),
         g.UseUniqueRegister(node->InputAt(1)));
  }
}

void InstructionSelector::VisitI64x2Abs(Node* node) {
  X64OperandGenerator g(this);
  if (CpuFeatures::IsSupported(AVX)) {
    Emit(kX64IAbs | LaneSizeField::encode(kL64) |
             VectorLengthField::encode(kV128),
         g.DefineAsRegister(node), g.UseUniqueRegister(node->InputAt(0)));
  } else {
    Emit(kX64IAbs | LaneSizeField::encode(kL64) |
             VectorLengthField::encode(kV128),
         g.DefineSameAsFirst(node), g.UseRegister(node->InputAt(0)));
  }
}

void InstructionSelector::VisitF64x2PromoteLowF32x4(Node* node) {
  X64OperandGenerator g(this);
  InstructionCode code = kX64F64x2PromoteLowF32x4;
  Node* input = node->InputAt(0);
  LoadTransformMatcher m(input);

  if (m.Is(LoadTransformation::kS128Load64Zero) && CanCover(node, input)) {
    if (m.ResolvedValue().kind == MemoryAccessKind::kProtected) {
      code |= AccessModeField::encode(kMemoryAccessProtectedMemOutOfBounds);
    }
    // LoadTransforms cannot be eliminated, so they are visited even if
    // unused. Mark it as defined so that we don't visit it.
    MarkAsDefined(input);
    VisitLoad(node, input, code);
    return;
  }

  VisitRR(this, node, code);
}

void InstructionSelector::VisitI16x8DotI8x16I7x16S(Node* node) {
  X64OperandGenerator g(this);
  Emit(kX64I16x8DotI8x16I7x16S, g.DefineAsRegister(node),
       g.UseUniqueRegister(node->InputAt(0)), g.UseRegister(node->InputAt(1)));
}

void InstructionSelector::VisitI32x4DotI8x16I7x16AddS(Node* node) {
  X64OperandGenerator g(this);
  InstructionOperand temps[] = {g.TempSimd128Register()};
  Emit(kX64I32x4DotI8x16I7x16AddS, g.DefineSameAsInput(node, 2),
       g.UseUniqueRegister(node->InputAt(0)),
       g.UseUniqueRegister(node->InputAt(1)),
       g.UseUniqueRegister(node->InputAt(2)), arraysize(temps), temps);
}

void InstructionSelector::AddOutputToSelectContinuation(OperandGenerator* g,
                                                        int first_input_index,
                                                        Node* node) {
  continuation_outputs_.push_back(
      g->DefineSameAsInput(node, first_input_index));
}

// static
MachineOperatorBuilder::Flags
InstructionSelector::SupportedMachineOperatorFlags() {
  MachineOperatorBuilder::Flags flags =
      MachineOperatorBuilder::kWord32ShiftIsSafe |
      MachineOperatorBuilder::kWord32Ctz | MachineOperatorBuilder::kWord64Ctz |
      MachineOperatorBuilder::kWord32Rol | MachineOperatorBuilder::kWord64Rol |
      MachineOperatorBuilder::kWord32Select |
      MachineOperatorBuilder::kWord64Select;
  if (CpuFeatures::IsSupported(POPCNT)) {
    flags |= MachineOperatorBuilder::kWord32Popcnt |
             MachineOperatorBuilder::kWord64Popcnt;
  }
  if (CpuFeatures::IsSupported(SSE4_1)) {
    flags |= MachineOperatorBuilder::kFloat32RoundDown |
             MachineOperatorBuilder::kFloat64RoundDown |
             MachineOperatorBuilder::kFloat32RoundUp |
             MachineOperatorBuilder::kFloat64RoundUp |
             MachineOperatorBuilder::kFloat32RoundTruncate |
             MachineOperatorBuilder::kFloat64RoundTruncate |
             MachineOperatorBuilder::kFloat32RoundTiesEven |
             MachineOperatorBuilder::kFloat64RoundTiesEven;
  }
  return flags;
}

// static
MachineOperatorBuilder::AlignmentRequirements
InstructionSelector::AlignmentRequirements() {
  return MachineOperatorBuilder::AlignmentRequirements::
      FullUnalignedAccessSupport();
}

}  // namespace compiler
}  // namespace internal
}  // namespace v8
