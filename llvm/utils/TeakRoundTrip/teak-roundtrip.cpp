//===- teak-roundtrip.cpp - Exhaustive Teak assembler round-trip check ----===//
//
// Standalone helper built on the teakra disassembler vendored in
// llvm/lib/Target/Teak/AsmParser.
//
//   teak-roundtrip gen   > all.s
//       Prints every valid opcode (with several extension-word values for
//       opcodes that take one) as assembly text.
//   teak-roundtrip dis file.bin
//       Disassembles raw little-endian words (e.g. llvm-objcopy -O binary).
//   teak-roundtrip check all.bin
//       Reads the raw .text produced by assembling all.s and checks that every
//       instruction disassembles to the same text that was generated.
//
// Build:
//   clang++ -std=c++17 -O2 -I llvm/lib/Target/Teak/AsmParser \
//     llvm/utils/TeakRoundTrip/teak-roundtrip.cpp \
//     llvm/lib/Target/Teak/AsmParser/disassembler.cpp -o teak-roundtrip
//
//===----------------------------------------------------------------------===//

#include "disassembler.h"
#include <cctype>
#include <cstdio>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

using namespace Teakra::Disassembler;

static bool isValid(std::uint16_t Opcode) {
  for (const std::string &T : GetTokenList(Opcode))
    if (T.find("[ERROR]") != std::string::npos)
      return false;
  return true;
}

static const std::uint16_t ExtValues[] = {0x0000, 0x0001, 0x7FFF, 0x8000,
                                          0xFFFF};

// Calls F(opcode, extension, hasExtension) for every generated instruction.
static void forEachInstruction(
    const std::function<void(std::uint16_t, std::uint16_t, bool)> &F) {
  for (std::uint32_t O = 0; O < 0x10000; ++O) {
    if (!isValid(O))
      continue;
    if (!NeedExpansion(O)) {
      F(O, 0, false);
      continue;
    }
    for (std::uint16_t E : ExtValues)
      F(O, E, true);
  }
}

// Text as the assembler is expected to accept it: mnemonic, then operands
// separated by commas. '?' (from "mov?") cannot be typed and is dropped.
static std::string render(std::uint16_t Opcode, std::uint16_t Ext) {
  std::vector<std::string> Tokens = GetTokenList(Opcode, Ext);
  std::string S;
  for (size_t I = 0; I < Tokens.size(); ++I) {
    if (Tokens[I].empty())
      continue;
    if (!S.empty())
      S += S.find(' ') == std::string::npos ? " " : ", ";
    S += Tokens[I];
  }
  // Product-sum instructions have no mnemonic in teakra's output.
  if (!S.empty() && S[0] == '0')
    S = "app " + S;
  std::string Out;
  for (char C : S)
    if (C != '?')
      Out += C;
  return Out;
}

// Rewrites every number as its 16-bit two's complement value and drops the
// "s7" suffix, so equivalent spellings of the same value compare equal
// ("+0x0001" / "0x0001", "-0x0001" / "0xffff", "[r7+0x0003s7]" / "[r7+0x0003]").
// Other suffixes such as "u8" are kept: they select different instructions.
static std::string normalize(const std::string &S) {
  std::string Out;
  for (size_t I = 0; I < S.size();) {
    bool Sign = (S[I] == '+' || S[I] == '-') && S.compare(I + 1, 2, "0x") == 0;
    if (!Sign && S.compare(I, 2, "0x") != 0) {
      Out += S[I++];
      continue;
    }
    bool Negative = Sign && S[I] == '-';
    size_t J = I + (Sign ? 3 : 2);
    unsigned long V = 0;
    while (J < S.size() && std::isxdigit((unsigned char)S[J]))
      V = V * 16 + std::stoul(std::string(1, S[J++]), nullptr, 16);
    if (Negative)
      V = -V;
    char Buf[16];
    std::snprintf(Buf, sizeof(Buf), "0x%04lx", V & 0xFFFF);
    Out += Buf;
    if (S.compare(J, 2, "s7") == 0)
      J += 2;
    I = J;
  }
  return Out;
}

int main(int argc, char **argv) {
  if (argc >= 2 && std::string(argv[1]) == "gen") {
    forEachInstruction([](std::uint16_t O, std::uint16_t E, bool) {
      std::printf("    %s\n", render(O, E).c_str());
    });
    return 0;
  }
  if (argc >= 3 && std::string(argv[1]) == "check") {
    std::ifstream In(argv[2], std::ios::binary);
    std::vector<unsigned char> Bytes((std::istreambuf_iterator<char>(In)),
                                     std::istreambuf_iterator<char>());
    size_t Pos = 0, Count = 0, Failures = 0, Shorter = 0;
    bool Truncated = false;
    forEachInstruction([&](std::uint16_t O, std::uint16_t E, bool) {
      if (Truncated)
        return;
      ++Count;
      auto Word = [&](size_t P) {
        return (std::uint16_t)(Bytes[P] | (Bytes[P + 1] << 8));
      };
      if (Pos + 2 > Bytes.size()) {
        Truncated = true;
        return;
      }
      std::uint16_t Got = Word(Pos);
      bool GotExt = NeedExpansion(Got);
      std::uint16_t GotE = 0;
      if (GotExt) {
        if (Pos + 4 > Bytes.size()) {
          Truncated = true;
          return;
        }
        GotE = Word(Pos + 2);
      }
      Pos += GotExt ? 4 : 2;
      std::string Want = render(O, E);
      std::string Have = isValid(Got) ? render(Got, GotE) : "<invalid>";
      // The assembler may pick a shorter encoding of the same 16-bit value:
      // a sign-extended immediate or 7-bit offset ("mov 0xffff, r0" ->
      // "mov -0x0001, r0", "[r7+0x0001]" -> "[r7+0x0001s7]").
      if (Want != Have && normalize(Want) == normalize(Have)) {
        ++Shorter;
        return;
      }
      if (Want != Have) {
        if (++Failures <= 50)
          std::printf("MISMATCH %04x:%04x '%s' -> %04x:%04x '%s'\n", O, E,
                      Want.c_str(), Got, GotE, Have.c_str());
      }
    });
    if (Truncated) {
      std::printf("ERROR: output truncated after %zu instructions\n", Count);
      return 1;
    }
    if (Pos != Bytes.size())
      std::printf("ERROR: %zu trailing bytes\n", Bytes.size() - Pos);
    std::printf("%zu instructions checked, %zu mismatches, %zu shortened to an "
                "equivalent encoding\n",
                Count, Failures, Shorter);
    return (Failures || Pos != Bytes.size()) ? 1 : 0;
  }
  if (argc >= 3 && std::string(argv[1]) == "dis") {
    std::ifstream In(argv[2], std::ios::binary);
    std::vector<unsigned char> B((std::istreambuf_iterator<char>(In)),
                                 std::istreambuf_iterator<char>());
    for (size_t P = 0; P + 1 < B.size();) {
      std::uint16_t W = B[P] | (B[P + 1] << 8);
      std::uint16_t E = 0;
      bool Ext = NeedExpansion(W) && P + 3 < B.size();
      if (Ext)
        E = B[P + 2] | (B[P + 3] << 8);
      std::printf("%05zx: %04x %s %s\n", P / 2, W, Ext ? "" : "    ",
                  isValid(W) ? render(W, E).c_str() : "<invalid>");
      if (Ext)
        std::printf("%05zx: %04x\n", P / 2 + 1, E);
      P += Ext ? 4 : 2;
    }
    return 0;
  }
  std::fprintf(stderr, "usage: %s gen | dis <file.bin> | check <file.bin>\n",
               argv[0]);
  return 2;
}
