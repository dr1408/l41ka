// Copyright (c) 0cyn All Rights Reserved

.section __TEXT,__laikadfu,regular,pure_instructions
.p2align 2

.globl _laikadfu_counter
_laikadfu_counter:
    isb
    mrs x0, CNTPCT_EL0
    ret

.globl _laikadfu_cache_invalidate
_laikadfu_cache_invalidate:
    dc ivac, x0
    dsb sy
    isb
    ret

.globl _laikadfu_cache_clean_invalidate
_laikadfu_cache_clean_invalidate:
    dc civac, x0
    dsb sy
    isb
    ret

.globl _laikadfu_cache_clean
_laikadfu_cache_clean:
    dc cvac, x0
    ret

.globl _laikadfu_dmb_oshst
_laikadfu_dmb_oshst:
    dmb oshst
    ret

.globl _laikadfu_sync_instructions
_laikadfu_sync_instructions:
    dsb sy
    ic iallu
    dsb sy
    isb
    ret

.globl _laikadfu_fail
_laikadfu_fail:
    mov w24, w0

.globl _laikadfu_spin
_laikadfu_spin:
    dsb sy
1:
    wfe
    b 1b

.section __DATA,__laikadfu_cfg,regular
.p2align 3
.globl _laikadfu_end
_laikadfu_end:
