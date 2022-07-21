//===- AIEVecInterfaces.td ---------------------------------*- tablegen -*-===//
//
// This file is licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// (c) Copyright 2022 Xilinx Inc.
//
//===----------------------------------------------------------------------===//

#ifndef AIE_AIEVECINTERFACES
#define AIE_AIEVECINTERFACES

include "mlir/IR/OpBase.td"

def AccumulatorOpInterface : OpInterface<"AccumulatorOpInterface"> {
  let description = [{
    Interface for operations that may need to deal with the
    accumulator registers.
  }];
  let cppNamespace = "::xilinx::aievec";
  let methods = [
    InterfaceMethod<
      /*desc=*/[{
        Returns whether or not an operation writes to an accumulator register.
      }],
      /*retTy=*/"bool",
      /*methodName=*/"writesToAccumulator",
      /*args*/(ins),
      /*methodBody=*/"",
      /*defaultImplementation=*/[{
        return false;
      }]
    >,
    InterfaceMethod<
      /*desc=*/[{
        Returns whether or not an operation reads from an accumulator register.
      }],
      /*retTy=*/"bool",
      /*methodName=*/"readsFromAccumulator",
      /*args*/(ins),
      /*methodBody=*/"",
      /*defaultImplementation=*/[{
        return false;
      }]
    >,
    InterfaceMethod<
      /*desc=*/[{
        Returns operands that do read from an accumulator.
      }],
      /*retTy=*/"bool",
      /*methodName=*/"getAccumulatorOperands",
      /*args*/(ins),
      /*methodBody=*/"",
      /*defaultImplementation=*/[{
        return false;
      }]
    >,
  ];
}
#endif // AIE_AIEVECINTERFACES
