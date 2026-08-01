// Copyright (c) 0cyn All Rights Reserved

.text
.p2align 2

.equ PATCHFINDER_SEARCH_LEN, 0x200000

// not relevant because we run before it
// but iboot will copy a copy function here on startup and zero it later
// |
// V
.equ TEMP_TTBR0, 0x19c000000
.equ TEMP_L3, 0x19c004000
.equ TEMP_STACK, 0x19c008000
.equ TEMP_STACK_TOP, 0x19c00c000

.equ L3_PAGE_SHIFT, 14
.equ L3_INDEX_MASK, 0x7ff
.equ L2_BLOCK_SHIFT, 25

.equ PTE_AF_INNER, 0x703
.equ PTE_EL1_RO, 0x80
.equ PTE_PXN, (1 << 53)
.equ PTE_UXN, (1 << 54)

.globl _patchfinder_mmu_enable
_patchfinder_mmu_enable: // (void* target, void* our_page) -> sp
    movz x9, #(TEMP_TTBR0 & 0xffff)
    movk x9, #((TEMP_TTBR0 >> 16) & 0xffff), lsl #16
    movk x9, #((TEMP_TTBR0 >> 32) & 0xffff), lsl #32
    mov x10, x9
    add x11, x9, #0x8000
1:
    stp xzr, xzr, [x10], #16
    cmp x10, x11
    b.lo 1b

    movz x12, #(TEMP_L3 & 0xffff)
    movk x12, #((TEMP_L3 >> 16) & 0xffff), lsl #16
    movk x12, #((TEMP_L3 >> 32) & 0xffff), lsl #32
    orr x13, x12, #3
    lsr x14, x0, #L2_BLOCK_SHIFT
    and x14, x14, #L3_INDEX_MASK
    str x13, [x9, x14, lsl #3]

    // map iboot
    lsr x10, x0, #L3_PAGE_SHIFT
    lsl x10, x10, #L3_PAGE_SHIFT
    add x11, x0, #PATCHFINDER_SEARCH_LEN
    add x11, x11, #0x3000
    add x11, x11, #0xfff
    lsr x11, x11, #L3_PAGE_SHIFT
    lsl x11, x11, #L3_PAGE_SHIFT
    movz x15, #(PTE_AF_INNER & 0xffff)
    movk x15, #((PTE_PXN >> 48) & 0xffff), lsl #48
    orr x15, x15, x10
    movz x16, #(PTE_UXN >> 48), lsl #48
    orr x15, x15, x16
2:
    lsr x14, x10, #L3_PAGE_SHIFT
    and x14, x14, #L3_INDEX_MASK
    str x15, [x12, x14, lsl #3]
    add x10, x10, #0x4000
    add x15, x15, #0x4000
    cmp x10, x11
    b.lo 2b

    // map a stack 4 the patchfinder
    movz x10, #(TEMP_STACK & 0xffff)
    movk x10, #((TEMP_STACK >> 16) & 0xffff), lsl #16
    movk x10, #((TEMP_STACK >> 32) & 0xffff), lsl #32
    movz x15, #(PTE_AF_INNER & 0xffff)
    movk x15, #((PTE_PXN >> 48) & 0xffff), lsl #48
    orr x15, x15, x10
    orr x15, x15, x16
    lsr x14, x10, #L3_PAGE_SHIFT
    and x14, x14, #L3_INDEX_MASK
    str x15, [x12, x14, lsl #3]

    // map ourselves as exec
    lsr x10, x1, #L3_PAGE_SHIFT
    lsl x10, x10, #L3_PAGE_SHIFT
    movz x15, #((PTE_AF_INNER | PTE_EL1_RO) & 0xffff)
    orr x15, x15, x10
    orr x15, x15, x16
    lsr x14, x10, #L3_PAGE_SHIFT
    and x14, x14, #L3_INDEX_MASK
    str x15, [x12, x14, lsl #3]

    dsb sy
    tlbi vmalle1
    ic iallu
    dsb sy
    isb

    msr TTBR0_EL1, x9
    movz x10, #0xb51f
    movk x10, #0x0080, lsl #16
    movk x10, #0x0001, lsl #32
    msr TCR_EL1, x10
    mov x10, #0xff
    msr MAIR_EL1, x10
    isb

    // mmu on!
    movz x10, #0x520d
    movk x10, #0x048d, lsl #16
    msr SCTLR_EL1, x10
    isb

    movz x0, #(TEMP_STACK_TOP & 0xffff)
    movk x0, #((TEMP_STACK_TOP >> 16) & 0xffff), lsl #16
    movk x0, #((TEMP_STACK_TOP >> 32) & 0xffff), lsl #32
    ret


.globl _patchfinder_mmu_disable
_patchfinder_mmu_disable: // (void* target_base)
    lsr x9, x0, #6
    lsl x9, x9, #6
    add x10, x0, #PATCHFINDER_SEARCH_LEN
    add x10, x10, #63
    lsr x10, x10, #6
    lsl x10, x10, #6
3:
    dc civac, x9
    add x9, x9, #64
    cmp x9, x10
    b.lo 3b
    dsb sy
    ic iallu
    dsb sy
    isb

    msr SCTLR_EL1, xzr
    isb
    msr TTBR0_EL1, xzr
    msr TCR_EL1, xzr
    msr MAIR_EL1, xzr
    tlbi vmalle1
    ic iallu
    dsb sy
    isb
    ret
