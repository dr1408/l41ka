// Copyright (c) 0cyn All Rights Reserved
#ifndef L41KA_LAIKADFU_USB_H
#define L41KA_LAIKADFU_USB_H

#include <stdint.h>

void laikadfu_usb_enumerate(uintptr_t scratch);
__attribute__((noreturn)) void laikadfu_usb_receive(
	uint64_t next_stage, uint64_t boot_args, uintptr_t scratch);

#endif
