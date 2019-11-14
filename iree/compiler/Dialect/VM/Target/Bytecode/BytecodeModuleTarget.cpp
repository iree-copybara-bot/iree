// Copyright 2019 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "iree/compiler/Dialect/VM/Target/Bytecode/BytecodeModuleTarget.h"

#include "flatbuffers/flatbuffers.h"
#include "flatbuffers/minireflect.h"
#include "iree/compiler/Dialect/Types.h"
#include "iree/compiler/Dialect/VM/Analysis/RegisterAllocation.h"
#include "iree/compiler/Dialect/VM/Analysis/ValueLiveness.h"
#include "iree/compiler/Dialect/VM/IR/VMDialect.h"
#include "iree/compiler/Dialect/VM/IR/VMOps.h"
#include "iree/compiler/Dialect/VM/Target/Bytecode/BytecodeEncoder.h"
#include "iree/compiler/Dialect/VM/Target/Bytecode/ConstantEncoder.h"
#include "iree/compiler/Dialect/VM/Transforms/Passes.h"
#include "iree/schemas/bytecode_module_def_generated.h"
#include "llvm/Support/ErrorHandling.h"
#include "mlir/IR/Diagnostics.h"
#include "mlir/IR/Module.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/IR/Visitors.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Pass/PassManager.h"
#include "mlir/Transforms/DialectConversion.h"
#include "mlir/Transforms/Passes.h"
#include "mlir/Translation.h"

namespace mlir {
namespace iree_compiler {

namespace {

using flatbuffers::FlatBufferBuilder;
using flatbuffers::Offset;
using flatbuffers::Vector;

struct ModuleCounts {
  int importFuncs = 0;
  int exportFuncs = 0;
  int internalFuncs = 0;
  int globalBytes = 0;
  int globalRefs = 0;
  int rodatas = 0;
  int rwdatas = 0;
};

}  // namespace

// Computes symbol counts within the given |moduleOp|.
// These counts, including the global byte reservation count, are expected to
// match the actual values during serialization.
//
// Preconditions:
//  - OrdinalAllocationPass has run on the module
//  - All ordinals start from 0 and are contiguous
static ModuleCounts computeModuleSymbolCounts(IREE::VM::ModuleOp moduleOp) {
  ModuleCounts counts;
  for (auto &op : moduleOp.getBlock().getOperations()) {
    if (auto funcOp = dyn_cast<IREE::VM::FuncOp>(op)) {
      if (funcOp.isExternal()) {
        ++counts.importFuncs;
      } else {
        ++counts.internalFuncs;
      }
    } else if (isa<IREE::VM::ExportOp>(op)) {
      ++counts.exportFuncs;
    } else if (isa<IREE::VM::GlobalI32Op>(op)) {
      ++counts.globalBytes;
    } else if (isa<IREE::VM::GlobalRefOp>(op)) {
      ++counts.globalRefs;
    } else if (isa<IREE::VM::RodataOp>(op)) {
      ++counts.rodatas;
    }
  }
  return counts;
}

// Canonicalizes the module to its final form prior to emission.
// This verifies that we only have ops we can serialize and performs any of the
// required transformations (such as debug op stripping).
static LogicalResult canonicalizeModule(BytecodeTargetOptions targetOptions,
                                        IREE::VM::ModuleOp moduleOp) {
  OwningRewritePatternList patterns;
  ConversionTarget target(*moduleOp.getContext());
  target.addLegalDialect<IREE::VM::VMDialect>();

  if (targetOptions.stripDebugOps) {
    // TODO(benvanik): add RemoveDisabledDebugOp pattern.
    target.addIllegalOp<IREE::VM::TraceOp, IREE::VM::PrintOp, IREE::VM::BreakOp,
                        IREE::VM::CondBreakOp>();
  }

  if (failed(applyFullConversion(moduleOp, target, patterns))) {
    return moduleOp.emitError() << "unable to fully apply conversion to module";
  }

  PassManager passManager(moduleOp.getContext());
  auto &modulePasses = passManager.nest<IREE::VM::ModuleOp>();

  if (targetOptions.optimize) {
    // TODO(benvanik): does this run until it quiesces?
    modulePasses.addPass(mlir::createInlinerPass());
    modulePasses.addPass(mlir::createCSEPass());
    modulePasses.addPass(mlir::createCanonicalizerPass());
  }

  // TODO(benvanik): analysis instead? useful to have ordinals in MLIR text?
  // We don't want any more modifications after this point as they could
  // invalidate the ordinals.
  modulePasses.addPass(IREE::VM::createOrdinalAllocationPass());

  if (failed(passManager.run(moduleOp.getParentOfType<mlir::ModuleOp>()))) {
    return moduleOp.emitError() << "failed during transform passes";
  }

  return success();
}

// Returns a vector of tables of type T or None if |contents| is empty.
template <typename T>
static Optional<Offset<Vector<Offset<T>>>> createOptionalVector(
    const std::vector<Offset<T>> &contents, FlatBufferBuilder &fbb) {
  if (contents.empty()) return llvm::None;
  return fbb.CreateVector(contents);
}
template <typename T>
static Optional<Offset<Vector<T>>> createOptionalVector(
    const std::vector<T> &contents, FlatBufferBuilder &fbb) {
  if (contents.empty()) return llvm::None;
  return fbb.CreateVector(contents);
}

// Converts a Type of the expected IREE set (mostly integers and ref_ptrs) to an
// enum matching the description in the flatbuffer. This is currently... loosely
// defined.
static uint32_t typeToKindEnum(Type type) {
  if (type.isInteger(32)) {
    return 1;
  } else if (auto refPtrType = type.dyn_cast<IREE::RefPtrType>()) {
    // TODO(benvanik): use a stable type ID.
    return refPtrType.getObjectType().getKind() - Type::FIRST_IREE_TYPE;
  }
  type.dump();
  llvm_unreachable("invalid type");
  return 0;
}

// Returns a serialized function signature.
static Offset<iree::vm::FunctionSignatureDef> makeFunctionSignatureDef(
    FunctionType functionType, FlatBufferBuilder &fbb) {
  std::vector<uint32_t> argumentTypes;
  argumentTypes.resize(functionType.getNumInputs());
  for (int i = 0; i < argumentTypes.size(); ++i) {
    argumentTypes[i] = typeToKindEnum(functionType.getInput(i));
  }
  auto argumentTypesOffset = createOptionalVector(argumentTypes, fbb);

  std::vector<uint32_t> resultTypes;
  resultTypes.resize(functionType.getNumResults());
  for (int i = 0; i < resultTypes.size(); ++i) {
    resultTypes[i] = typeToKindEnum(functionType.getResult(i));
  }
  auto resultTypesOffset = createOptionalVector(resultTypes, fbb);

  iree::vm::FunctionSignatureDefBuilder fsd(fbb);
  if (argumentTypesOffset) {
    fsd.add_argument_types(argumentTypesOffset.getValue());
  }
  if (resultTypesOffset) {
    fsd.add_result_types(resultTypesOffset.getValue());
  }
  return fsd.Finish();
}

// Builds a complete BytecodeModuleDef FlatBuffer object in |fbb|.
// The order of the encoding is ordered to ensure that all metadata is at the
// front of the resulting buffer. Large read-only data and bytecode blobs always
// fill the end of the file meaning that when memory-mapping the file most will
// not need to be paged in to do the initial module preparation.
//
// To keep the actual BytecodeModuleDef and resulting parsing code simple a lot
// has been packed into the top-level table. This results in a messier function
// here during serialization but a much more trivial (and cache-friendly)
// representation at runtime.
static Offset<iree::vm::BytecodeModuleDef> buildFlatBufferModule(
    BytecodeTargetOptions targetOptions, IREE::VM::ModuleOp moduleOp,
    FlatBufferBuilder &fbb) {
  SymbolTable symbolTable(moduleOp);
  auto symbolCounts = computeModuleSymbolCounts(moduleOp);

  // Find all structural ops in the module.
  std::vector<IREE::VM::FuncOp> importFuncOps;
  std::vector<IREE::VM::ExportOp> exportFuncOps;
  std::vector<IREE::VM::FuncOp> internalFuncOps;
  std::vector<IREE::VM::RodataOp> rodataOps;
  importFuncOps.resize(symbolCounts.importFuncs);
  exportFuncOps.resize(symbolCounts.exportFuncs);
  internalFuncOps.resize(symbolCounts.internalFuncs);
  rodataOps.resize(symbolCounts.rodatas);
  for (auto &op : moduleOp.getBlock().getOperations()) {
    if (auto funcOp = dyn_cast<IREE::VM::FuncOp>(op)) {
      if (funcOp.isExternal()) {
        importFuncOps[funcOp.ordinal().getValue().getLimitedValue()] = funcOp;
      } else {
        internalFuncOps[funcOp.ordinal().getValue().getLimitedValue()] = funcOp;
      }
    } else if (auto exportOp = dyn_cast<IREE::VM::ExportOp>(op)) {
      exportFuncOps[exportOp.ordinal().getValue().getLimitedValue()] = exportOp;
    } else if (auto rodataOp = dyn_cast<IREE::VM::RodataOp>(op)) {
      rodataOps[rodataOp.ordinal().getValue().getLimitedValue()] = rodataOp;
    }
  }

  // Serialize read-only data first so that it ends up at the end of the file.
  // This is where large things like parameters live and we don't want that to
  // get paged in until it is needed.
  std::vector<Offset<Vector<uint8_t>>> rodataContentOffsets;
  rodataContentOffsets.reserve(rodataOps.size());
  for (auto rodataOp : rodataOps) {
    auto dataOffset =
        serializeConstant(rodataOp.getLoc(), rodataOp.value(), fbb);
    if (dataOffset.IsNull()) {
      rodataOp.emitOpError() << "failed to encode";
      return {};
    }
    rodataContentOffsets.push_back(dataOffset);
  }

  // Serialize function bytecode one at a time and then merge at the end.
  std::vector<std::vector<uint8_t>> bytecodeDataParts;
  std::vector<iree::vm::FunctionDescriptor> functionDescriptors;
  bytecodeDataParts.reserve(internalFuncOps.size());
  functionDescriptors.reserve(internalFuncOps.size());
  size_t totalBytecodeLength = 0;
  for (auto funcOp : internalFuncOps) {
    auto encodedFunction = BytecodeEncoder::encodeFunction(funcOp, symbolTable);
    if (!encodedFunction) {
      funcOp.emitError() << "failed to encode function bytecode";
      return {};
    }
    functionDescriptors.push_back(iree::vm::FunctionDescriptor(
        totalBytecodeLength, encodedFunction->bytecodeData.size(),
        encodedFunction->i32RegisterCount, encodedFunction->refRegisterCount));
    totalBytecodeLength += encodedFunction->bytecodeData.size();
    bytecodeDataParts.push_back(std::move(encodedFunction->bytecodeData));
  }
  // TODO(benvanik): compression? deduping?
  uint8_t *bytecodeDataPtr = nullptr;
  auto bytecodeDataOffset = fbb.CreateUninitializedVector<uint8_t>(
      totalBytecodeLength, &bytecodeDataPtr);
  size_t currentBytecodeOffset = 0;
  for (auto it : llvm::enumerate(internalFuncOps)) {
    int ordinal = it.index();
    auto data = std::move(bytecodeDataParts[ordinal]);
    std::memcpy(bytecodeDataPtr + currentBytecodeOffset, data.data(),
                data.size());
    currentBytecodeOffset += data.size();
  }

  // Serialize metadata that should be near the front of the file.
  std::vector<Offset<iree::vm::RodataSegmentDef>> rodataSegmentOffsets;
  rodataSegmentOffsets.reserve(rodataOps.size());
  for (auto rodataContentOffset : rodataContentOffsets) {
    iree::vm::RodataSegmentDefBuilder rsd(fbb);
    rsd.add_data(rodataContentOffset);
    rodataSegmentOffsets.push_back(rsd.Finish());
  }
  std::vector<Offset<iree::vm::RwdataSegmentDef>> rwdataSegmentOffsets;
  std::vector<Offset<iree::vm::ImportFunctionDef>> importFuncOffsets;
  importFuncOffsets.reserve(importFuncOps.size());
  for (auto importOp : importFuncOps) {
    auto nameOffset = fbb.CreateString(importOp.getName().str());
    auto signatureOffset = makeFunctionSignatureDef(importOp.getType(), fbb);
    iree::vm::ImportFunctionDefBuilder ifd(fbb);
    ifd.add_full_name(nameOffset);
    ifd.add_signature(signatureOffset);
    importFuncOffsets.push_back(ifd.Finish());
  }
  std::vector<Offset<iree::vm::ExportFunctionDef>> exportFuncOffsets;
  exportFuncOffsets.reserve(exportFuncOps.size());
  for (auto exportOp : exportFuncOps) {
    auto nameOffset = fbb.CreateString(exportOp.export_name().str());
    auto funcOp = symbolTable.lookup<IREE::VM::FuncOp>(exportOp.function_ref());
    auto signatureOffset = makeFunctionSignatureDef(funcOp.getType(), fbb);
    iree::vm::ExportFunctionDefBuilder efd(fbb);
    efd.add_local_name(nameOffset);
    efd.add_signature(signatureOffset);
    efd.add_internal_ordinal(funcOp.ordinal().getValue().getLimitedValue());
    exportFuncOffsets.push_back(efd.Finish());
  }
  std::vector<Offset<iree::vm::InternalFunctionDef>> internalFuncOffsets;
  if (!targetOptions.stripSymbols) {
    internalFuncOffsets.reserve(internalFuncOps.size());
    for (auto funcOp : internalFuncOps) {
      auto nameOffset = fbb.CreateString(funcOp.getName().str());
      auto signatureOffset = makeFunctionSignatureDef(funcOp.getType(), fbb);
      iree::vm::InternalFunctionDefBuilder ifd(fbb);
      ifd.add_local_name(nameOffset);
      ifd.add_signature(signatureOffset);
      internalFuncOffsets.push_back(ifd.Finish());
    }
  }

  auto functionDescriptorsOffset =
      fbb.CreateVectorOfStructs(functionDescriptors);
  auto rodataSegmentsOffset = createOptionalVector(rodataSegmentOffsets, fbb);
  auto rwdataSegmentsOffset = createOptionalVector(rwdataSegmentOffsets, fbb);
  auto internalFuncsOffset = fbb.CreateVector(internalFuncOffsets);
  auto exportFuncsOffset = fbb.CreateVector(exportFuncOffsets);
  auto importFuncsOffset = createOptionalVector(importFuncOffsets, fbb);

  Optional<Offset<iree::vm::ModuleStateDef>> moduleStateDef;
  if (symbolCounts.globalBytes || symbolCounts.globalRefs) {
    iree::vm::ModuleStateDefBuilder msd(fbb);
    msd.add_global_bytes_capacity(symbolCounts.globalBytes);
    msd.add_global_ref_count(symbolCounts.globalRefs);
    moduleStateDef = msd.Finish();
  }

  auto nameOffset = fbb.CreateString(moduleOp.sym_name().str());

  iree::vm::BytecodeModuleDefBuilder bmd(fbb);
  bmd.add_name(nameOffset);
  if (importFuncsOffset) {
    bmd.add_imported_functions(importFuncsOffset.getValue());
  }
  bmd.add_exported_functions(exportFuncsOffset);
  bmd.add_internal_functions(internalFuncsOffset);
  if (moduleStateDef) {
    bmd.add_module_state(moduleStateDef.getValue());
  }
  if (rwdataSegmentsOffset) {
    bmd.add_rwdata_segments(rwdataSegmentsOffset.getValue());
  }
  if (rodataSegmentsOffset) {
    bmd.add_rodata_segments(rodataSegmentsOffset.getValue());
  }
  bmd.add_function_descriptors(functionDescriptorsOffset);
  bmd.add_bytecode_data(bytecodeDataOffset);
  return bmd.Finish();
}

LogicalResult translateModuleToBytecode(BytecodeTargetOptions targetOptions,
                                        IREE::VM::ModuleOp moduleOp,
                                        llvm::raw_ostream &output) {
  if (failed(canonicalizeModule(targetOptions, moduleOp))) {
    return moduleOp.emitError()
           << "failed to canonicalize vm.module to a serializable form";
  }

  if (targetOptions.outputFormat == BytecodeOutputFormat::kMlirText) {
    // Run register allocation now and put the info in the IR so it's printed.
    for (auto funcOp : moduleOp.getBlock().getOps<IREE::VM::FuncOp>()) {
      if (!funcOp.empty()) {
        if (failed(ValueLiveness::annotateIR(funcOp))) {
          return funcOp.emitError() << "liveness analysis failed";
        } else if (failed(RegisterAllocation::annotateIR(funcOp))) {
          return funcOp.emitError() << "register allocation failed";
        }
      }
    }

    // Use the standard MLIR text printer.
    moduleOp.getOperation()->print(output);
    output << "\n";
    return success();
  }

  // NOTE: we order things so that all of the metadata is close to the start of
  // the module header in memory. This ensures that when we map the file only
  // the first few pages need to be accessed to get the metadata and the rest
  // can be large bulk data.
  FlatBufferBuilder fbb;
  auto moduleDef = buildFlatBufferModule(targetOptions, moduleOp, fbb);
  if (moduleDef.IsNull()) {
    return moduleOp.emitError()
           << "failed to build FlatBuffer BytecodeModuleDef";
  }

  iree::vm::FinishBytecodeModuleDefBuffer(fbb, moduleDef);
  const uint8_t *flatbufferBytes = fbb.GetBufferPointer();
  size_t flatbufferByteSize = fbb.GetSize();

  switch (targetOptions.outputFormat) {
    case BytecodeOutputFormat::kFlatBufferBinary:
      output.write(reinterpret_cast<const char *>(flatbufferBytes),
                   flatbufferByteSize);
      break;
    case BytecodeOutputFormat::kFlatBufferText: {
      flatbuffers::ToStringVisitor toStringVisitor("\n", false, "  ", false);
      flatbuffers::IterateFlatBuffer(flatbufferBytes,
                                     iree::vm::BytecodeModuleDefTypeTable(),
                                     &toStringVisitor);
      output << toStringVisitor.s << "\n";
      break;
    }
    default:
      llvm_unreachable("unimplemented output format");
  }

  return success();
}

}  // namespace iree_compiler
}  // namespace mlir