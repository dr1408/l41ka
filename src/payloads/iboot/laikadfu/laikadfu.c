// Copyright (c) 0cyn All Rights Reserved

//

#include "laikadfu_offsets.h"
#include "usb/synopsys.h"
#include "usb/usb.h"

#pragma clang section text="__TEXT,__laikadfu"

extern __attribute__((noreturn)) void laikadfu_spin();

__attribute__((section("__DATA,__laikadfu_cfg"), used)) volatile const struct laikadfu_offsets gLaikaDFUOffsets = {
	.magic = LAIKADFU_OFFSETS_MAGIC,
};

__attribute__((noreturn)) void laikadfu_main(
	uint64_t next_stage, uint64_t boot_args, uintptr_t scratch)
{
	synopsys_initialize();
	laikadfu_usb_enumerate(scratch);
	laikadfu_usb_receive(next_stage, boot_args, scratch);

	laikadfu_spin();
}
