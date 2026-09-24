//===-- TeakMCExpr.h - Teak word-address expressions -------------*- C++ -*-===//
//
// Teak addresses 16-bit words, but MC lays sections out in bytes. Label
// references that become relocations are converted to words by the ELF writer
// and the linker; this wrapper does the same for label arithmetic that the
// assembler resolves itself (e.g. ".short end - start" or "mov end-start, r0"),
// which MC would otherwise fold to a byte count.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_TEAK_MCTARGETDESC_TEAKMCEXPR_H
#define LLVM_LIB_TARGET_TEAK_MCTARGETDESC_TEAKMCEXPR_H

#include "llvm/MC/MCExpr.h"

namespace llvm {

class TeakWordExpr : public MCTargetExpr {
  const MCExpr *Expr;
  MCContext &Ctx;

  TeakWordExpr(const MCExpr *Expr, MCContext &Ctx) : Expr(Expr), Ctx(Ctx) {}

public:
  /// Wraps Expr unless it is a constant or already wrapped.
  static const MCExpr *create(const MCExpr *Expr, MCContext &Ctx);

  const MCExpr *getSubExpr() const { return Expr; }

  void printImpl(raw_ostream &OS, const MCAsmInfo *MAI) const override;
  bool evaluateAsRelocatableImpl(MCValue &Res, const MCAsmLayout *Layout,
                                 const MCFixup *Fixup) const override;
  void visitUsedExpr(MCStreamer &Streamer) const override;
  MCFragment *findAssociatedFragment() const override {
    return Expr->findAssociatedFragment();
  }
  void fixELFSymbolsInTLSFixups(MCAssembler &Asm) const override {}

  static bool classof(const MCExpr *E) {
    return E->getKind() == MCExpr::Target;
  }
};

} // end namespace llvm

#endif
