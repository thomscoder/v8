// Copyright 2022 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/compiler/turboshaft/operations.h"

#include <atomic>
#include <sstream>

#include "src/base/platform/platform.h"
#include "src/common/assert-scope.h"
#include "src/common/globals.h"
#include "src/compiler/frame-states.h"
#include "src/compiler/turboshaft/deopt-data.h"
#include "src/compiler/turboshaft/graph.h"
#include "src/handles/handles-inl.h"

namespace v8::internal::compiler::turboshaft {

const char* OpcodeName(Opcode opcode) {
#define OPCODE_NAME(Name) #Name,
  const char* table[kNumberOfOpcodes] = {
      TURBOSHAFT_OPERATION_LIST(OPCODE_NAME)};
#undef OPCODE_NAME
  return table[OpcodeIndex(opcode)];
}

std::ostream& operator<<(std::ostream& os, OperationPrintStyle styled_op) {
  const Operation& op = styled_op.op;
  os << OpcodeName(op.opcode) << "(";
  bool first = true;
  for (OpIndex input : op.inputs()) {
    if (!first) os << ", ";
    first = false;
    os << styled_op.op_index_prefix << input.id();
  }
  os << ")";
  op.PrintOptions(os);
  return os;
}

std::ostream& operator<<(std::ostream& os, IntegerUnaryOp::Kind kind) {
  switch (kind) {
    case IntegerUnaryOp::Kind::kReverseBytes:
      return os << "ReverseBytes";
    case IntegerUnaryOp::Kind::kCountLeadingZeros:
      return os << "CountLeadingZeros";
  }
}

std::ostream& operator<<(std::ostream& os, FloatUnaryOp::Kind kind) {
  switch (kind) {
    case FloatUnaryOp::Kind::kAbs:
      return os << "Abs";
    case FloatUnaryOp::Kind::kNegate:
      return os << "Negate";
    case FloatUnaryOp::Kind::kSilenceNaN:
      return os << "SilenceNaN";
    case FloatUnaryOp::Kind::kRoundUp:
      return os << "RoundUp";
    case FloatUnaryOp::Kind::kRoundDown:
      return os << "RoundDown";
    case FloatUnaryOp::Kind::kRoundToZero:
      return os << "RoundToZero";
    case FloatUnaryOp::Kind::kRoundTiesEven:
      return os << "RoundTiesEven";
    case FloatUnaryOp::Kind::kLog:
      return os << "Log";
    case FloatUnaryOp::Kind::kSqrt:
      return os << "Sqrt";
    case FloatUnaryOp::Kind::kExp:
      return os << "Exp";
    case FloatUnaryOp::Kind::kExpm1:
      return os << "Expm1";
    case FloatUnaryOp::Kind::kSin:
      return os << "Sin";
    case FloatUnaryOp::Kind::kCos:
      return os << "Cos";
    case FloatUnaryOp::Kind::kAsin:
      return os << "Asin";
    case FloatUnaryOp::Kind::kAcos:
      return os << "Acos";
    case FloatUnaryOp::Kind::kSinh:
      return os << "Sinh";
    case FloatUnaryOp::Kind::kCosh:
      return os << "Cosh";
    case FloatUnaryOp::Kind::kAsinh:
      return os << "Asinh";
    case FloatUnaryOp::Kind::kAcosh:
      return os << "Acosh";
    case FloatUnaryOp::Kind::kTan:
      return os << "Tan";
    case FloatUnaryOp::Kind::kTanh:
      return os << "Tanh";
  }
}

std::ostream& operator<<(std::ostream& os, ShiftOp::Kind kind) {
  switch (kind) {
    case ShiftOp::Kind::kShiftRightArithmeticShiftOutZeros:
      return os << "ShiftRightArithmeticShiftOutZeros";
    case ShiftOp::Kind::kShiftRightArithmetic:
      return os << "ShiftRightArithmetic";
    case ShiftOp::Kind::kShiftRightLogical:
      return os << "ShiftRightLogical";
    case ShiftOp::Kind::kShiftLeft:
      return os << "ShiftLeft";
    case ShiftOp::Kind::kRotateRight:
      return os << "RotateRight";
    case ShiftOp::Kind::kRotateLeft:
      return os << "RotateLeft";
  }
}

std::ostream& operator<<(std::ostream& os, ComparisonOp::Kind kind) {
  switch (kind) {
    case ComparisonOp::Kind::kSignedLessThan:
      return os << "SignedLessThan";
    case ComparisonOp::Kind::kSignedLessThanOrEqual:
      return os << "SignedLessThanOrEqual";
    case ComparisonOp::Kind::kUnsignedLessThan:
      return os << "UnsignedLessThan";
    case ComparisonOp::Kind::kUnsignedLessThanOrEqual:
      return os << "UnsignedLessThanOrEqual";
  }
}

std::ostream& operator<<(std::ostream& os, ChangeOp::Kind kind) {
  switch (kind) {
    case ChangeOp::Kind::kSignedNarrowing:
      return os << "SignedNarrowing";
    case ChangeOp::Kind::kUnsignedNarrowing:
      return os << "UnsignedNarrowing";
    case ChangeOp::Kind::kIntegerTruncate:
      return os << "IntegerTruncate";
    case ChangeOp::Kind::kFloatConversion:
      return os << "FloatConversion";
    case ChangeOp::Kind::kSignedFloatTruncate:
      return os << "SignedFloatTruncate";
    case ChangeOp::Kind::kUnsignedFloatTruncate:
      return os << "UnsignedFloatTruncate";
    case ChangeOp::Kind::kSignedFloatTruncateOverflowToMin:
      return os << "SignedFloatTruncateOverflowToMin";
    case ChangeOp::Kind::kSignedToFloat:
      return os << "SignedToFloat";
    case ChangeOp::Kind::kUnsignedToFloat:
      return os << "UnsignedToFloat";
    case ChangeOp::Kind::kExtractHighHalf:
      return os << "ExtractHighHalf";
    case ChangeOp::Kind::kExtractLowHalf:
      return os << "ExtractLowHalf";
    case ChangeOp::Kind::kZeroExtend:
      return os << "ZeroExtend";
    case ChangeOp::Kind::kSignExtend:
      return os << "SignExtend";
    case ChangeOp::Kind::kBitcast:
      return os << "Bitcast";
  }
}

std::ostream& operator<<(std::ostream& os, Float64InsertWord32Op::Kind kind) {
  switch (kind) {
    case Float64InsertWord32Op::Kind::kLowHalf:
      return os << "LowHalf";
    case Float64InsertWord32Op::Kind::kHighHalf:
      return os << "HighHalf";
  }
}

std::ostream& operator<<(std::ostream& os, ProjectionOp::Kind kind) {
  switch (kind) {
    case ProjectionOp::Kind::kTuple:
      return os << "tuple";
    case ProjectionOp::Kind::kExceptionValue:
      return os << "exception value";
  }
}

std::ostream& operator<<(std::ostream& os, FrameConstantOp::Kind kind) {
  switch (kind) {
    case FrameConstantOp::Kind::kStackCheckOffset:
      return os << "stack check offset";
    case FrameConstantOp::Kind::kFramePointer:
      return os << "frame pointer";
    case FrameConstantOp::Kind::kParentFramePointer:
      return os << "parent frame pointer";
  }
}

void Operation::PrintOptions(std::ostream& os) const {
  switch (opcode) {
#define SWITCH_CASE(Name)              \
  case Opcode::k##Name:                \
    Cast<Name##Op>().PrintOptions(os); \
    break;
    TURBOSHAFT_OPERATION_LIST(SWITCH_CASE)
#undef SWITCH_CASE
  }
}

void PendingLoopPhiOp::PrintOptions(std::ostream& os) const {
  os << "[" << rep << ", #o" << old_backedge_index.id() << "]";
}

void ConstantOp::PrintOptions(std::ostream& os) const {
  os << "[";
  switch (kind) {
    case Kind::kWord32:
      os << "word32: " << static_cast<int32_t>(storage.integral);
      break;
    case Kind::kWord64:
      os << "word64: " << static_cast<int64_t>(storage.integral);
      break;
    case Kind::kNumber:
      os << "number: " << number();
      break;
    case Kind::kTaggedIndex:
      os << "tagged index: " << tagged_index();
      break;
    case Kind::kFloat64:
      os << "float64: " << float64();
      break;
    case Kind::kFloat32:
      os << "float32: " << float32();
      break;
    case Kind::kExternal:
      os << "external: " << external_reference();
      break;
    case Kind::kHeapObject:
      os << "heap object: " << handle();
      break;
    case Kind::kCompressedHeapObject:
      os << "compressed heap object: " << handle();
      break;
    case Kind::kDelayedString:
      os << delayed_string();
      break;
  }
  os << "]";
}

void LoadOp::PrintOptions(std::ostream& os) const {
  os << "[";
  os << (kind == Kind::kTaggedBase ? "tagged base" : "raw");
  if (!IsAlignedAccess(kind)) os << ", unaligned";
  os << ", " << loaded_rep;
  if (offset != 0) os << ", offset: " << offset;
  os << "]";
}

void ParameterOp::PrintOptions(std::ostream& os) const {
  os << "[" << parameter_index;
  if (debug_name) os << ", " << debug_name;
  os << "]";
}

void IndexedLoadOp::PrintOptions(std::ostream& os) const {
  os << "[";
  os << (kind == Kind::kTaggedBase ? "tagged base" : "raw");
  if (!IsAlignedAccess(kind)) os << ", unaligned";
  os << ", " << loaded_rep;
  if (element_size_log2 != 0)
    os << ", element size: 2^" << int{element_size_log2};
  if (offset != 0) os << ", offset: " << offset;
  os << "]";
}

void StoreOp::PrintOptions(std::ostream& os) const {
  os << "[";
  os << (kind == Kind::kTaggedBase ? "tagged base" : "raw");
  if (!IsAlignedAccess(kind)) os << ", unaligned";
  os << ", " << stored_rep;
  os << ", " << write_barrier;
  if (offset != 0) os << ", offset: " << offset;
  os << "]";
}

void IndexedStoreOp::PrintOptions(std::ostream& os) const {
  os << "[";
  os << (kind == Kind::kTaggedBase ? "tagged base" : "raw");
  if (!IsAlignedAccess(kind)) os << ", unaligned";
  os << ", " << stored_rep;
  os << ", " << write_barrier;
  if (element_size_log2 != 0)
    os << ", element size: 2^" << int{element_size_log2};
  if (offset != 0) os << ", offset: " << offset;
  os << "]";
}

void FrameStateOp::PrintOptions(std::ostream& os) const {
  os << "[";
  os << (inlined ? "inlined" : "not inlined");
  os << ", ";
  os << data->frame_state_info;
  os << ", state values:";
  FrameStateData::Iterator it = data->iterator(state_values());
  while (it.has_more()) {
    os << " ";
    switch (it.current_instr()) {
      case FrameStateData::Instr::kInput: {
        MachineType type;
        OpIndex input;
        it.ConsumeInput(&type, &input);
        os << "#" << input.id() << "(" << type << ")";
        break;
      }
      case FrameStateData::Instr::kUnusedRegister:
        it.ConsumeUnusedRegister();
        os << ".";
        break;
      case FrameStateData::Instr::kDematerializedObject: {
        uint32_t id;
        uint32_t field_count;
        it.ConsumeDematerializedObject(&id, &field_count);
        os << "$" << id << "(field count: " << field_count << ")";
        break;
      }
      case FrameStateData::Instr::kDematerializedObjectReference: {
        uint32_t id;
        it.ConsumeDematerializedObjectReference(&id);
        os << "$" << id;
        break;
      }
      case FrameStateData::Instr::kArgumentsElements: {
        CreateArgumentsType type;
        it.ConsumeArgumentsElements(&type);
        os << "ArgumentsElements(" << type << ")";
        break;
      }
      case FrameStateData::Instr::kArgumentsLength: {
        it.ConsumeArgumentsLength();
        os << "ArgumentsLength";
        break;
      }
    }
  }
  os << "]";
}

void BinopOp::PrintOptions(std::ostream& os) const {
  os << "[";
  switch (kind) {
    case Kind::kAdd:
      os << "Add, ";
      break;
    case Kind::kSub:
      os << "Sub, ";
      break;
    case Kind::kMul:
      os << "Mul, ";
      break;
    case Kind::kSignedMulOverflownBits:
      os << "SignedMulOverflownBits, ";
      break;
    case Kind::kUnsignedMulOverflownBits:
      os << "UnsignedMulOverflownBits, ";
      break;
    case Kind::kSignedDiv:
      os << "SignedDiv, ";
      break;
    case Kind::kUnsignedDiv:
      os << "UnsignedDiv, ";
      break;
    case Kind::kSignedMod:
      os << "SignedMod, ";
      break;
    case Kind::kUnsignedMod:
      os << "UnsignedMod, ";
      break;
    case Kind::kBitwiseAnd:
      os << "BitwiseAnd, ";
      break;
    case Kind::kBitwiseOr:
      os << "BitwiseOr, ";
      break;
    case Kind::kBitwiseXor:
      os << "BitwiseXor, ";
      break;
    case Kind::kMin:
      os << "Min, ";
      break;
    case Kind::kMax:
      os << "Max, ";
      break;
    case Kind::kPower:
      os << "Power, ";
      break;
    case Kind::kAtan2:
      os << "Atan2, ";
      break;
  }
  os << rep;
  os << "]";
}

void OverflowCheckedBinopOp::PrintOptions(std::ostream& os) const {
  os << "[";
  switch (kind) {
    case Kind::kSignedAdd:
      os << "signed add, ";
      break;
    case Kind::kSignedSub:
      os << "signed sub, ";
      break;
    case Kind::kSignedMul:
      os << "signed mul, ";
      break;
  }
  os << rep;
  os << "]";
}

std::ostream& operator<<(std::ostream& os, BlockIndex b) {
  if (!b.valid()) {
    return os << "<invalid block>";
  }
  return os << 'B' << b.id();
}

std::ostream& operator<<(std::ostream& os, const Block* b) {
  return os << b->index();
}

void SwitchOp::PrintOptions(std::ostream& os) const {
  os << "[";
  for (const Case& c : cases) {
    os << "case " << c.value << ": " << c.destination << ", ";
  }
  os << " default: " << default_case << "]";
}

std::string Operation::ToString() const {
  std::stringstream ss;
  ss << *this;
  return ss.str();
}

}  // namespace v8::internal::compiler::turboshaft
