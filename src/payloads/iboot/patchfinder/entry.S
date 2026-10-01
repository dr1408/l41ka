// Copyright (c) 0cyn All Rights Reserved

//    __/\\\________________/\\\______________________/\\\_______________________________________________/\\\\\_
//     _\///\\\_____________\///\\\___________________\/\\\_____________________________________________/\\\///__
//      ___\///\\\_____________\///\\\____________/\\\_\/\\\____________________________/\\\\\\\\\______/\\\______
//       _____\///\\\_____________\///\\\_________\///__\/\\\___________________________/\\\/////\\\__/\\\\\\\\\___
//        _______\///\\\_____________\///\\\________/\\\_\/\\\\\\\\\____________________\/\\\\\\\\\\__\////\\\//____
//         _________\///\\\_____________\///\\\_____\/\\\_\/\\\////\\\___________________\/\\\//////______\/\\\______
//          ___________\///\\\_____________\///\\\___\/\\\_\/\\\__\/\\\___________________\/\\\____________\/\\\______
//           _____________\///\\\_____________\///\\\_\/\\\_\/\\\\\\\\\___/\\\\\\\\\\\\\\\_\/\\\____________\/\\\______
//            _______________\///________________\///__\///__\/////////___\///////////////__\///_____________\///_______

.text
.p2align 2

.extern _patchfinder_mmu_enable
.extern _patchfinder_mmu_disable

.globl _start
_start:
    mov x28, lr

    adr x0, _stage2_marker_entry
    bl _stage2_set_serial_marker

    mov x0, x28
    adr x1, _start
    // cacheing + stack setup for the cpp we're about to bl to.
    bl _patchfinder_mmu_enable // ((void*)iboot, (void*)us)
    mov x19, sp
    mov sp, x0

    mov x0, x28
    bl _patchfinder
    mov x20, x0

    mov x0, x28
    bl _patchfinder_mmu_disable

    mov sp, x19
    cbz x20, 1f
    adr x0, _stage2_marker_fail
    b 2f
1:
    adr x0, _stage2_marker_ok
2:
    bl _stage2_set_serial_marker
    mov lr, x28
    mov x0, x20
    mov x28, #0
    mov x18, #0
    mov x19, #0
    mov x20, #0
    cbnz x0, __spin
    ret

__spin:
b __spin

_stage2_set_serial_marker:
    // Replace the Apple DFU serial string with a compact stage2 marker and
    // rebuild the ROM USB string descriptor. This is intentionally visible
    // from host-side `lsusb -v -d 05ac:1229` even when laikaDFU never starts.
    // x0 = NUL-terminated marker string
    ldr x9, =0x19c00bc58        // T8020 USB_SN_STR
    mov x10, x0
0:
    ldrb w11, [x10], #1
    strb w11, [x9], #1
    cbnz w11, 0b

    ldr x0, =0x19c00bc58        // USB_SN_STR
    ldr x12, =0x10000d584       // USB_DESC_MAKE_STR
    blr x12
    ldr x13, =0x19c00890a       // USB_DEV_DESC_SN_IDX
    strb w0, [x13]
    ret

_stage2_marker_entry:
    .asciz "L41kA:[S2E]"
_stage2_marker_ok:
    .asciz "L41kA:[PF0]"
_stage2_marker_fail:
    .asciz "L41kA:[PFF]"
    .p2align 2
