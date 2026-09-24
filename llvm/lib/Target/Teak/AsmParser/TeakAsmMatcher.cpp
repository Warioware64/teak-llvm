//===-- TeakAsmMatcher.cpp - Teak instruction table matcher ---------------===//
//
// See TeakAsmMatcher.h for an overview.
//
//===----------------------------------------------------------------------===//

#include "TeakAsmMatcher.h"
#include "disassembler.h"
#include "llvm/ADT/StringExtras.h"
#include <algorithm>
#include <map>
#include <functional>
#include <tuple>

using namespace llvm;
using namespace llvm::TeakAsm;

namespace {
/// Key of a numeric edge, ordered by preference when several encodings
/// accept the same value:
///  0. short forms printed without "u8" (sign-extended imm8/imm7, offsets...),
///     which behave exactly like the 16-bit form;
///  1. the 16-bit extension word;
///  2. zero-extended "u8" forms. These are not always equivalent to the 16-bit
///     form ("and 0x12u8, a0" keeps bits 8-15 of the accumulator), so they are
///     only chosen when written with "u8" or when no other form exists.
struct NumKey {
  bool Slot;
  /// The value for constant edges; for slots the value rendered with an
  /// extension word of 0 (non-zero for 18-bit addresses).
  int64_t Value;
  std::string Suffix;

  int rank() const { return Slot ? 1 : Suffix == "u8" ? 2 : 0; }
  bool operator<(const NumKey &O) const {
    return std::make_tuple(rank(), Slot, Value, std::cref(Suffix)) <
           std::make_tuple(O.rank(), O.Slot, O.Value, std::cref(O.Suffix));
  }
};
} // namespace

struct InstTable::Node {
  bool End = false;
  bool Expansion = false;
  /// The opcode is an alternative encoding teakra marks with '?'.
  bool Alternative = false;
  uint16_t Opcode = 0;
  std::map<std::string, std::unique_ptr<Node>> Literals;
  std::map<NumKey, std::unique_ptr<Node>> Numbers;
};

InstTable::InstTable() : Root(std::make_unique<Node>()) { build(); }
InstTable::~InstTable() = default;

const InstTable &InstTable::get() {
  // Built once per process; thread-safe static initialisation.
  static const InstTable Table;
  return Table;
}

bool InstTable::isKeyword(StringRef Word) const {
  return std::binary_search(Keywords.begin(), Keywords.end(), Word.lower());
}

std::vector<InputAtom> InstTable::atomize(StringRef T) {
  std::vector<InputAtom> Atoms;
  size_t I = 0, N = T.size();
  while (I < N) {
    char C = T[I];
    if (isAlpha(C) || C == '_') {
      size_t J = I;
      while (J < N && (isAlnum(T[J]) || T[J] == '_'))
        ++J;
      InputAtom A;
      A.Text = T.slice(I, J).lower();
      Atoms.push_back(std::move(A));
      I = J;
    } else if (isDigit(C) ||
               ((C == '+' || C == '-') && I + 1 < N && isDigit(T[I + 1]))) {
      bool Negative = C == '-';
      if (!isDigit(C))
        ++I;
      unsigned Radix = 10;
      if (T.substr(I).startswith("0x")) {
        Radix = 16;
        I += 2;
      }
      size_t J = I;
      while (J < N && (Radix == 16 ? isHexDigit(T[J]) : isDigit(T[J])))
        ++J;
      uint64_t V = 0;
      T.slice(I, J).getAsInteger(Radix, V);
      I = J;
      InputAtom A;
      A.Kind = InputAtom::Number;
      A.IsConstant = true;
      A.Value = Negative ? -(int64_t)V : (int64_t)V;
      if (I + 1 < N && (T[I] == 'u' || T[I] == 's') && isDigit(T[I + 1])) {
        J = I + 1;
        while (J < N && isDigit(T[J]))
          ++J;
        A.Suffix = T.slice(I, J).str();
        I = J;
      }
      Atoms.push_back(std::move(A));
    } else if (C == ',' || C == '?' || C == ' ' || C == '\t') {
      // Commas are optional in the input; '?' (from "mov?") can't be lexed.
      ++I;
    } else {
      InputAtom A;
      A.Text = std::string(1, C);
      Atoms.push_back(std::move(A));
      ++I;
    }
  }
  return Atoms;
}

static std::vector<InputAtom> atomizeAll(const std::vector<std::string> &Tokens) {
  std::vector<InputAtom> Atoms;
  for (const std::string &Tok : Tokens) {
    std::vector<InputAtom> Part = InstTable::atomize(Tok);
    Atoms.insert(Atoms.end(), Part.begin(), Part.end());
  }
  return Atoms;
}

/// Finds the atom that holds the extension word by rendering the opcode with
/// different extension values. Returns the index, or -1 if there is none.
static int findExtensionSlot(uint16_t Opcode, const std::vector<InputAtom> &A0) {
  auto A1 = atomizeAll(Teakra::Disassembler::GetTokenList(Opcode, 0x1234));
  auto A2 = atomizeAll(Teakra::Disassembler::GetTokenList(Opcode, 0xFFFF));
  if (A1.size() != A0.size() || A2.size() != A0.size())
    return -1;
  int Slot = -1;
  for (size_t I = 0; I < A0.size(); ++I) {
    if (A0[I].Kind != A1[I].Kind || A0[I].Text != A1[I].Text)
      return -1;
    if (A0[I].Kind != InputAtom::Number || A0[I].Value == A1[I].Value)
      continue;
    if (Slot != -1)
      return -1;
    int64_t D1 = A1[I].Value - A0[I].Value;
    int64_t D2 = A2[I].Value - A0[I].Value;
    if (D1 != 0x1234 || (D2 != 0xFFFF && D2 != -1))
      return -1;
    Slot = I;
  }
  return Slot;
}

void InstTable::insert(const std::vector<InputAtom> &Atoms, int Slot,
                       uint16_t Opcode, bool Expansion, bool Alternative) {
  Node *Cur = Root.get();
  for (size_t I = 0; I < Atoms.size(); ++I) {
    const InputAtom &A = Atoms[I];
    std::unique_ptr<Node> *Next;
    if (A.Kind == InputAtom::Literal) {
      Next = &Cur->Literals[A.Text];
      if (isAlpha(A.Text[0]))
        Keywords.push_back(A.Text);
    } else {
      Next = &Cur->Numbers[NumKey{(int)I == Slot, A.Value, A.Suffix}];
    }
    if (!*Next)
      *Next = std::make_unique<Node>();
    Cur = Next->get();
  }
  // Several encodings can share one spelling (don't-care bits, or the
  // equivalent "mov?" forms); the lowest opcode wins, except that regular
  // forms are preferred over "mov?" ones (codegen uses the regular ones).
  if (Cur->End && !(Cur->Alternative && !Alternative))
    return;
  Cur->End = true;
  Cur->Opcode = Opcode;
  Cur->Expansion = Expansion;
  Cur->Alternative = Alternative;
}

void InstTable::build() {
  for (uint32_t O = 0; O < 0x10000; ++O) {
    uint16_t Opcode = O;
    std::vector<std::string> Tokens = Teakra::Disassembler::GetTokenList(Opcode);
    if (std::any_of(Tokens.begin(), Tokens.end(), [](const std::string &T) {
          return T.find("[ERROR]") != std::string::npos;
        }))
      continue;
    std::vector<InputAtom> Atoms = atomizeAll(Tokens);
    bool Expansion = Teakra::Disassembler::NeedExpansion(Opcode);
    int Slot = Expansion ? findExtensionSlot(Opcode, Atoms) : -1;
    bool Alternative = Tokens[0].find('?') != std::string::npos;
    insert(Atoms, Slot, Opcode, Expansion, Alternative);

    // teakra prints the product-sum instructions without a mnemonic
    // ("0+p0+p1 a0", "acc-p0-p1 b1"); accept them with an "app" prefix, which
    // is required when the line would otherwise start with a number.
    const std::string &First = Tokens[0];
    if (StringRef(First).startswith("0+") || StringRef(First).startswith("0-") ||
        StringRef(First).startswith("acc+") ||
        StringRef(First).startswith("acc-") ||
        StringRef(First).startswith("sv+") || StringRef(First).startswith("sv-")) {
      InputAtom App;
      App.Text = "app";
      Atoms.insert(Atoms.begin(), App);
      insert(Atoms, Slot == -1 ? -1 : Slot + 1, Opcode, Expansion, Alternative);
    }
  }
  llvm::sort(Keywords);
  Keywords.erase(std::unique(Keywords.begin(), Keywords.end()), Keywords.end());
}

namespace {
struct Matcher {
  ArrayRef<InputAtom> In;
  MatchResult Cur;
  size_t Furthest = 0;
  bool NeedConstant = false;

  void reach(size_t I, bool NeedConst) {
    if (I > Furthest) {
      Furthest = I;
      NeedConstant = NeedConst;
    } else if (I == Furthest && NeedConst) {
      NeedConstant = true;
    }
  }

  bool run(const InstTable::Node *N, size_t I) {
    reach(I, false);
    if (I == In.size()) {
      if (!N->End)
        return false;
      Cur.Opcode = N->Opcode;
      Cur.HasExtension = N->Expansion;
      return true;
    }
    const InputAtom &A = In[I];
    if (A.Kind == InputAtom::Literal) {
      auto It = N->Literals.find(A.Text);
      return It != N->Literals.end() && run(It->second.get(), I + 1);
    }
    // An explicit width suffix selects the fields printed with it; fields
    // printed without a suffix are tried next (the compiler prints "s8" on
    // some fields that teakra prints bare).
    for (int Pass = 0; Pass < (A.Suffix.empty() ? 1 : 2); ++Pass) {
      for (const auto &Edge : N->Numbers) {
        const NumKey &K = Edge.first;
        if (!A.Suffix.empty() && (Pass == 0 ? A.Suffix != K.Suffix
                                            : !K.Suffix.empty()))
          continue;
        if (tryNumber(A, K, Edge.second.get(), I))
          return true;
      }
    }
    return false;
  }

  bool tryNumber(const InputAtom &A, const NumKey &K,
                 const InstTable::Node *Child, size_t I) {
    if (!K.Slot) {
      if (!A.IsConstant) {
        reach(I, true);
        return false;
      }
      // Values are also accepted as 16-bit two's complement: a field printed
      // as unsigned 0xfffd accepts -3, and a signed field accepts 0xfff8 for
      // -8 (as printed by the compiler).
      bool Wrapped =
          (K.Suffix.empty() && K.Value >= 0x8000 && K.Value <= 0xFFFF &&
           A.Value < 0 && A.Value >= -0x8000 && (A.Value & 0xFFFF) == K.Value) ||
          (K.Value < 0 && A.Value >= 0x8000 && A.Value <= 0xFFFF &&
           A.Value - 0x10000 == K.Value);
      return (A.Value == K.Value || Wrapped) && run(Child, I + 1);
    }
    if (A.IsConstant) {
      int64_t D = A.Value - K.Value;
      bool InRange = K.Value == 0 ? (A.Value >= -0x8000 && A.Value <= 0xFFFF)
                                  : (D >= 0 && D <= 0xFFFF);
      if (!InRange)
        return false;
      Cur.Extension = D & 0xFFFF;
      Cur.ExtensionExpr = nullptr;
    } else {
      // A relocatable expression can only fill a plain 16-bit slot.
      if (K.Value != 0)
        return false;
      Cur.Extension = 0;
      Cur.ExtensionExpr = A.Expr;
    }
    if (run(Child, I + 1))
      return true;
    Cur.ExtensionExpr = nullptr;
    return false;
  }
};
} // namespace

MatchResult InstTable::match(ArrayRef<InputAtom> Atoms) const {
  Matcher M;
  M.In = Atoms;
  if (M.run(Root.get(), 0)) {
    M.Cur.Success = true;
    return M.Cur;
  }
  MatchResult R;
  R.FailIndex = M.Furthest;
  R.NeedConstant = M.NeedConstant;
  return R;
}
