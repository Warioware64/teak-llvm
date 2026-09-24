//===-- TeakAsmMatcher.h - Teak instruction table matcher -------*- C++ -*-===//
//
// Matches parsed Teak assembly against an instruction table built from
// teakra's disassembler. Every opcode is rendered to text and split into
// "atoms" (identifiers, punctuation and numbers). Numbers are matched by value
// rather than by spelling, so operands can be arbitrary constant expressions
// and the 16-bit extension word can be a relocatable expression.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_TEAK_ASMPARSER_TEAKASMMATCHER_H
#define LLVM_LIB_TARGET_TEAK_ASMPARSER_TEAKASMMATCHER_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace llvm {
class MCExpr;

namespace TeakAsm {

/// One element of a parsed instruction line.
struct InputAtom {
  enum KindTy { Literal, Number } Kind = Literal;
  /// Literal: lower-cased identifier or single punctuation character.
  std::string Text;
  /// Number: the parsed expression; IsConstant tells whether Value is valid.
  const MCExpr *Expr = nullptr;
  bool IsConstant = false;
  int64_t Value = 0;
  /// Optional explicit width suffix such as "u8" or "s7".
  std::string Suffix;
};

struct MatchResult {
  bool Success = false;
  uint16_t Opcode = 0;
  /// The opcode is followed by an extension word.
  bool HasExtension = false;
  /// The extension word value when it is a constant.
  uint16_t Extension = 0;
  /// The extension word expression when it is not a constant.
  const MCExpr *ExtensionExpr = nullptr;
  /// On failure: index of the first atom that could not be matched (equal to
  /// the number of atoms when the instruction is incomplete).
  size_t FailIndex = 0;
  /// On failure: whether a non-constant expression was rejected at FailIndex.
  bool NeedConstant = false;
};

class InstTable {
public:
  /// Returns the process-wide table, building it on first use.
  static const InstTable &get();

  /// True if the lower-cased identifier is a register, condition or other
  /// fixed operand word used by some instruction.
  bool isKeyword(StringRef Word) const;

  MatchResult match(ArrayRef<InputAtom> Atoms) const;

  struct Node;
  ~InstTable();

  /// Splits one teakra disassembler token into atoms (exposed for tests).
  static std::vector<InputAtom> atomize(StringRef Token);

private:
  InstTable();
  void build();
  void insert(const std::vector<InputAtom> &Atoms, int Slot, uint16_t Opcode,
              bool Expansion, bool Alternative);

  std::unique_ptr<Node> Root;
  std::vector<std::string> Keywords;
};

} // namespace TeakAsm
} // namespace llvm

#endif
