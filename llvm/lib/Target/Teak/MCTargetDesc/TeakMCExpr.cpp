//===-- TeakMCExpr.cpp - Teak word-address expressions ---------------------===//

#include "TeakMCExpr.h"
#include "llvm/MC/MCAsmLayout.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/MCSymbol.h"
#include "llvm/MC/MCValue.h"

using namespace llvm;

const MCExpr *TeakWordExpr::create(const MCExpr *Expr, MCContext &Ctx) {
  if (isa<MCConstantExpr>(Expr) || isa<MCTargetExpr>(Expr))
    return Expr;
  return new (Ctx) TeakWordExpr(Expr, Ctx);
}

void TeakWordExpr::printImpl(raw_ostream &OS, const MCAsmInfo *MAI) const {
  Expr->print(OS, MAI);
}

void TeakWordExpr::visitUsedExpr(MCStreamer &Streamer) const {
  Streamer.visitUsedExpr(*Expr);
}

/// Rebuilds E with every label replaced by its word offset. Returns null if
/// some label has no known offset.
static const MCExpr *toWords(const MCExpr *E, const MCAsmLayout &Layout,
                             MCContext &Ctx, bool &HasLabel) {
  switch (E->getKind()) {
  case MCExpr::Constant:
    return E;
  case MCExpr::SymbolRef: {
    const MCSymbol &Sym = cast<MCSymbolRefExpr>(E)->getSymbol();
    if (Sym.isVariable())
      return toWords(Sym.getVariableValue(), Layout, Ctx, HasLabel);
    uint64_t Offset;
    if (!Layout.getSymbolOffset(Sym, Offset))
      return nullptr;
    HasLabel = true;
    return MCConstantExpr::create(Offset >> 1, Ctx);
  }
  case MCExpr::Unary: {
    const auto *U = cast<MCUnaryExpr>(E);
    const MCExpr *Sub = toWords(U->getSubExpr(), Layout, Ctx, HasLabel);
    return Sub ? MCUnaryExpr::create(U->getOpcode(), Sub, Ctx) : nullptr;
  }
  case MCExpr::Binary: {
    const auto *B = cast<MCBinaryExpr>(E);
    const MCExpr *L = toWords(B->getLHS(), Layout, Ctx, HasLabel);
    const MCExpr *R = L ? toWords(B->getRHS(), Layout, Ctx, HasLabel) : nullptr;
    return R ? MCBinaryExpr::create(B->getOpcode(), L, R, Ctx) : nullptr;
  }
  case MCExpr::Target:
    if (const auto *W = dyn_cast<TeakWordExpr>(E))
      return toWords(W->getSubExpr(), Layout, Ctx, HasLabel);
    return nullptr;
  }
  return nullptr;
}

bool TeakWordExpr::evaluateAsRelocatableImpl(MCValue &Res,
                                             const MCAsmLayout *Layout,
                                             const MCFixup *Fixup) const {
  if (!Expr->evaluateAsRelocatable(Res, Layout, Fixup))
    return false;
  // Relocations are converted to words later; only fully resolved label
  // arithmetic needs to be redone here.
  if (!Layout || Res.getSymA() || Res.getSymB())
    return true;
  bool HasLabel = false;
  const MCExpr *Words = toWords(Expr, *Layout, Ctx, HasLabel);
  int64_t Value;
  if (HasLabel && Words && Words->evaluateAsAbsolute(Value))
    Res = MCValue::get(Value);
  return true;
}
