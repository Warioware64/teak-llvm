//===-- TeakAsmBackend.cpp - Teak Assembler Backend -------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "MCTargetDesc/TeakMCTargetDesc.h"
#include "MCTargetDesc/TeakFixupKinds.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/MC/MCAsmBackend.h"
#include "llvm/MC/MCAssembler.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCDirectives.h"
#include "llvm/MC/MCELFObjectWriter.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCFixupKindInfo.h"
#include "llvm/MC/MCMachObjectWriter.h"
#include "llvm/MC/MCObjectWriter.h"
#include "llvm/MC/MCSectionELF.h"
#include "llvm/MC/MCSectionMachO.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/MCValue.h"
//#include "llvm/Support/ELF.h"
#include "llvm/Support/ErrorHandling.h"
//#include "llvm/Support/MachO.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/Endian.h"
using namespace llvm;
using namespace llvm::support::endian;

namespace {
class TeakELFObjectWriter : public MCELFObjectTargetWriter {
public:
  TeakELFObjectWriter(uint8_t OSABI)
      : MCELFObjectTargetWriter(/*Is64Bit*/ false, OSABI, ELF::EM_TEAK,
                                /*HasRelocationAddend*/ false) {}
};

class TeakAsmBackend : public MCAsmBackend {
public:
  TeakAsmBackend(const Target &T, const StringRef TT) : MCAsmBackend(support::little) {}

  ~TeakAsmBackend() {}

  unsigned getNumFixupKinds() const override {
    return Teak::NumTargetFixupKinds;
  }

  const MCFixupKindInfo &getFixupKindInfo(MCFixupKind Kind) const override {
    const static MCFixupKindInfo Infos[Teak::NumTargetFixupKinds] =
    {
        // This table *must* be in the order that the fixup_* kinds are defined in
        // TeakFixupKinds.h.
        //
        // Name                      Offset (bits) Size (bits)     Flags
        // The 18-bit forms also patch two bits of the opcode word, which
        // can't be described here; only the extension word is listed.
        { "fixup_teak_call_imm18", 16, 16, 0 },
        { "fixup_teak_rel7", 4, 7, MCFixupKindInfo::FKF_IsPCRel },
        { "fixup_teak_ptr_imm16", 16, 16, 0 },
        { "fixup_teak_bkrep_reg", 16, 16, 0 },
        { "fixup_teak_bkrep_r6", 16, 16, 0 },
    };

    if (Kind < FirstTargetFixupKind)
        return MCAsmBackend::getFixupKindInfo(Kind);

    assert(unsigned(Kind - FirstTargetFixupKind) < getNumFixupKinds() && "Invalid kind!");
    return Infos[Kind - FirstTargetFixupKind];
  }

  void applyFixup(const MCAssembler &Asm, const MCFixup &Fixup,
                  const MCValue &Target, MutableArrayRef<char> Data,
                  uint64_t Value, bool IsResolved,
                  const MCSubtargetInfo *STI) const override;

  bool mayNeedRelaxation(const MCInst &Inst,
                         const MCSubtargetInfo &STI) const override { return false; }

  bool fixupNeedsRelaxation(const MCFixup &Fixup, uint64_t Value,
                            const MCRelaxableFragment *DF,
                            const MCAsmLayout &Layout) const override {
    return false;
  }

  void relaxInstruction(const MCInst &Inst, const MCSubtargetInfo &STI,
                        MCInst &Res) const override {}

  bool writeNopData(raw_ostream &OS, uint64_t Count) const override {
    // nop is 0x0000.
    OS.write_zeros(Count);
    return true;
  }

  unsigned getPointerSize() const { return 2; }
};
} // end anonymous namespace

void TeakAsmBackend::applyFixup(const MCAssembler &Asm, const MCFixup &Fixup,
                               const MCValue &Target,
                               MutableArrayRef<char> Data, uint64_t Value,
                               bool IsResolved,
                               const MCSubtargetInfo *STI) const {
    unsigned Offset = Fixup.getOffset();
    unsigned Kind = Fixup.getKind();
    bool IsPCRel = getFixupKindInfo(Fixup.getKind()).Flags & MCFixupKindInfo::FKF_IsPCRel;

    // Teak addresses 16-bit words. Relocated values are already in words (see
    // ELFObjectWriter) and so is resolved label arithmetic (see TeakWordExpr),
    // but a resolved PC-relative distance is in bytes, while its constant part
    // is written in words by the user.
    int64_t V = (int64_t)Value;
    if (IsResolved && IsPCRel) {
        int64_t C = Target.getConstant();
        V = ((V - C) >> 1) + C;
    }

    switch (Kind)
    {
        case Teak::fixup_teak_call_imm18:
            write16le(&Data[Offset], (read16le(&Data[Offset]) & ~0x30) | (((V >> 16) & 3) << 4));
            write16le(&Data[Offset + 2], V & 0xFFFF);
            break;
        case Teak::fixup_teak_rel7:
        {
            int64_t Rel = V - 1; // relative to the next instruction
            if (IsResolved && (Rel < -64 || Rel > 63))
                Asm.getContext().reportError(Fixup.getLoc(),
                    "relative branch target out of range (" + Twine(Rel) +
                    " words, must be within -64..63); use br/call instead");
            write16le(&Data[Offset], (read16le(&Data[Offset]) & ~0x7F0) | ((Rel & 0x7F) << 4));
            break;
        }
        case Teak::fixup_teak_ptr_imm16:
            write16le(&Data[Offset + 2], V & 0xFFFF);
            break;
        case Teak::fixup_teak_bkrep_reg:
            V--; //bkrep wants as address the last instruction word
            write16le(&Data[Offset], (read16le(&Data[Offset]) & ~0x60) | (((V >> 16) & 3) << 5));
            write16le(&Data[Offset + 2], V & 0xFFFF);
            break;
        case Teak::fixup_teak_bkrep_r6:
            V--; //bkrep wants as address the last instruction word
            write16le(&Data[Offset], (read16le(&Data[Offset]) & ~3) | ((V >> 16) & 3));
            write16le(&Data[Offset + 2], V & 0xFFFF);
            break;
        case FK_Data_1:
            Data[Offset] = V & 0xFF;
            break;
        case FK_Data_2:
            write16le(&Data[Offset], V & 0xFFFF);
            break;
        case FK_Data_4:
            // 32-bit values are stored high word first (see TeakELFStreamer).
            write16le(&Data[Offset], (V >> 16) & 0xFFFF);
            write16le(&Data[Offset + 2], V & 0xFFFF);
            break;
        default:
            Asm.getContext().reportError(Fixup.getLoc(), "unsupported fixup kind for Teak");
            break;
    }
}

namespace {

class ELFTeakAsmBackend : public TeakAsmBackend {
public:
  uint8_t OSABI;
  ELFTeakAsmBackend(const Target &T, const StringRef TT, uint8_t _OSABI)
      : TeakAsmBackend(T, TT), OSABI(_OSABI) {}

  std::unique_ptr<MCObjectTargetWriter> createObjectTargetWriter() const override {
    return createTeakELFObjectWriter(OSABI);
  }
};

} // end anonymous namespace

MCAsmBackend *llvm::createTeakAsmBackend(const Target &T, const MCSubtargetInfo &STI,
                                  const MCRegisterInfo &MRI,
                                  const llvm::MCTargetOptions &TO) {
  const uint8_t ABI = MCELFObjectTargetWriter::getOSABI(STI.getTargetTriple().getOS());
  return new ELFTeakAsmBackend(T, STI.getTargetTriple().getTriple(), ABI);
}