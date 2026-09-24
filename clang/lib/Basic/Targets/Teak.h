//===--- Teak.h - Declare Teak target feature support -----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file declares Teak TargetInfo objects.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_LIB_BASIC_TARGETS_TEAK_H
#define LLVM_CLANG_LIB_BASIC_TARGETS_TEAK_H

#include "clang/Basic/TargetInfo.h"
#include "clang/Basic/TargetOptions.h"
#include "llvm/ADT/Triple.h"
#include "llvm/Support/Compiler.h"

namespace clang {
namespace targets {

class LLVM_LIBRARY_VISIBILITY TeakTargetInfo : public TargetInfo {
public:
  TeakTargetInfo(const llvm::Triple &Triple, const TargetOptions &)
      : TargetInfo(Triple) {
    NoAsmVariants = true;
    // Pointers are 16-bit word addresses ("p0:16:16" in the data layout).
    // With the default 32-bit width, the layout of structures containing
    // pointers didn't match the LLVM types.
    PointerWidth = PointerAlign = 16;
    LongLongAlign = 32;
    SuitableAlign = 32;
    DoubleAlign = LongDoubleAlign = 32;
    SizeType = UnsignedInt;
    PtrDiffType = SignedInt;
    IntPtrType = SignedInt;
    UseZeroLengthBitfieldAlignment = true;
    BoolWidth = 16;
    BoolAlign = 16;
    MinGlobalAlign = 16;
    resetDataLayout("E-m:e-P1-p0:16:16:16-p1:32:32:32-i1:16:16-i8:16:16-i16:16:16-i32:32:32-a:0:32-n16:40");
  }

  void getTargetDefines(const LangOptions &Opts,
                        MacroBuilder &Builder) const override;

  ArrayRef<Builtin::Info> getTargetBuiltins() const override { return None; }

  BuiltinVaListKind getBuiltinVaListKind() const override {
    return TargetInfo::VoidPtrBuiltinVaList;
  }

  const char *getClobbers() const override { return ""; }

  ArrayRef<const char *> getGCCRegNames() const override {
    static const char *const GCCRegNames[] = {
        "a0",   "a0l",  "a0h",  "a0e",  "a1",   "a1l",  "a1h",  "a1e",
        "b0",   "b0l",  "b0h",  "b0e",  "b1",   "b1l",  "b1h",  "b1e",
        "r0",   "r1",   "r2",   "r3",   "r4",   "r5",   "r6",   "r7",
        "x0",   "x1",   "y0",   "y1",   "p0",   "p1",   "pc",   "sp",
        "sv",   "lc",   "stt0", "stt1", "stt2", "mod0", "mod1", "mod2",
        "mod3", "ext0", "ext1", "ext2", "ext3"};
    return llvm::makeArrayRef(GCCRegNames);
  }

  ArrayRef<TargetInfo::GCCRegAlias> getGCCRegAliases() const override {
    return None;
  }

  bool validateAsmConstraint(const char *&Name,
                             TargetInfo::ConstraintInfo &Info) const override {
    return false;
  }
};

} // namespace targets
} // namespace clang

#endif // LLVM_CLANG_LIB_BASIC_TARGETS_TEAK_H
