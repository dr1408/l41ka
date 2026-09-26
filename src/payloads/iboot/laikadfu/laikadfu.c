// Copyright (c) 0cyn All Rights Reserved

//

#include "laikadfu_offsets.h"
#include "diag.h"
#include "usb/synopsys.h"
#include "usb/usb.h"

#pragma clang section text="__TEXT,__laikadfu"

extern __attribute__((noreturn)) void laikadfu_spin();

__attribute__((section("__DATA,__laikadfu_diag"), used)) volatile struct laikadfu_diag_config gLaikaDFUDiag = {
	.magic = LAIKADFU_DIAG_MAGIC,
	.mode = LAIKADFU_DIAG_NONE,
	.checkpoint = 0,
};

__attribute__((section("__DATA,__laikadfu_cfg"), used)) volatile const struct laikadfu_offsets gLaikaDFUOffsets = {
	.magic = LAIKADFU_OFFSETS_MAGIC,
};

void laikadfu_diag_checkpoint(uint32_t checkpoint)
{
	gLaikaDFUDiag.checkpoint = checkpoint;
	if (gLaikaDFUDiag.mode == checkpoint)
		laikadfu_spin();
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
