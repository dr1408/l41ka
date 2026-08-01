// Copyright (c) 0cyn All Rights Reserved

//    __/\\\________________/\\\________________/\\\\\\____________/\\\_________/\\\\\_______________
//     _\///\\\_____________\///\\\_____________\////\\\___________\/\\\_______/\\\///________________
//      ___\///\\\_____________\///\\\______________\/\\\___________\/\\\______/\\\____________________
//       _____\///\\\_____________\///\\\____________\/\\\___________\/\\\___/\\\\\\\\\____/\\\____/\\\_
//        _______\///\\\_____________\///\\\__________\/\\\______/\\\\\\\\\__\////\\\//____\/\\\___\/\\\_
//         _________\///\\\_____________\///\\\________\/\\\_____/\\\////\\\_____\/\\\______\/\\\___\/\\\_
//          ___________\///\\\_____________\///\\\______\/\\\____\/\\\__\/\\\_____\/\\\______\/\\\___\/\\\_
//           _____________\///\\\_____________\///\\\__/\\\\\\\\\_\//\\\\\\\/\\____\/\\\______\//\\\\\\\\\__
//            _______________\///________________\///__\/////////___\///////\//_____\///________\/////////___

// disclaimer: doesn't actually implement DFU 1.1

#include "laikadfu_offsets.h"

.section __TEXT,__laikadfu,regular,pure_instructions
.p2align 2

.equ BOOT_ARGS_PHYS_BASE_OFFSET, 0x10

// laikaDFU expects the following entry ABI:
// x0 = xnu entry
// x1 = xnu boot_args

// we will exit to our payload with the following ABI:
// x0       = boot_args                                 // legacy required
// w1       = laikaDFU version
// w2       = l41ka version
// x3       = laikaDFU offsets
// lr/x30   = xnu ptr                                   // legacy required
// sp       = base of SRAM for pongo relocation logic   // required in our pongoOS backwards compatible fork
//
// in order to keep compatibility with other loaders.

// uncomment to build a crappy exception handler that exists to prevent vbar_el1=0 from nesting on actual faults.
// #define EX_HANDLER

.globl _laikadfu
_laikadfu:
    msr DAIFSet, #0xf

#ifdef EX_HANDLER
    ldr x9, _laikadfu_walmart_exception_handler_offs
    movz w10, #0x1400, lsl #16 // b .
    str w10, [x9, #0x000]
    str w10, [x9, #0x080]
    str w10, [x9, #0x100]
    str w10, [x9, #0x180]
    str w10, [x9, #0x200]
    str w10, [x9, #0x280]
    str w10, [x9, #0x300]
    str w10, [x9, #0x380]
    str w10, [x9, #0x400]
    str w10, [x9, #0x480]
    str w10, [x9, #0x500]
    str w10, [x9, #0x580]
    str w10, [x9, #0x600]
    str w10, [x9, #0x680]
    str w10, [x9, #0x700]
    str w10, [x9, #0x780]
    mov x11, x9
    mov w12, #16
0:
    dc cvau, x11
    add x11, x11, #0x80
    subs w12, w12, #1
    b.ne 0b
    dsb sy
    ic iallu
    dsb sy
    isb
    msr vbar_el1, x9
    isb
#else
    msr vbar_el1, xzr
#endif

    ldr x2, [x1, #BOOT_ARGS_PHYS_BASE_OFFSET]
    and x2, x2, #0x7ffffffff
    orr x2, x2, #0x800000000

    adr x3, _laikadfu
    lsr x3, x3, #14
    lsl x3, x3, #14
    mov sp, x3
    b _laikadfu_main

#ifdef EX_HANDLER
_laikadfu_walmart_exception_handler_offs:
    .quad 0x19c2cf000 // this is the best i can do for something that probably wont hit anything
                      // this is absolutely not a portable value.
#endif

.globl _laikadfu_handoff
_laikadfu_handoff:
    mov x27, x0
    mov x29, x1
    dsb sy
    msr SCTLR_EL1, xzr
    isb
    bl clean_invalidate_all_caches
    tlbi vmalle1
    dsb sy
    isb
    ic iallu
    dsb sy
    isb
    mov x30, x27
    mov x0, x29
    movz w1, #(LAIKADFU_VERSION & 0xffff)
    movk w1, #((LAIKADFU_VERSION >> 16) & 0xffff), lsl #16
    movz w2, #(LAIKA_VERSION & 0xffff)
    movk w2, #((LAIKA_VERSION >> 16) & 0xffff), lsl #16
    adrp x3, _gLaikaDFUOffsets@PAGE
    add x3, x3, _gLaikaDFUOffsets@PAGEOFF
    ldr x10, [x3, #LAIKADFU_SRAM_BASE_OFFSET]
    msr spsel, #0x1 // sp = sp_el1
    msr sp_el0, xzr
    mov sp, x10
    dsb sy
    isb
    // x0       = boot_args
    // w1       = laikaDFU version
    // w2       = l41ka version
    // x3       = laikaDFU offsets
    // lr/x30   = xnu ptr
    // sp       = base of SRAM for pongo relocation logic
    br x10

clean_invalidate_all_caches:
    dmb sy
    mrs x0, CLIDR_EL1
    mov w3, #14
    mov w8, #1
    mov w10, #0
1:
    add w2, w10, w10, lsr #1
    lsr w1, w0, w2
    and w1, w1, #7
    cmp w1, #2
    b.lt 5f
    msr CSSELR_EL1, x10
    isb
    mrs x1, CCSIDR_EL1
    and w2, w1, #7
    add w2, w2, #4
    ubfx w4, w1, #3, #10
    clz w5, w4
    lsl w9, w4, w5
    lsl w16, w8, w5
3:
    ubfx w7, w1, #13, #15
    lsl w7, w7, w2
    lsl w17, w8, w2
2:
    orr w11, w10, w9
    orr w11, w11, w7
    dc cisw, x11
    subs w7, w7, w17
    b.ge 2b
    subs w9, w9, w16
    b.ge 3b
5:
    add w10, w10, #2
    cmp w3, w10
    dsb sy
    b.gt 1b
    ret
