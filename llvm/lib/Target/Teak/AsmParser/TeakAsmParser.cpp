//===-- TeakAsmParser.cpp - Parse Teak assembly to MCInst instructions ----===//
//
// The syntax is the one printed by teakra's disassembler, relaxed:
//  - numbers are arbitrary expressions (symbols, .equ/.set values, macro
//    arguments, arithmetic) and need no fixed spelling or width suffix;
//  - commas between operands are optional;
//  - a missing condition code defaults to "always";
//  - the 16-bit extension word and branch/loop targets may be relocatable.
//
// Instructions are matched against the table in TeakAsmMatcher and emitted as
// raw opcodes, with a fixup when an operand refers to a symbol.
//
//===----------------------------------------------------------------------===//

#include "MCTargetDesc/TeakFixupKinds.h"
#include "MCTargetDesc/TeakMCExpr.h"
#include "MCTargetDesc/TeakMCTargetDesc.h"
#include "TargetInfo/TeakTargetInfo.h"
#include "TeakAsmMatcher.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCParser/MCAsmLexer.h"
#include "llvm/MC/MCParser/MCParsedAsmOperand.h"
#include "llvm/MC/MCParser/MCTargetAsmParser.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/Support/TargetRegistry.h"

using namespace llvm;
using TeakAsm::InputAtom;

namespace {

/// A parsed operand atom: either a literal word/punctuation character or a
/// numeric expression.
struct TeakOperand : public MCParsedAsmOperand {
  InputAtom Atom;
  SMLoc StartLoc, EndLoc;

  TeakOperand(InputAtom A, SMLoc S, SMLoc E)
      : Atom(std::move(A)), StartLoc(S), EndLoc(E) {}

  bool isToken() const override { return Atom.Kind == InputAtom::Literal; }
  bool isImm() const override { return Atom.Kind == InputAtom::Number; }
  bool isReg() const override { return false; }
  bool isMem() const override { return false; }
  unsigned getReg() const override {
    llvm_unreachable("Teak operands are never registers");
  }
  SMLoc getStartLoc() const override { return StartLoc; }
  SMLoc getEndLoc() const override { return EndLoc; }

  void print(raw_ostream &OS) const override {
    if (isToken())
      OS << "'" << Atom.Text << "'";
    else
      OS << *Atom.Expr << Atom.Suffix;
  }

  static std::unique_ptr<TeakOperand> createLiteral(StringRef Text, SMLoc S,
                                                    SMLoc E) {
    InputAtom A;
    A.Text = Text.lower();
    return std::make_unique<TeakOperand>(std::move(A), S, E);
  }
};

class TeakAsmParser : public MCTargetAsmParser {
  const TeakAsm::InstTable &Table;

  bool MatchAndEmitInstruction(SMLoc IDLoc, unsigned &Opcode,
                               OperandVector &Operands, MCStreamer &Out,
                               uint64_t &ErrorInfo,
                               bool MatchingInlineAsm) override;
  bool ParseRegister(unsigned &RegNo, SMLoc &StartLoc, SMLoc &EndLoc) override;
  bool ParseInstruction(ParseInstructionInfo &Info, StringRef Name,
                        SMLoc NameLoc, OperandVector &Operands) override;
  bool ParseDirective(AsmToken DirectiveID) override { return true; }
  // Only used for MS-style inline assembly.
  void convertToMapAndConstraints(unsigned Kind,
                                  const OperandVector &Operands) override {}

  bool startsExpression(const AsmToken &Tok, bool AllowSign) const;
  bool parseNumber(OperandVector &Operands);
  bool parseBinOpRHS(unsigned Precedence, const MCExpr *&Res, SMLoc &EndLoc);
  bool emitMatched(SMLoc IDLoc, OperandVector &Operands, MCStreamer &Out);

public:
  TeakAsmParser(const MCSubtargetInfo &STI, MCAsmParser &Parser,
                const MCInstrInfo &MII, const MCTargetOptions &Options)
      : MCTargetAsmParser(Options, STI, MII),
        Table(TeakAsm::InstTable::get()) {
  }
};

} // end anonymous namespace

bool TeakAsmParser::startsExpression(const AsmToken &Tok,
                                     bool AllowSign) const {
  switch (Tok.getKind()) {
  case AsmToken::Integer:
  case AsmToken::LParen:
  case AsmToken::Tilde:
  case AsmToken::Exclaim:
  case AsmToken::Dot:
    return true;
  case AsmToken::Plus:
  case AsmToken::Minus:
    return AllowSign;
  case AsmToken::Identifier:
    // Registers, conditions and other operand words are never symbols.
    // A symbol with such a name can still be used inside parentheses.
    return Tok.getString() == "." || !Table.isKeyword(Tok.getString());
  default:
    return false;
  }
}

static unsigned getBinOpPrecedence(AsmToken::TokenKind K,
                                   MCBinaryExpr::Opcode &Kind) {
  switch (K) {
  case AsmToken::Plus:
    Kind = MCBinaryExpr::Add;
    return 1;
  case AsmToken::Minus:
    Kind = MCBinaryExpr::Sub;
    return 1;
  case AsmToken::Pipe:
    Kind = MCBinaryExpr::Or;
    return 2;
  case AsmToken::Caret:
    Kind = MCBinaryExpr::Xor;
    return 2;
  case AsmToken::Amp:
    Kind = MCBinaryExpr::And;
    return 2;
  case AsmToken::Star:
    Kind = MCBinaryExpr::Mul;
    return 3;
  case AsmToken::Slash:
    Kind = MCBinaryExpr::Div;
    return 3;
  case AsmToken::Percent:
    Kind = MCBinaryExpr::Mod;
    return 3;
  case AsmToken::LessLess:
    Kind = MCBinaryExpr::Shl;
    return 3;
  case AsmToken::GreaterGreater:
    Kind = MCBinaryExpr::AShr;
    return 3;
  default:
    return 0;
  }
}

/// Like the generic expression parser, but a binary operator is only consumed
/// when an operand follows it, so "0+p0+p1", "r7+3" or "a0||..." stop before
/// the operator.
bool TeakAsmParser::parseBinOpRHS(unsigned Precedence, const MCExpr *&Res,
                                  SMLoc &EndLoc) {
  while (true) {
    MCBinaryExpr::Opcode Kind = MCBinaryExpr::Add;
    unsigned TokPrec = getBinOpPrecedence(getLexer().getKind(), Kind);
    if (TokPrec == 0 || TokPrec < Precedence)
      return false;
    if (!startsExpression(getLexer().peekTok(), /*AllowSign=*/false))
      return false;
    getParser().Lex(); // Eat the operator.

    const MCExpr *RHS;
    if (getParser().parsePrimaryExpr(RHS, EndLoc))
      return true;

    MCBinaryExpr::Opcode Dummy;
    unsigned NextPrec = getBinOpPrecedence(getLexer().getKind(), Dummy);
    if (TokPrec < NextPrec && parseBinOpRHS(TokPrec + 1, RHS, EndLoc))
      return true;

    Res = MCBinaryExpr::create(Kind, Res, RHS, getContext());
  }
}

bool TeakAsmParser::parseNumber(OperandVector &Operands) {
  SMLoc S = getLexer().getLoc();
  SMLoc E;
  const MCExpr *Expr;
  if (getParser().parsePrimaryExpr(Expr, E) || parseBinOpRHS(1, Expr, E))
    return true;

  InputAtom A;
  A.Kind = InputAtom::Number;
  A.Expr = Expr;
  A.IsConstant = Expr->evaluateAsAbsolute(A.Value);

  // Optional width suffix as printed by the disassembler: 0x12u8, 3s7.
  const AsmToken &Tok = getLexer().getTok();
  if (Tok.is(AsmToken::Identifier)) {
    StringRef Id = Tok.getString();
    if (Id.size() >= 2 && (Id[0] == 'u' || Id[0] == 's') &&
        all_of(Id.drop_front(), isDigit)) {
      A.Suffix = Id.str();
      E = Tok.getEndLoc();
      getParser().Lex();
    }
  }
  Operands.push_back(std::make_unique<TeakOperand>(std::move(A), S, E));
  return false;
}

bool TeakAsmParser::ParseInstruction(ParseInstructionInfo &Info, StringRef Name,
                                     SMLoc NameLoc, OperandVector &Operands) {
  Operands.push_back(TeakOperand::createLiteral(
      Name, NameLoc, SMLoc::getFromPointer(NameLoc.getPointer() + Name.size())));

  while (!getLexer().is(AsmToken::EndOfStatement)) {
    const AsmToken &Tok = getLexer().getTok();
    switch (Tok.getKind()) {
    case AsmToken::Comma:
      getParser().Lex();
      continue;
    case AsmToken::Error:
      return true; // Already diagnosed by the lexer.
    case AsmToken::Eof:
      return Error(Tok.getLoc(), "unexpected end of file");
    case AsmToken::Plus:
    case AsmToken::Minus:
      // A sign directly followed by a value is part of the number, as in
      // "[r7-3]" or "modr r0, +2"; otherwise it is punctuation ("[r0++]").
      if (startsExpression(getLexer().peekTok(), /*AllowSign=*/false)) {
        if (parseNumber(Operands))
          return true;
        continue;
      }
      break;
    default:
      if (startsExpression(Tok, /*AllowSign=*/false)) {
        if (parseNumber(Operands))
          return true;
        continue;
      }
      break;
    }

    // Keywords and punctuation. Multi-character punctuation tokens such as
    // "->", "||" or "<<" are split so they line up with the table.
    SMLoc S = Tok.getLoc();
    StringRef Text = Tok.getString();
    if (Tok.is(AsmToken::Identifier)) {
      Operands.push_back(TeakOperand::createLiteral(Text, S, Tok.getEndLoc()));
    } else {
      for (size_t I = 0; I < Text.size(); ++I) {
        if (Text[I] == ' ' || Text[I] == '\t')
          continue;
        SMLoc CS = SMLoc::getFromPointer(S.getPointer() + I);
        Operands.push_back(TeakOperand::createLiteral(
            Text.substr(I, 1), CS, SMLoc::getFromPointer(CS.getPointer() + 1)));
      }
    }
    getParser().Lex();
  }
  getParser().Lex(); // Consume the EndOfStatement.
  return false;
}

static std::vector<InputAtom> getAtoms(const OperandVector &Operands) {
  std::vector<InputAtom> Atoms;
  for (const auto &Op : Operands)
    Atoms.push_back(static_cast<const TeakOperand &>(*Op).Atom);
  return Atoms;
}

/// Matches the atoms, retrying with an implicit "always" condition.
static TeakAsm::MatchResult matchWithDefaultCondition(
    const TeakAsm::InstTable &Table, std::vector<InputAtom> Atoms) {
  TeakAsm::MatchResult R = Table.match(Atoms);
  if (R.Success)
    return R;
  InputAtom Always;
  Always.Text = "always";
  Atoms.push_back(Always);
  TeakAsm::MatchResult R2 = Table.match(Atoms);
  return R2.Success ? R2 : R;
}

bool TeakAsmParser::MatchAndEmitInstruction(SMLoc IDLoc, unsigned &Opcode,
                                            OperandVector &Operands,
                                            MCStreamer &Out,
                                            uint64_t &ErrorInfo,
                                            bool MatchingInlineAsm) {
  return emitMatched(IDLoc, Operands, Out);
}

bool TeakAsmParser::emitMatched(SMLoc IDLoc, OperandVector &Operands,
                                MCStreamer &Out) {
  std::vector<InputAtom> Atoms = getAtoms(Operands);
  StringRef Mnemonic = Atoms[0].Text;

  // Branch, call and block-repeat targets that refer to a symbol use a
  // dedicated fixup: find the encoding with a zero target, then patch it.
  bool IsAbsBranch = Mnemonic == "br" || Mnemonic == "call";
  bool IsRelBranch = Mnemonic == "brr" || Mnemonic == "callr";
  bool IsBkrep = Mnemonic == "bkrep";
  int TargetIdx = -1;
  if (IsAbsBranch || IsRelBranch || IsBkrep) {
    for (size_t I = 1; I < Atoms.size(); ++I)
      if (Atoms[I].Kind == InputAtom::Number && !Atoms[I].IsConstant)
        TargetIdx = I;
  }

  const MCExpr *TargetExpr = nullptr;
  if (TargetIdx != -1) {
    TargetExpr = Atoms[TargetIdx].Expr;
    Atoms[TargetIdx].IsConstant = true;
    Atoms[TargetIdx].Value = 0;
    Atoms[TargetIdx].Suffix.clear();
  }

  TeakAsm::MatchResult R = matchWithDefaultCondition(Table, Atoms);
  if (!R.Success) {
    size_t Idx = R.FailIndex;
    if (Idx == 0)
      return Error(IDLoc, "unknown instruction '" + Mnemonic + "'");
    if (Idx >= Operands.size())
      return Error(IDLoc, "too few operands for instruction");
    const TeakOperand &Op = static_cast<const TeakOperand &>(*Operands[Idx]);
    SMRange Range(Op.getStartLoc(), Op.getEndLoc());
    if (Op.isToken())
      return Error(Op.getStartLoc(), "invalid operand for instruction", Range);
    if (R.NeedConstant)
      return Error(Op.getStartLoc(),
                   "expression must be a constant for this operand", Range);
    return Error(Op.getStartLoc(),
                 "immediate value out of range or not valid for this operand",
                 Range);
  }

  MCInst Inst;
  Inst.setLoc(IDLoc);
  Inst.addOperand(MCOperand::createImm(R.Opcode));

  unsigned FixupKind = 0;
  if (TargetExpr) {
    if (IsAbsBranch) {
      FixupKind = Teak::fixup_teak_call_imm18;
    } else if (IsRelBranch) {
      FixupKind = Teak::fixup_teak_rel7;
    } else if ((R.Opcode & 0xFF00) == 0x5C00) {
      // bkrep imm8, addr16: the loop end is the address of the last word.
      FixupKind = Teak::fixup_teak_ptr_imm16;
      TargetExpr = MCBinaryExpr::createSub(
          TargetExpr, MCConstantExpr::create(1, getContext()), getContext());
    } else if ((R.Opcode & 0xFFFC) == 0x8FDC) {
      FixupKind = Teak::fixup_teak_bkrep_r6;
    } else {
      FixupKind = Teak::fixup_teak_bkrep_reg;
    }
  } else if (R.ExtensionExpr) {
    FixupKind = Teak::fixup_teak_ptr_imm16;
    TargetExpr = R.ExtensionExpr;
  }

  if (TargetExpr) {
    // Label arithmetic resolved by the assembler is counted in words.
    if (FixupKind != Teak::fixup_teak_rel7)
      TargetExpr = TeakWordExpr::create(TargetExpr, getContext());
    Inst.setOpcode(R.HasExtension ? Teak::RawAsmOpExtendedFixup
                                  : Teak::RawAsmOpFixup);
    Inst.addOperand(MCOperand::createExpr(TargetExpr));
    Inst.addOperand(MCOperand::createImm(FixupKind));
  } else if (R.HasExtension) {
    Inst.setOpcode(Teak::RawAsmOpExtended);
    Inst.addOperand(MCOperand::createImm(R.Extension));
  } else {
    Inst.setOpcode(Teak::RawAsmOp);
  }
  Out.EmitInstruction(Inst, getSTI());
  return false;
}

bool TeakAsmParser::ParseRegister(unsigned &RegNo, SMLoc &StartLoc,
                                  SMLoc &EndLoc) {
  const AsmToken &Tok = getParser().getTok();
  StartLoc = Tok.getLoc();
  EndLoc = Tok.getEndLoc();
  RegNo = 0;
  if (Tok.is(AsmToken::Identifier)) {
    const MCRegisterInfo *MRI = getContext().getRegisterInfo();
    for (unsigned Reg = 1, E = MRI->getNumRegs(); Reg < E; ++Reg) {
      if (Tok.getString().equals_lower(MRI->getName(Reg))) {
        RegNo = Reg;
        getParser().Lex();
        return false;
      }
    }
  }
  return Error(StartLoc, "invalid register name");
}

extern "C" LLVM_EXTERNAL_VISIBILITY void LLVMInitializeTeakAsmParser() {
  RegisterMCAsmParser<TeakAsmParser> X(getTheTeakTarget());
}
