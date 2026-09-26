// Copyright (c) 0cyn All Rights Reserved

//

#include "laikadfu_offsets.h"
#include "diag.h"
#include "usb/synopsys.h"
#include "usb/usb.h"

#pragma clang section text="__TEXT,__laikadfu"

extern __attribute__((noreturn)) void laikadfu_spin();
extern uint64_t laikadfu_counter(void);
extern void laikadfu_cache_clean(const void *addr);
extern __attribute__((noreturn)) void laikadfu_diag_crash(uint32_t code);

__attribute__((section("__DATA,__laikadfu_diag"), used)) volatile struct laikadfu_diag_config gLaikaDFUDiag = {
	.magic = LAIKADFU_DIAG_MAGIC,
	.mode = LAIKADFU_DIAG_NONE,
	.checkpoint = 0,
	.version = LAIKADFU_DIAG_CONFIG_VERSION,
	.failed = 0,
	.last_op = 0,
	.last_error = 0,
	.counter = 0,
	.arg0 = 0,
	.arg1 = 0,
	.arg2 = 0,
	.arg3 = 0,
};

__attribute__((section("__DATA,__laikadfu_cfg"), used)) volatile const struct laikadfu_offsets gLaikaDFUOffsets = {
	.magic = LAIKADFU_OFFSETS_MAGIC,
};

static void laikadfu_diag_delay_seconds(uint32_t seconds)
{
	const uint64_t counter_hz = 24000000ull;
	uint64_t deadline = laikadfu_counter() + (uint64_t)seconds * counter_hz;
	while (laikadfu_counter() < deadline)
		;
}

static void laikadfu_diag_flush(void)
{
	const char *base = (const char *)&gLaikaDFUDiag;
	laikadfu_cache_clean(base);
	laikadfu_cache_clean(base + 64);
}

void laikadfu_diag_record(uint32_t op, uint32_t error, uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3)
{
	gLaikaDFUDiag.magic = LAIKADFU_DIAG_MAGIC;
	gLaikaDFUDiag.version = LAIKADFU_DIAG_CONFIG_VERSION;
	gLaikaDFUDiag.last_op = op;
	gLaikaDFUDiag.last_error = error;
	gLaikaDFUDiag.counter = laikadfu_counter();
	gLaikaDFUDiag.arg0 = arg0;
	gLaikaDFUDiag.arg1 = arg1;
	gLaikaDFUDiag.arg2 = arg2;
	gLaikaDFUDiag.arg3 = arg3;
	laikadfu_diag_flush();
}

void laikadfu_diag_fail_record(uint32_t code, uint64_t arg0, uint64_t arg1, uint64_t arg2)
{
	gLaikaDFUDiag.failed = 1;
	laikadfu_diag_record(LAIKADFU_DIAG_OP_FAIL, code, arg0, arg1, arg2, 0);
}

void laikadfu_diag_checkpoint(uint32_t checkpoint)
{
	gLaikaDFUDiag.checkpoint = checkpoint;
	laikadfu_diag_record(LAIKADFU_DIAG_OP_CHECKPOINT, 0, checkpoint, 0, 0, 0);
	if (gLaikaDFUDiag.mode == checkpoint)
		laikadfu_spin();
	if (gLaikaDFUDiag.mode == checkpoint + LAIKADFU_DIAG_REBOOT_BASE)
	{
		/* Visible target-side checkpoint: wait N seconds, then fault out. */
		laikadfu_diag_delay_seconds(checkpoint);
		laikadfu_diag_crash(0xc100u | checkpoint);
	}
}

__attribute__((noreturn)) void laikadfu_main(
	uint64_t next_stage, uint64_t boot_args, uintptr_t scratch)
{
	laikadfu_diag_checkpoint(LAIKADFU_DIAG_ENTRY);
	synopsys_initialize();
	laikadfu_diag_checkpoint(LAIKADFU_DIAG_PRE_EP0);
	laikadfu_usb_enumerate(scratch);
	laikadfu_usb_receive(next_stage, boot_args, scratch);

	laikadfu_spin();
}
