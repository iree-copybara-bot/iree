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

//===- IREEToSPIRV.h -------------------------------------------*- C++//-*-===//
//
// Translation of IREE statements in dispatch functions to SPIR-V.
//
//===----------------------------------------------------------------------===//
#ifndef IREE_COMPILER_TRANSLATION_SPIRV_XLATOSPIRV_IREETOSPIRV_H
#define IREE_COMPILER_TRANSLATION_SPIRV_XLATOSPIRV_IREETOSPIRV_H

#include "iree/compiler/Dialect/IREE/IR/IREEOps.h"
#include "iree/compiler/Translation/SPIRV/XLAToSPIRV/SPIRVLowering.h"

namespace mlir {
namespace iree_compiler {

/// Translation of iree.load_input operation.
class IREELoadOpSPIRVLowering final
    : public SPIRVOpLowering<IREE::LoadInputOp> {
 public:
  using SPIRVOpLowering<IREE::LoadInputOp>::SPIRVOpLowering;

  LogicalResult lowerOperation(
      Operation *op, OpBuilder &builder, AffineMap index,
      ArrayRef<Value> operands,
      TensorIndexToScalarValueMap &valueCache) const override;
};

/// Translation of iree.return operation.
class IREEReturnOpSPIRVLowering final : public SPIRVOpLowering<IREE::ReturnOp> {
 public:
  using SPIRVOpLowering<IREE::ReturnOp>::SPIRVOpLowering;

  LogicalResult lowerOperation(
      Operation *op, OpBuilder &builder,
      TensorIndexToScalarValueMap &valueCache) const override;
};

/// Translation of iree.store_output operation.
class IREEStoreOpSPIRVLowering final
    : public SPIRVOpLowering<IREE::StoreOutputOp> {
 public:
  using SPIRVOpLowering<IREE::StoreOutputOp>::SPIRVOpLowering;

  LogicalResult lowerOperation(
      Operation *op, OpBuilder &builder,
      TensorIndexToScalarValueMap &valueCache) const override;
};

/// Translation of iree.store_reduce operation. It is converted to a call to the
/// "reduction.apply" function which does the atomically updates the output.
class IREEStoreReduceOpSPIRVLowering final
    : public SPIRVOpLowering<IREE::StoreReduceOp>::SPIRVOpLowering {
 public:
  using SPIRVOpLowering<IREE::StoreReduceOp>::SPIRVOpLowering;

  LogicalResult lowerOperation(
      Operation *op, OpBuilder &builder,
      TensorIndexToScalarValueMap &valueCache) const override;
};

}  // namespace iree_compiler
}  // namespace mlir

#endif  // IREE_COMPILER_TRANSLATION_SPIRV_XLATOSPIRV_IREETOSPIRV_H