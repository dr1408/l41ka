// Copyright (c) 0cyn All Rights Reserved

#include "synopsys.h"

#include "../laikadfu_offsets.h"
#include "../diag.h"

#pragma clang section text="__TEXT,__laikadfu"

#define COUNTER_HZ   24000000ull
#define LINK_TIMEOUT (2ull * COUNTER_HZ)

#define GAHBCFG     0x008u
#define GUSBCFG     0x00cu
#define GRSTCTL     0x010u
#define GINTSTS     0x014u
#define GINTMSK     0x018u
#define GRXFSIZ     0x024u
#define GNPTXFSIZ   0x028u
#define DCFG        0x800u
#define DCTL        0x804u
#define DIEPMSK     0x810u
#define DOEPMSK     0x814u
#define DAINTMSK    0x81cu
#define DIEPCTL0    0x900u
#define DIEPINT0    0x908u
#define DIEPTSIZ0   0x910u
#define DIEPDMA0    0x914u
#define DOEPCTL0    0xb00u
#define DOEPINT0    0xb08u
#define DOEPTSIZ0   0xb10u
#define DOEPDMA0    0xb14u
#define DOEPCTL2    0xb40u
#define DOEPINT2    0xb48u
#define DOEPTSIZ2   0xb50u
#define DOEPDMA2    0xb54u

#define EP_ENABLE_CLEAR_NAK 0x84000000u
#define EP_ENABLE           0x80000000u
#define EP_DISABLE_SET_NAK  0x48000000u
#define EP_STALL            0x00200000u
#define EPINT_DISABLED      0x00000002u
#define OUT_DMA_SIZE        0x00080040u
#define EP0_IN_SIZE         0x00080000u
#define EP2_TRANSFER_SIZE   64u

#define DCTL_SOFT_DISCONNECT      0x00000002u
#define DCTL_SET_GLOBAL_OUT_NAK   0x00000200u
#define DCTL_CLEAR_GLOBAL_OUT_NAK 0x00000400u
#define GINTSTS_GLOBAL_OUT_NAK    0x00000080u
#define GRSTCTL_CORE_SOFT_RESET   0x00000001u
#define GRSTCTL_AHB_IDLE          0x80000000u

extern volatile const struct laikadfu_offsets gLaikaDFUOffsets;

extern uint64_t laikadfu_counter(void);
extern void laikadfu_cache_invalidate(const void *addr);
extern void laikadfu_cache_clean_invalidate(const void *addr);
extern void laikadfu_dmb_oshst(void);
extern __attribute__((noreturn)) void laikadfu_fail(uint32_t code);

static inline volatile uint32_t *reg32(uintptr_t base, uint32_t offset)
{
	return (volatile uint32_t *)(base + offset);
}

static inline uint32_t read32(uintptr_t base, uint32_t offset)
{
	return *reg32(base, offset);
}

static inline void write32(uintptr_t base, uint32_t offset, uint32_t value)
{
	*reg32(base, offset) = value;
}

static void delay_us(uint32_t microseconds)
{
	uint64_t deadline = laikadfu_counter() + (uint64_t)microseconds * 24u;
	while (laikadfu_counter() < deadline)
		;
}

static uint32_t clock_gate(uintptr_t reg, int enable)
{
	uint32_t value = read32(reg, 0);
	value = enable ? value | 0x0fu : (value & ~0x0fu) | 4u;
	write32(reg, 0, value);

	for (uint32_t timeout = 0x100000u; timeout != 0; timeout--)
	{
		value = read32(reg, 0);
		if ((value & 0x0fu) == ((value >> 4) & 0x0fu))
			break;
	}
	return value;
}

static void configure_usb_complex(void)
{
	laikadfu_diag_checkpoint(LAIKADFU_DIAG_PRE_COMPLEX);
	clock_gate(gLaikaDFUOffsets.usb_clock0, 0);
	clock_gate(gLaikaDFUOffsets.usb_clock1, 0);
	clock_gate(gLaikaDFUOffsets.usb_clock2, 0);
	delay_us(1000);
	clock_gate(gLaikaDFUOffsets.usb_clock0, 1);
	clock_gate(gLaikaDFUOffsets.usb_clock1, 1);
	clock_gate(gLaikaDFUOffsets.usb_clock2, 1);

	write32(gLaikaDFUOffsets.usb_complex, 0,
		(uint32_t)gLaikaDFUOffsets.usb_complex_control);
	write32(gLaikaDFUOffsets.usb_complex, 0x48, 0x03000088u);
	write32(gLaikaDFUOffsets.usb_complex, 0x68,
		(uint32_t)gLaikaDFUOffsets.usb_phy_cfg0);
	write32(gLaikaDFUOffsets.usb_complex, 0x6c, 0x00020c44u);
	write32(gLaikaDFUOffsets.usb_complex, 0x60,
		read32(gLaikaDFUOffsets.usb_complex, 0x60) | 1u);
	delay_us(20);
	write32(gLaikaDFUOffsets.usb_complex, 0x60,
		read32(gLaikaDFUOffsets.usb_complex, 0x60) & ~0x0cu);
	delay_us(20);
	write32(gLaikaDFUOffsets.usb_complex, 0x60,
		read32(gLaikaDFUOffsets.usb_complex, 0x60) & ~1u);
	delay_us(20);
	write32(gLaikaDFUOffsets.usb_complex, 0x64,
		read32(gLaikaDFUOffsets.usb_complex, 0x64) & ~2u);
	delay_us(1500);
	laikadfu_diag_checkpoint(LAIKADFU_DIAG_POST_COMPLEX);
}

static void dart_bypass_usb(uintptr_t base)
{
	for (uint32_t offset = 0x200; offset != 0x210; offset += sizeof(uint32_t))
		write32(base, offset, 0);

	laikadfu_dmb_oshst();
	write32(base, 0x34, (uint32_t)gLaikaDFUOffsets.dart_tlb_stream_mask);
	write32(base, 0x20, 0);
	if (gLaikaDFUOffsets.dart_wait_for_tlb != 0)
		for (uint32_t timeout = 0x100000u;
			timeout != 0 && (read32(base, 0x20) & 4u) != 0; timeout--)
			;

	write32(base, 0x40, read32(base, 0x40));
	uint32_t tcr = read32(base, 0x100);
	write32(base, 0x100, (tcr & 0xff00fe7fu) | 0x00010100u);
}

void synopsys_initialize(void)
{
	configure_usb_complex();
	dart_bypass_usb(gLaikaDFUOffsets.dart_base);
	laikadfu_diag_checkpoint(LAIKADFU_DIAG_POST_DART);

	write32(gLaikaDFUOffsets.dwc2_base, GRSTCTL, 1);
	uint32_t timeout = 0x100000u;
	while (timeout != 0 &&
		(read32(gLaikaDFUOffsets.dwc2_base, GRSTCTL) & 1u) != 0)
		timeout--;
	if (timeout == 0)
		laikadfu_fail(0xd001u);

	write32(gLaikaDFUOffsets.dwc2_base, DCTL,
		read32(gLaikaDFUOffsets.dwc2_base, DCTL) | 2u);
	timeout = 0x100000u;
	while (timeout != 0 &&
		(read32(gLaikaDFUOffsets.dwc2_base, GRSTCTL) & 0x80000000u) == 0)
		timeout--;
	if (timeout == 0)
		laikadfu_fail(0xd002u);
	laikadfu_diag_checkpoint(LAIKADFU_DIAG_POST_RESET);

	write32(gLaikaDFUOffsets.dwc2_base, GAHBCFG, 0x2eu);
	write32(gLaikaDFUOffsets.dwc2_base, GUSBCFG, 0x1408u);
	write32(gLaikaDFUOffsets.dwc2_base, DCFG, 4);
	write32(gLaikaDFUOffsets.dwc2_base, GINTMSK, 0);
	write32(gLaikaDFUOffsets.dwc2_base, DOEPMSK, 0);
	write32(gLaikaDFUOffsets.dwc2_base, DIEPMSK, 0);
	write32(gLaikaDFUOffsets.dwc2_base, DAINTMSK, 0);
	write32(gLaikaDFUOffsets.dwc2_base, DIEPINT0, 0x1fu);
	write32(gLaikaDFUOffsets.dwc2_base, DOEPINT0, 0x0fu);
	write32(gLaikaDFUOffsets.dwc2_base, GINTMSK, 0x1000u);
	write32(gLaikaDFUOffsets.dwc2_base, GINTSTS, 0x1000u);
	write32(gLaikaDFUOffsets.dwc2_base, DCTL,
		read32(gLaikaDFUOffsets.dwc2_base, DCTL) & ~2u);
	write32(gLaikaDFUOffsets.usb_phy, 0,
		read32(gLaikaDFUOffsets.usb_phy, 0) | 2u);
	laikadfu_diag_checkpoint(LAIKADFU_DIAG_POST_CONNECT);

	uint64_t deadline = laikadfu_counter() + LINK_TIMEOUT;
	while ((read32(gLaikaDFUOffsets.dwc2_base, GINTSTS) & 0x1000u) == 0
		&& laikadfu_counter() < deadline)
		;
	if ((read32(gLaikaDFUOffsets.dwc2_base, GINTSTS) & 0x1000u) == 0)
		laikadfu_fail(0xd003u);

	write32(gLaikaDFUOffsets.dwc2_base, GINTSTS, 0x1000u);
	write32(gLaikaDFUOffsets.dwc2_base, DCFG,
		read32(gLaikaDFUOffsets.dwc2_base, DCFG) & 0xfffff80fu);
	write32(gLaikaDFUOffsets.dwc2_base, GRXFSIZ, 0x021bu);
	write32(gLaikaDFUOffsets.dwc2_base, GNPTXFSIZ, 0x0010021bu);
	write32(gLaikaDFUOffsets.dwc2_base, DOEPCTL0, 0);
	write32(gLaikaDFUOffsets.dwc2_base, DIEPCTL0, 0);
	write32(gLaikaDFUOffsets.dwc2_base, DOEPMSK, 0x0du);
	write32(gLaikaDFUOffsets.dwc2_base, DIEPMSK, 0x0du);
	write32(gLaikaDFUOffsets.dwc2_base, DAINTMSK, 0x00010001u);
	write32(gLaikaDFUOffsets.dwc2_base, DIEPINT0, 0x1fu);
	write32(gLaikaDFUOffsets.dwc2_base, DOEPINT0, 0x0fu);
}

void synopsys_shutdown(void)
{
	write32(gLaikaDFUOffsets.dwc2_base, DCTL,
		read32(gLaikaDFUOffsets.dwc2_base, DCTL) | DCTL_SOFT_DISCONNECT);
	write32(gLaikaDFUOffsets.dwc2_base, GINTMSK, 0);
	write32(gLaikaDFUOffsets.dwc2_base, DIEPMSK, 0);
	write32(gLaikaDFUOffsets.dwc2_base, DOEPMSK, 0);
	write32(gLaikaDFUOffsets.dwc2_base, DAINTMSK, 0);

	uint32_t timeout = 0x100000u;
	if ((read32(gLaikaDFUOffsets.dwc2_base, DOEPCTL2) & EP_ENABLE) != 0)
	{
		write32(gLaikaDFUOffsets.dwc2_base, GINTSTS, GINTSTS_GLOBAL_OUT_NAK);
		write32(gLaikaDFUOffsets.dwc2_base, DCTL,
			read32(gLaikaDFUOffsets.dwc2_base, DCTL) |
				DCTL_SET_GLOBAL_OUT_NAK);
		while (timeout != 0 &&
			(read32(gLaikaDFUOffsets.dwc2_base, GINTSTS) &
				GINTSTS_GLOBAL_OUT_NAK) == 0)
			timeout--;
		if (timeout == 0)
			laikadfu_fail(0xd007u);

		write32(gLaikaDFUOffsets.dwc2_base, GINTSTS, GINTSTS_GLOBAL_OUT_NAK);
		write32(gLaikaDFUOffsets.dwc2_base, DOEPCTL2,
			read32(gLaikaDFUOffsets.dwc2_base, DOEPCTL2) |
				EP_DISABLE_SET_NAK);
		timeout = 0x100000u;
		while (timeout != 0 &&
			(read32(gLaikaDFUOffsets.dwc2_base, DOEPINT2) & EPINT_DISABLED) == 0)
			timeout--;
		if (timeout == 0)
			laikadfu_fail(0xd008u);

		write32(gLaikaDFUOffsets.dwc2_base, DOEPINT2, EPINT_DISABLED);
		write32(gLaikaDFUOffsets.dwc2_base, DCTL,
			read32(gLaikaDFUOffsets.dwc2_base, DCTL) |
				DCTL_CLEAR_GLOBAL_OUT_NAK);
	}

	timeout = 0x100000u;
	while (timeout != 0 &&
		(read32(gLaikaDFUOffsets.dwc2_base, GRSTCTL) & GRSTCTL_AHB_IDLE) == 0)
		timeout--;
	if (timeout == 0)
		laikadfu_fail(0xd005u);

	write32(gLaikaDFUOffsets.dwc2_base, GRSTCTL, GRSTCTL_CORE_SOFT_RESET);
	timeout = 0x100000u;
	while (timeout != 0 &&
		(read32(gLaikaDFUOffsets.dwc2_base, GRSTCTL) &
			GRSTCTL_CORE_SOFT_RESET) != 0)
		timeout--;
	if (timeout == 0)
		laikadfu_fail(0xd006u);

	delay_us(10);
	write32(gLaikaDFUOffsets.dwc2_base, DCTL,
		read32(gLaikaDFUOffsets.dwc2_base, DCTL) | DCTL_SOFT_DISCONNECT);
	write32(gLaikaDFUOffsets.usb_phy, 0,
		read32(gLaikaDFUOffsets.usb_phy, 0) & ~2u);
	delay_us(3000);
}

static void ep0_start_setup(uintptr_t scratch)
{
	laikadfu_cache_invalidate((const void *)scratch);
	write32(gLaikaDFUOffsets.dwc2_base, DOEPDMA0, (uint32_t)scratch);
	write32(gLaikaDFUOffsets.dwc2_base, DOEPTSIZ0, OUT_DMA_SIZE);
	write32(gLaikaDFUOffsets.dwc2_base, DOEPCTL0,
		read32(gLaikaDFUOffsets.dwc2_base, DOEPCTL0) | 0x80000000u);
}

void synopsys_ep0_wait_setup(uintptr_t scratch)
{
	ep0_start_setup(scratch);
	uint32_t status;
	do
		status = read32(gLaikaDFUOffsets.dwc2_base, DOEPINT0);
	while ((status & 8u) == 0);

	delay_us(2);
	laikadfu_cache_invalidate((const void *)scratch);
	write32(gLaikaDFUOffsets.dwc2_base, DOEPINT0, status);
}

void synopsys_ep0_send_status(uintptr_t scratch)
{
	write32(gLaikaDFUOffsets.dwc2_base, DIEPDMA0, (uint32_t)(scratch + 64));
	write32(gLaikaDFUOffsets.dwc2_base, DIEPTSIZ0, EP0_IN_SIZE);
	write32(gLaikaDFUOffsets.dwc2_base, DIEPCTL0,
		read32(gLaikaDFUOffsets.dwc2_base, DIEPCTL0) | EP_ENABLE_CLEAR_NAK);

	uint32_t status;
	do
		status = read32(gLaikaDFUOffsets.dwc2_base, DIEPINT0);
	while ((status & 1u) == 0);
	write32(gLaikaDFUOffsets.dwc2_base, DIEPINT0, status);
}

void synopsys_ep0_send_data(
	uintptr_t scratch, const void *source, uint32_t available, uint32_t requested)
{
	uint32_t count = available < requested ? available : requested;
	uint8_t *buffer = (uint8_t *)(scratch + 64);
	const uint8_t *bytes = source;
	for (uint32_t i = 0; i < count; i++)
		buffer[i] = bytes[i];

	laikadfu_cache_clean_invalidate(buffer);
	write32(gLaikaDFUOffsets.dwc2_base, DIEPDMA0, (uint32_t)(uintptr_t)buffer);
	write32(gLaikaDFUOffsets.dwc2_base, DIEPTSIZ0, count | EP0_IN_SIZE);
	write32(gLaikaDFUOffsets.dwc2_base, DIEPCTL0,
		read32(gLaikaDFUOffsets.dwc2_base, DIEPCTL0) | EP_ENABLE_CLEAR_NAK);

	uint32_t status;
	do
		status = read32(gLaikaDFUOffsets.dwc2_base, DIEPINT0);
	while ((status & 1u) == 0);
	write32(gLaikaDFUOffsets.dwc2_base, DIEPINT0, status);

	laikadfu_cache_invalidate((const void *)scratch);
	write32(gLaikaDFUOffsets.dwc2_base, DOEPDMA0, (uint32_t)scratch);
	write32(gLaikaDFUOffsets.dwc2_base, DOEPTSIZ0, OUT_DMA_SIZE);
	write32(gLaikaDFUOffsets.dwc2_base, DOEPCTL0,
		read32(gLaikaDFUOffsets.dwc2_base, DOEPCTL0) | EP_ENABLE_CLEAR_NAK);
	do
		status = read32(gLaikaDFUOffsets.dwc2_base, DOEPINT0);
	while ((status & 1u) == 0);
	write32(gLaikaDFUOffsets.dwc2_base, DOEPINT0, status);
}

void synopsys_ep0_set_address(uint16_t address)
{
	uint32_t config = read32(gLaikaDFUOffsets.dwc2_base, DCFG) & 0xfffff80fu;
	write32(gLaikaDFUOffsets.dwc2_base, DCFG, config | ((address & 0x7fu) << 4));
}

void synopsys_ep0_stall(void)
{
	write32(gLaikaDFUOffsets.dwc2_base, DIEPCTL0,
		read32(gLaikaDFUOffsets.dwc2_base, DIEPCTL0) | EP_STALL);
}

void synopsys_ep2_initialize(void)
{
	write32(gLaikaDFUOffsets.dwc2_base, DOEPINT2,
		read32(gLaikaDFUOffsets.dwc2_base, DOEPINT2));
	write32(gLaikaDFUOffsets.dwc2_base, DOEPCTL2, 0x18088200u);
	write32(gLaikaDFUOffsets.dwc2_base, DAINTMSK,
		read32(gLaikaDFUOffsets.dwc2_base, DAINTMSK) | 0x40000u);
}

void synopsys_ep2_prime(uintptr_t scratch)
{
	uintptr_t buffer = scratch + 128;
	laikadfu_cache_invalidate((const void *)buffer);
	write32(gLaikaDFUOffsets.dwc2_base, DOEPDMA2, (uint32_t)buffer);
	write32(gLaikaDFUOffsets.dwc2_base, DOEPTSIZ2, OUT_DMA_SIZE);
	write32(gLaikaDFUOffsets.dwc2_base, DOEPCTL2,
		read32(gLaikaDFUOffsets.dwc2_base, DOEPCTL2) | EP_ENABLE_CLEAR_NAK);
}

uint32_t synopsys_ep2_wait(uintptr_t scratch)
{
	uint32_t status;
	do
		status = read32(gLaikaDFUOffsets.dwc2_base, DOEPINT2);
	while ((status & 5u) == 0);

	uint32_t transfer_size = read32(gLaikaDFUOffsets.dwc2_base, DOEPTSIZ2);
	write32(gLaikaDFUOffsets.dwc2_base, DOEPINT2, status);
	if ((status & 4u) != 0)
		laikadfu_fail(status);

	uint32_t count = EP2_TRANSFER_SIZE - (transfer_size & 0x7ffffu);
	laikadfu_cache_invalidate((const void *)(scratch + 128));
	return count;
}
