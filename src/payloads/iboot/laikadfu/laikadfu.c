// Copyright (c) 0cyn All Rights Reserved

//

#include "laikadfu_offsets.h"
#include "diag.h"
#include "usb/synopsys.h"
#include "usb/usb.h"

#pragma clang section text="__TEXT,__laikadfu"

extern __attribute__((noreturn)) void laikadfu_spin();
extern uint64_t laikadfu_counter(void);
extern __attribute__((noreturn)) void laikadfu_diag_crash(uint32_t code);

__attribute__((section("__DATA,__laikadfu_diag"), used)) volatile struct laikadfu_diag_config gLaikaDFUDiag = {
	.magic = LAIKADFU_DIAG_MAGIC,
	.mode = LAIKADFU_DIAG_NONE,
	.checkpoint = 0,
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

void laikadfu_diag_checkpoint(uint32_t checkpoint)
{
	gLaikaDFUDiag.checkpoint = checkpoint;
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
