//===- Teak.cpp ------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Teak (TeakLite II) is the DSP of the Nintendo DSi/3DS. It addresses 16-bit
// words: section addresses and offsets are in bytes here, and relocations
// store word addresses (byte address >> 1). Implicit addends are likewise
// stored in words and converted back to bytes by getImplicitAddend.
//
//===----------------------------------------------------------------------===//

#include "InputFiles.h"
#include "Symbols.h"
#include "Target.h"
#include "lld/Common/ErrorHandler.h"
#include "llvm/Object/ELF.h"
#include "llvm/Support/Endian.h"

using namespace llvm;
using namespace llvm::object;
using namespace llvm::support::endian;
using namespace llvm::ELF;

namespace lld {
namespace elf {

namespace {
class Teak final : public TargetInfo {
public:
  Teak();
  RelExpr getRelExpr(RelType type, const Symbol &s,
                     const uint8_t *loc) const override;
  void relocateOne(uint8_t *loc, RelType type, uint64_t val) const override;
  int64_t getImplicitAddend(const uint8_t* buf, RelType type) const override;
};
} // namespace

Teak::Teak() { noneRel = R_TEAK_NONE; }

RelExpr Teak::getRelExpr(RelType type, const Symbol &s,
                        const uint8_t *loc) const {
  switch (type) {
  case R_TEAK_REL7:
    return R_PC;
  default:
    return R_ABS;
  }
}

static int64_t getRel7Field(const uint8_t *loc) {
  return SignExtend64<7>((read16le(loc) >> 4) & 0x7F);
}

void Teak::relocateOne(uint8_t *loc, RelType type, uint64_t val) const
{
	switch (type)
	{
		case R_TEAK_16:
			write16le(loc, (val >> 1) & 0xFFFF);
			break;
		case R_TEAK_REL7:
		{
			// Relative to the word after the branch.
			int64_t rel = ((int64_t)val >> 1) - 1;
			checkInt(loc, rel, 7, type);
			write16le(loc, (read16le(loc) & ~0x7F0) | ((rel & 0x7F) << 4));
			break;
		}
		case R_TEAK_CALL_IMM18:
			write16le(loc, (read16le(loc) & ~0x30) | (((val >> 17) & 3) << 4));
			write16le(loc + 2, (val >> 1) & 0xFFFF);
			break;
		case R_TEAK_PTR_IMM16:
			write16le(loc + 2, (val >> 1) & 0xFFFF);
			break;
		case R_TEAK_BKREP_REG:
			val -= 2;
			write16le(loc, (read16le(loc) & ~0x60) | (((val >> 17) & 3) << 5));
			write16le(loc + 2, (val >> 1) & 0xFFFF);
			break;
		case R_TEAK_BKREP_R6:
			val -= 2;
			write16le(loc, (read16le(loc) & ~3) | ((val >> 17) & 3));
			write16le(loc + 2, (val >> 1) & 0xFFFF);
			break;
		default:
			error(getErrorLocation(loc) + "unrecognized relocation " + toString(type));
	}
}

int64_t Teak::getImplicitAddend(const uint8_t* buf, RelType type) const
{
	switch (type)
	{
		case R_TEAK_16:
			return read16le(buf) << 1;
		case R_TEAK_REL7:
			return (getRel7Field(buf) + 1) << 1;
		case R_TEAK_CALL_IMM18:
			return (((read16le(buf) >> 4) & 3) << 17) | (read16le(buf + 2) << 1);
		case R_TEAK_PTR_IMM16:
			return read16le(buf + 2) << 1;
		case R_TEAK_BKREP_REG:
			return (((read16le(buf) >> 5) & 3) << 17) | (read16le(buf + 2) << 1) + 2;
		case R_TEAK_BKREP_R6:
			return ((read16le(buf) & 3) << 17) | (read16le(buf + 2) << 1) + 2;
		default:
			error("unrecognized relocation " + toString(type));
	}
	return 0;
}

TargetInfo *getTeakTargetInfo() {
  static Teak target;
  return &target;
}

} // namespace elf
} // namespace lld 