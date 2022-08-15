//===- AIEVecToLLVM.cpp - AIEVec to LLVM dialect conversion ---------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "../PassDetail.h"
#include "mlir/Conversion/LLVMCommon/ConversionTarget.h"
#include "mlir/Conversion/LLVMCommon/Pattern.h"
#include "mlir/Dialect/Arithmetic/IR/Arithmetic.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/IR/TypeUtilities.h"

#include "aie/Dialect/AIEVec/AIEVecUtils.h"
#include "aie/Conversion/AIEVecToLLVM/AIEVecToLLVM.h"
#include "aie/Dialect/AIEVec/IR/AIEVecOps.h"

#include <sstream>

using namespace mlir;
using namespace xilinx;
using namespace xilinx::aievec;

namespace xilinx {
namespace aievec {

struct BufferParams {
  int32_t start;
  int32_t offsets;
  int32_t offsets_hi;
  int32_t step;
  int32_t square;
};

std::string getVectorTypeString(VectorType type) {
  std::stringstream ss;
  auto size = getVectorLaneSize(type);
  ss << "v" << size;
  if (auto intType  = type.getElementType().dyn_cast<IntegerType>()) {
    ss << "int" << intType.getWidth();
  } else if (auto floatType = type.getElementType().dyn_cast<FloatType>()) {
    ss << "float";
  }
  return ss.str();
}

bool accIsDouble(VectorType type) {
  auto lanes = getVectorLaneSize(type);
  auto accWidth = type.getElementType().cast<IntegerType>().getWidth();
  if (accWidth == 80) accWidth = 96;
  return (lanes * accWidth) > 384;
}

std::string getMulOrFMABuiltinName(Operation *op) {
  std::string baseName;
  Value lhs, rhs, result;
  if (auto mulOp = dyn_cast<xilinx::aievec::MulOp>(op)) {
    baseName = "mul";
    lhs = mulOp.lhs();
    rhs = mulOp.rhs();
    result = mulOp.result();
  } else if (auto fmaOp = dyn_cast<xilinx::aievec::FMAOp>(op)) {
    baseName = "mac";
    lhs = fmaOp.lhs();
    rhs = fmaOp.rhs();
    result = fmaOp.result();
  }
  VectorType resultType = result.getType().cast<VectorType>();
  int resultSize = getVectorLaneSize(resultType);
  std::stringstream ss;
  ss << "__builtin_aie_";
  if (auto intType = resultType.getElementType().dyn_cast<IntegerType>()) {
    ss << baseName;
    ss << resultSize << "_"
       << getVectorTypeString(lhs.getType().cast<VectorType>()) << "_"
       << getVectorTypeString(rhs.getType().cast<VectorType>()) << "_"
       << (accIsDouble(resultType) ? "bm" : "am")
       << "_sw" << intType.getWidth();
  } else if (resultType.getElementType().dyn_cast<FloatType>()) {
    ss << "vfp" << baseName;
  }
  return ss.str();
}

// Squashes the easy-to-read 16-bit square encoding into
// the 8-bit encoding the configuration register uses
int32_t encodeSquare(int32_t square) {
  int32_t out = 0;
  out |= ((square >>  0) & 0x3) << 0;
  out |= ((square >>  4) & 0x3) << 2;
  out |= ((square >>  8) & 0x3) << 4;
  out |= ((square >> 12) & 0x3) << 6;
  return out & 0xFF;
}

class SRSOpConversion : public mlir::ConvertOpToLLVMPattern<xilinx::aievec::SRSOp> {
  public:
    using ConvertOpToLLVMPattern<xilinx::aievec::SRSOp>::ConvertOpToLLVMPattern;

    static std::string getBuiltinName(xilinx::aievec::SRSOp op) {
      std::stringstream ss;
      ss << "__builtin_aie_";

      // determine the prefix
      auto sourceType = op.source().getType().cast<VectorType>();
      auto resultType = op.result().getType().cast<VectorType>();
      auto sourceElType = sourceType.getElementType().cast<IntegerType>();
      auto resultElType = resultType.getElementType().cast<IntegerType>();

      auto sourceElWidth = sourceElType.getWidth();
      auto resultElWidth = resultElType.getWidth();

      if (sourceElWidth == 48 && resultElWidth == 8) {
        ss << (resultElType.getSignedness() == IntegerType::Unsigned ? 'u' : 'b');
      } else if ((sourceElWidth == 48 && resultElWidth == 32) || (sourceElWidth == 80 && resultElWidth == 64)) {
        ss << 'l';
      }
      ss << "srs_" << getVectorTypeString(resultType);

      return ss.str();
    }

    LogicalResult
    matchAndRewrite(xilinx::aievec::SRSOp op, OpAdaptor adaptor,
                    ConversionPatternRewriter &rewriter) const override {
      // If the intrinsic declaration doesn't exist, create it
      std::string builtinName = getBuiltinName(op);
      auto module = op->getParentOfType<ModuleOp>();
      MLIRContext *context = rewriter.getContext();
      auto func = module.lookupSymbol<LLVM::LLVMFuncOp>(
        StringAttr::get(context, builtinName));
      auto shiftType = IntegerType::get(context, 8);
      auto shiftVal = rewriter.create<LLVM::ConstantOp>(op->getLoc(), shiftType, rewriter.getI8IntegerAttr(op.shift()));

      if (!func) {
        OpBuilder::InsertionGuard guard(rewriter);
        rewriter.setInsertionPointToStart(module.getBody());
        func = rewriter.create<LLVM::LLVMFuncOp>(
            rewriter.getUnknownLoc(), builtinName,
            LLVM::LLVMFunctionType::get(op.result().getType(),
                                        {op.source().getType(),
                                         shiftType})
                                       );
        rewriter.setInsertionPoint(op);
      }

      // Create a constant for the shift value
      rewriter.replaceOpWithNewOp<LLVM::CallOp>(op, func, ValueRange{op.source(), shiftVal});
      return success();
    }
};

class MulOpConversion : public mlir::ConvertOpToLLVMPattern<xilinx::aievec::MulOp> {
  public:
    using ConvertOpToLLVMPattern<xilinx::aievec::MulOp>::ConvertOpToLLVMPattern;

    LogicalResult
    matchAndRewrite(xilinx::aievec::MulOp op, OpAdaptor adaptor,
                    ConversionPatternRewriter &rewriter) const override {
      auto module = op->getParentOfType<ModuleOp>();
      MLIRContext *context = rewriter.getContext();

      auto startType = IntegerType::get(context, 32);
      auto offsetsType = VectorType::get({2}, IntegerType::get(context, 32));
      auto confType = VectorType::get({2}, IntegerType::get(context, 32));

      // If the intrinsic declaration doesn't exist, create it
      std::string builtinName = getMulOrFMABuiltinName(op);
      auto func = module.lookupSymbol<LLVM::LLVMFuncOp>(
        StringAttr::get(context, builtinName));

      if (!func) {
        OpBuilder::InsertionGuard guard(rewriter);
        rewriter.setInsertionPointToStart(module.getBody());
        func = rewriter.create<LLVM::LLVMFuncOp>(
            rewriter.getUnknownLoc(), builtinName,
            LLVM::LLVMFunctionType::get(op.result().getType(),
                                        {op.lhs().getType(),
                                         op.rhs().getType(),
                                         startType, /* xstart */
                                         startType, /* ystart */
                                         startType, /* zstart */
                                         offsetsType, /* xoffsets */
                                         offsetsType, /* zoffsets */
                                         confType})
                                       );
        rewriter.setInsertionPoint(op);
      }

      // Parse the string attribute values
      BufferParams x = {};
      BufferParams z = {};
      op.xstart().getAsInteger(0, x.start);
      op.xoffsets().getAsInteger(0, x.offsets);
      op.xoffsets_hi().getAsInteger(0, x.offsets_hi);
      op.xstep().getAsInteger(0, x.step);
      op.xsquare().getAsInteger(0, x.square);
      op.zstart().getAsInteger(0, z.start);
      op.zoffsets().getAsInteger(0, z.offsets);
      op.zoffsets_hi().getAsInteger(0, z.offsets_hi);
      op.zstep().getAsInteger(0, z.step);
      op.zsquare().getAsInteger(0, z.square);

      // Encode the configuration register
      int32_t conf[2] = {0,0};
      conf[0] |= ((x.step & 0x3F) << 0) | ((z.step & 0x3F) << 8);
      conf[1] |= (encodeSquare(x.square) << 0) | (encodeSquare(z.square) << 8);

      // Create the constants and replace the op
      auto xstartVal = rewriter.create<LLVM::ConstantOp>(op->getLoc(), startType, rewriter.getI32IntegerAttr(x.start));
      auto ystartVal = rewriter.create<LLVM::ConstantOp>(op->getLoc(), startType, rewriter.getI32IntegerAttr(0));
      auto zstartVal = rewriter.create<LLVM::ConstantOp>(op->getLoc(), startType, rewriter.getI32IntegerAttr(z.start));
      auto xoffsetsVal = rewriter.create<LLVM::ConstantOp>(op->getLoc(), offsetsType, rewriter.getI32VectorAttr({x.offsets, x.offsets_hi}));
      auto zoffsetsVal = rewriter.create<LLVM::ConstantOp>(op->getLoc(), offsetsType, rewriter.getI32VectorAttr({z.offsets, z.offsets_hi}));
      auto confVal = rewriter.create<LLVM::ConstantOp>(op->getLoc(), confType, rewriter.getI32VectorAttr({conf[0], conf[1]}));
      rewriter.replaceOpWithNewOp<LLVM::CallOp>(op, func, ValueRange{op.lhs(), op.rhs(), xstartVal, ystartVal, zstartVal, xoffsetsVal, zoffsetsVal, confVal});
      return success();
    }
};

class FMAOpConversion : public mlir::ConvertOpToLLVMPattern<xilinx::aievec::FMAOp> {
  public:
    using ConvertOpToLLVMPattern<xilinx::aievec::FMAOp>::ConvertOpToLLVMPattern;

    LogicalResult
    matchAndRewrite(xilinx::aievec::FMAOp op, OpAdaptor adaptor,
                    ConversionPatternRewriter &rewriter) const override {
      auto module = op->getParentOfType<ModuleOp>();
      MLIRContext *context = rewriter.getContext();

      auto startType = IntegerType::get(context, 32);
      auto offsetsType = VectorType::get({2}, IntegerType::get(context, 32));
      auto confType = VectorType::get({2}, IntegerType::get(context, 32));

      // If the intrinsic declaration doesn't exist, create it
      std::string builtinName = getMulOrFMABuiltinName(op);
      auto func = module.lookupSymbol<LLVM::LLVMFuncOp>(
        StringAttr::get(context, builtinName));

      if (!func) {
        OpBuilder::InsertionGuard guard(rewriter);
        rewriter.setInsertionPointToStart(module.getBody());
        func = rewriter.create<LLVM::LLVMFuncOp>(
            rewriter.getUnknownLoc(), builtinName,
            LLVM::LLVMFunctionType::get(op.result().getType(),
                                        {op.lhs().getType(),
                                         op.rhs().getType(),
                                         op.acc().getType(),
                                         startType, /* xstart */
                                         startType, /* ystart */
                                         startType, /* zstart */
                                         offsetsType, /* xoffsets */
                                         offsetsType, /* zoffsets */
                                         confType})
                                       );
        rewriter.setInsertionPoint(op);
      }

      // Parse the string attribute values
      BufferParams x = {};
      BufferParams z = {};
      op.xstart().getAsInteger(0, x.start);
      op.xoffsets().getAsInteger(0, x.offsets);
      op.xoffsets_hi().getAsInteger(0, x.offsets_hi);
      op.xstep().getAsInteger(0, x.step);
      op.xsquare().getAsInteger(0, x.square);
      op.zstart().getAsInteger(0, z.start);
      op.zoffsets().getAsInteger(0, z.offsets);
      op.zoffsets_hi().getAsInteger(0, z.offsets_hi);
      op.zstep().getAsInteger(0, z.step);
      op.zsquare().getAsInteger(0, z.square);

      // Encode the configuration register
      int32_t conf[2] = {0,0};
      conf[0] |= ((x.step & 0x3F) << 0) | ((z.step & 0x3F) << 8);
      conf[1] |= (encodeSquare(x.square) << 0) | (encodeSquare(z.square) << 8);
      conf[1] |= op.fmsub() << 17;

      // Create the constants and replace the op
      auto xstartVal = rewriter.create<LLVM::ConstantOp>(op->getLoc(), startType, rewriter.getI32IntegerAttr(x.start));
      auto ystartVal = rewriter.create<LLVM::ConstantOp>(op->getLoc(), startType, rewriter.getI32IntegerAttr(0));
      auto zstartVal = rewriter.create<LLVM::ConstantOp>(op->getLoc(), startType, rewriter.getI32IntegerAttr(z.start));
      auto xoffsetsVal = rewriter.create<LLVM::ConstantOp>(op->getLoc(), offsetsType, rewriter.getI32VectorAttr({x.offsets, x.offsets_hi}));
      auto zoffsetsVal = rewriter.create<LLVM::ConstantOp>(op->getLoc(), offsetsType, rewriter.getI32VectorAttr({z.offsets, z.offsets_hi}));
      auto confVal = rewriter.create<LLVM::ConstantOp>(op->getLoc(), confType, rewriter.getI32VectorAttr({conf[0], conf[1]}));
      rewriter.replaceOpWithNewOp<LLVM::CallOp>(op, func, ValueRange{op.lhs(), op.rhs(), op.acc(), xstartVal, ystartVal, zstartVal, xoffsetsVal, zoffsetsVal, confVal});
      return success();
    }
};

class UPDOpConversion : public mlir::ConvertOpToLLVMPattern<xilinx::aievec::UPDOp> {
  public:
    using ConvertOpToLLVMPattern<xilinx::aievec::UPDOp>::ConvertOpToLLVMPattern;

    static std::string getBuiltinName(xilinx::aievec::UPDOp op, int loadSize) {
      auto resultType = op.result().getType().cast<VectorType>();
      std::stringstream ss;
      ss << "upd_";
      ss << (loadSize == 128   ? 'v'
             : loadSize == 256 ? 'w'
                               : 'x') << "_";
      ss << getVectorTypeString(resultType) << "_";
      // The index actually affects which builtin to call
      ss << (op.index() == 0 ? "lo" : "hi");
      return ss.str();
    }

    LogicalResult
    matchAndRewrite(xilinx::aievec::UPDOp op, OpAdaptor adaptor,
                    ConversionPatternRewriter &rewriter) const override {
      auto module = op->getParentOfType<ModuleOp>();
      MLIRContext *context = rewriter.getContext();

      // A bit more complicated: load the vector, then update result vector
      // AIE1 is capable of 128-bit on one bank and 256-bit loads on even-odd banks
      // Identify size of update
      int vecSizeInBits = getVectorSizeInBits(op.result().getType().cast<VectorType>());

      auto ptr = this->getStridedElementPtr(op->getLoc(),
          op.source().getType().cast<MemRefType>(),
          adaptor.source(),
          adaptor.indices(),
          rewriter);

      if (vecSizeInBits <= 256) {
        // total <=256-bit updates are much simpler:
        // we can do a direct load into the vector register
        // look at the indices to calculate the address
        // thanks vector for telling me about this function :)
        auto vectorPtrType = LLVM::LLVMPointerType::get(
          op.result().getType().cast<VectorType>(),
          op.source().getType().cast<MemRefType>().getMemorySpaceAsInt());
        auto castedPtr = rewriter.create<LLVM::BitcastOp>(op->getLoc(),
          vectorPtrType,
          ptr);
        rewriter.replaceOpWithNewOp<LLVM::LoadOp>(op, castedPtr, 1);
      } else {
        // total >256-bit updates will require upd ops to fill the whole vector
        // each UDP op represents one of these 256-bit loads and updates

        // determine the load size
        // TODO: no examples of 1024-bit output vectors: doesn't feel right
        // to attempt a 512-bit load to do an update like this
        int loadSize = vecSizeInBits == 256   ? 128
                       : vecSizeInBits == 512 ? 256
                                              : 512;

        // Create a vectorType for the load proper
        // Load half of the final result vector
        auto resultType = op.result().getType().cast<VectorType>();
        int lanes = getVectorLaneSize(resultType);
        auto loadType = VectorType::get({(int64_t) lanes/2}, resultType.getElementType());

        // Load the vector
        auto vectorPtrType = LLVM::LLVMPointerType::get(
          loadType,
          op.source().getType().cast<MemRefType>().getMemorySpaceAsInt());
        auto castedPtr = rewriter.create<LLVM::BitcastOp>(op->getLoc(),
          vectorPtrType,
          ptr);
        auto loadValue = rewriter.create<LLVM::LoadOp>(op->getLoc(), castedPtr, 1);

        // Get set up for the builtin
        std::string builtinName = getBuiltinName(op, loadSize);

        // If the intrinsic declaration doesn't exist, create it
        auto func = module.lookupSymbol<LLVM::LLVMFuncOp>(
          StringAttr::get(context, builtinName));

        if (!func) {
          OpBuilder::InsertionGuard guard(rewriter);
          rewriter.setInsertionPointToStart(module.getBody());
          func = rewriter.create<LLVM::LLVMFuncOp>(
              rewriter.getUnknownLoc(), builtinName,
              LLVM::LLVMFunctionType::get(resultType,
                                          {resultType,
                                          loadType})
                                         );
          rewriter.setInsertionPoint(op);
        }

        Value destValue;
        if (op.vector()) {
          destValue = op.vector();
        } else {
          // If this UPD is not working off of an existing destination vector,
          // create an undefined vector as the destination
          //destValue = rewriter.create<LLVM::UndefOp>(op->getLoc(), resultType);

          std::stringstream ss;
          ss << "__builtin_aie_" << getVectorTypeString(resultType) << "undef";
          std::string builtinName = ss.str();

          auto func = module.lookupSymbol<LLVM::LLVMFuncOp>(
                StringAttr::get(rewriter.getContext(), builtinName));

          if (!func) {
            OpBuilder::InsertionGuard guard(rewriter);
            rewriter.setInsertionPointToStart(module.getBody());
            func = rewriter.create<LLVM::LLVMFuncOp>(
                rewriter.getUnknownLoc(), builtinName,
                LLVM::LLVMFunctionType::get(resultType, {}));
            rewriter.setInsertionPoint(op);
          }
          destValue = rewriter.create<LLVM::CallOp>(op->getLoc(), func, ValueRange{})->getOpResult(0);
        }

        // Create our call
        rewriter.replaceOpWithNewOp<LLVM::CallOp>(op, func, ValueRange{destValue, loadValue});
      }

      return success();

    }
};

void populateAIEVecToLLVMConversionPatterns(mlir::LLVMTypeConverter &converter,
                                            mlir::RewritePatternSet &patterns) {
  patterns.add<xilinx::aievec::SRSOpConversion>(converter);
  patterns.add<xilinx::aievec::MulOpConversion>(converter);
  patterns.add<xilinx::aievec::FMAOpConversion>(converter);
  patterns.add<xilinx::aievec::UPDOpConversion>(converter);
}

struct ConvertAIEVecToLLVMPass
    : public ConvertAIEVecToLLVMBase<ConvertAIEVecToLLVMPass> {
  void runOnOperation() override {
    mlir::RewritePatternSet patterns(&getContext());
    mlir::LLVMTypeConverter converter(&getContext());
    populateAIEVecToLLVMConversionPatterns(converter, patterns);

    LLVMConversionTarget target(getContext());
    if (failed(applyPartialConversion(getOperation(), target, std::move(patterns))))
      signalPassFailure();
  }
};

std::unique_ptr<mlir::OperationPass<mlir::ModuleOp>> xilinx::aievec::createConvertAIEVecToLLVMPass() {
  return std::make_unique<ConvertAIEVecToLLVMPass>();
}

} // xilinx
} // aievec
