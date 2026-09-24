# REQUIRES: teak
# RUN: llvm-mc -filetype=obj -triple=teak %s -o %t.o
# RUN: ld.lld -Ttext=0x100 --section-start=.other=0x2000 --section-start=.data=0x4000 %t.o -o %t
# RUN: llvm-objcopy -O binary -j .text %t %t.text
# RUN: od -An -v -tx2 %t.text | FileCheck %s --check-prefix=TEXT
# RUN: llvm-objcopy -O binary -j .data %t %t.data
# RUN: od -An -v -tx2 %t.data | FileCheck %s --check-prefix=DATA

# Addresses are in 16-bit words: byte address 0x2000 is word 0x1000.

    .text
    .global _start
_start:
    call far_func          # R_TEAK_CALL_IMM18 against a global symbol
    br after_loop          # R_TEAK_CALL_IMM18 against .text + offset
    mov var2, r0           # R_TEAK_PTR_IMM16 against .data + offset
    bkrep r0, loop_end     # R_TEAK_BKREP_REG: address of the last loop word
    nop
loop_end:
after_loop:
    brr near_func          # R_TEAK_REL7 across sections
    nop

    .section .text.near, "ax", %progbits
near_func:
    ret

    .section .other, "ax", %progbits
far_func:
    ret

    .data
var1:
    .short 0x1111
var2:
    .short far_func        # R_TEAK_16
    .short var2 + 1

# _start is word 0x80, after_loop/loop_end are word 0x89 (bkrep points at the
# last loop word, 0x88), far_func is word 0x1000, var2 is word 0x2001 and
# near_func (.text.near, placed after .text) is word 0x8b.

# TEXT:      41c0 1000 4180 0089 5e00 2001 5d00 0088
# TEXT-NEXT: 0000 5010 0000 4580

# DATA: 1111 1000 2002
