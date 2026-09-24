//===-- TeakR7HazardPass.cpp - Stale r7 in r7+offset operands -------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// On DSi hardware, an instruction with an [r7 + offset] operand that follows
// an instruction computing r7 (addv, modr...) uses the old value of r7. Found
// with examples/dsp-bench/00-asm-probe of libteak (snippet gauss_edge): the
// instruction after "addv 8, r7" read [old r7 - 3]. Normal [r7] operands
// aren't affected, nor are r7 + offset operands after "mov imm, r7".
// Emulators (teakra) don't model this.
//
// This pass inserts a nop between such instructions.
//
//===----------------------------------------------------------------------===//

#include "Teak.h"
#include "TeakInstrInfo.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
using namespace llvm;

namespace
{
    class TeakR7HazardPass : public MachineFunctionPass
    {
    public:
        static char sId;

        TeakR7HazardPass() : MachineFunctionPass(sId) { }

        bool runOnMachineFunction(MachineFunction& mf) override;

        MachineFunctionProperties getRequiredProperties() const override
        {
            return MachineFunctionProperties()
              .set(MachineFunctionProperties::Property::NoVRegs);
        }

        StringRef getPassName() const override { return "teak r7 offset hazard pass"; }
    };

    char TeakR7HazardPass::sId = 0;
}

// Instructions that write r7 without the hazard
static bool isMovImm(const MachineInstr& mi)
{
    unsigned op = mi.getOpcode();
    return op == Teak::MOV_imm16_regnob16 || op == Teak::MOV_imm8s;
}

static bool writesR7(const MachineInstr& mi, const TargetRegisterInfo* tri)
{
    return !isMovImm(mi) && mi.modifiesRegister(Teak::R7, tri);
}

static bool usesR7Offset(const MachineInstr& mi, const TargetInstrInfo* tii)
{
    return tii->getName(mi.getOpcode()).contains("r7offset");
}

// First real instruction at or after "it" in the block, or null
static const MachineInstr* nextReal(MachineBasicBlock::const_iterator it,
                                    const MachineBasicBlock& mbb)
{
    for (; it != mbb.end(); ++it)
    {
        if (!it->isMetaInstruction())
            return &*it;
    }
    return nullptr;
}

bool TeakR7HazardPass::runOnMachineFunction(MachineFunction& mf)
{
    const TargetInstrInfo* tii = mf.getSubtarget().getInstrInfo();
    const TargetRegisterInfo* tri = mf.getSubtarget().getRegisterInfo();
    bool changed = false;

    for (auto& mbb : mf)
    {
        for (auto it = mbb.begin(); it != mbb.end(); ++it)
        {
            if (it->isMetaInstruction() || !writesR7(*it, tri))
                continue;

            bool hazard = false;
            const MachineInstr* next = nextReal(std::next(it), mbb);
            if (next)
                hazard = usesR7Offset(*next, tii);
            else
            {
                // End of the block: check the blocks that can follow
                for (const MachineBasicBlock* succ : mbb.successors())
                {
                    const MachineInstr* first = nextReal(succ->begin(), *succ);
                    if (first && usesR7Offset(*first, tii))
                        hazard = true;
                }
            }

            if (hazard)
            {
                BuildMI(mbb, std::next(it), it->getDebugLoc(), tii->get(Teak::NOP));
                changed = true;
            }
        }
    }

    return changed;
}

FunctionPass* llvm::createTeakR7HazardPass()
{
    return new TeakR7HazardPass();
}
