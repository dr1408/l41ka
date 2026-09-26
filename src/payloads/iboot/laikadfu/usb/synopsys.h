// Copyright (c) 0cyn All Rights Reserved
#ifndef L41KA_LAIKADFU_SYNOPSYS_H
#define L41KA_LAIKADFU_SYNOPSYS_H

#include <stdint.h>

void synopsys_initialize(void);
void synopsys_shutdown(void);

void synopsys_ep0_wait_setup(uintptr_t scratch);
void synopsys_ep0_send_status(uintptr_t scratch);
void synopsys_ep0_send_data(
	uintptr_t scratch, const void *source, uint32_t available, uint32_t requested);
void synopsys_ep0_set_address(uint16_t address);
void synopsys_ep0_stall(void);

void synopsys_ep2_initialize(void);
void synopsys_ep2_prime(uintptr_t scratch);
uint32_t synopsys_ep2_wait(uintptr_t scratch);

#endif
