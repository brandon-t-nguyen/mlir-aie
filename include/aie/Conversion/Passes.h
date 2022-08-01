//===- Passes.h - Conversion Pass Construction and Registration -----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef AIE_CONVERSION_PASSES_H
#define AIE_CONVERSION_PASSES_H

#include "aie/Conversion/AIEVecToLLVM/AIEVecToLLVM.h"

namespace xilinx {

#define GEN_PASS_REGISTRATION
#include "aie/Conversion/Passes.h.inc"

} // namespace xilinx

#endif // AIE_CONVERSION_PASSES_H
