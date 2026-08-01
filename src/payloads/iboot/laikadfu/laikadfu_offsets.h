// Copyright (c) 0cyn All Rights Reserved
#ifndef L41KA_LAIKADFU_OFFSETS_H
#define L41KA_LAIKADFU_OFFSETS_H

#define LAIKADFU_OFFSETS_MAGIC 0x554644414b49414cull
#define LAIKADFU_SRAM_BASE_OFFSET 64
#define OS_VERS_ENCODE(major, minor, patch) (((major) << 16) | ((minor) << 8) | (patch))
#define LAIKADFU_VERSION OS_VERS_ENCODE(1, 0, 0)
#define LAIKA_VERSION OS_VERS_ENCODE(L41KA_VERSION_MAJOR, L41KA_VERSION_MINOR, L41KA_VERSION_PATCH)

#ifndef __ASSEMBLER__

#include <stddef.h>
#include <stdint.h>

struct os_vers
{
	uint32_t patch : 8;
	uint32_t minor : 8;
	uint32_t major : 16;
};

struct laikadfu_offsets
{
	uint64_t magic;			// this is primarly for iBootPatcherSetup to doublecheck that we didn't fuck up linkage
	uint64_t usb_clock0;
	uint64_t usb_clock1;
	uint64_t usb_clock2;
	uint64_t usb_complex;
	uint64_t usb_phy;
	uint64_t dwc2_base;
	uint64_t dart_base;
	uint64_t sram_base;
	uint64_t usb_complex_control;
	uint64_t usb_phy_cfg0;
	uint64_t dart_tlb_stream_mask;
	uint64_t dart_wait_for_tlb;
};

#ifdef __cplusplus
static_assert(sizeof(os_vers) == sizeof(uint32_t));
static_assert(offsetof(laikadfu_offsets, sram_base) == LAIKADFU_SRAM_BASE_OFFSET);
static_assert(sizeof(laikadfu_offsets) == 104);
#else
_Static_assert(sizeof(struct os_vers) == sizeof(uint32_t), "version size mismatch");
_Static_assert(offsetof(struct laikadfu_offsets, sram_base) == LAIKADFU_SRAM_BASE_OFFSET, "offset mismatch");
_Static_assert(sizeof(struct laikadfu_offsets) == 104, "size mismatch");
#endif

#endif

#endif
