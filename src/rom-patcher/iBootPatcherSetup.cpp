// Copyright (c) 0cyn All Rights Reserved

#include "rom-patcher/iBootPatcherSetup.h"

#include <cstring>
#include <pf_aarch64.h>

#include "payloads/iboot/laikadfu/laikadfu_offsets.h"
#include "../payloads/shellcode/t8030/offsets.h"
#include "control/Fault.h"
#include "../../include/control/Log.h"

#include <stdint.h>

#include "generated-payloads/combined_stage2_payload.h"
#include "usb/Device.h"
#include "usb/PwnedDFUDevice.h"
#include "usb/libusb.h"

namespace {
	int PrepareStage2Payload(usb::PwnedDFUDevice& device, uint64_t target, const uint8_t* payload, size_t payload_length,
		const laikadfu_offsets& offsets)
	{
		if (payload_length == 0u || payload_length >= PAGE_SIZE || payload_length < sizeof(laikadfu_offsets))
			return LIBUSB_ERROR_NOT_SUPPORTED;

		const size_t offsets_offset = payload_length - sizeof(laikadfu_offsets);
		uint64_t offsets_magic = 0;

		if (payload != nullptr)
			std::memcpy(&offsets_magic, payload + offsets_offset, sizeof(offsets_magic));
		else
		{
			// user-supplied stage2 is already in place (hopefully)
			const int rc = device.Read(target + offsets_offset, &offsets_magic, sizeof(offsets_magic));
			if (rc != LIBUSB_SUCCESS) return rc;
		}

		// important! make sure we don't have linker problems.
		if (offsets_magic != LAIKADFU_OFFSETS_MAGIC)
			return LIBUSB_ERROR_OTHER;

		if (payload != nullptr)
		{
			const int rc = device.Write(target, payload, payload_length);
			if (rc != LIBUSB_SUCCESS) return rc;
		}
		return device.Write(target + offsets_offset, &offsets, sizeof(offsets));
	}

	int T8020(usb::PwnedDFUDevice& device, const uint8_t* payload, size_t payload_length)
	{
		constexpr uint64_t t8020_payload_pa = 0x19c388000ull;

		const laikadfu_offsets offsets = {
			.magic = LAIKADFU_OFFSETS_MAGIC,
			.usb_clock0 = 0x23b0802a8ull,
			.usb_clock1 = 0x23b0802b0ull,
			.usb_clock2 = 0x23b0802a8ull,
			.usb_complex = 0x239000000ull,
			.usb_phy = 0x239000064ull,
			.dwc2_base = 0x239100000ull,
			.dart_base = 0x239900000ull,
			.sram_base = 0x19c000000ull,
			.usb_complex_control = 1,
			.usb_phy_cfg0 = 0x27373af3u,
			.dart_tlb_stream_mask = 1,
			.dart_wait_for_tlb = 0,
		};
		int rc = PrepareStage2Payload(device, t8020_payload_pa, payload, payload_length, offsets);
		if (rc != LIBUSB_SUCCESS) return rc;

		// set up the super basic loop state and jump to 0x1000019f8
		const uint32_t nand_trampoline[] = {
			aarch64::sub_x(31, 31, 0x90),
			aarch64::movz_w(19, 0, 0),
			aarch64::movz_w(20, 8, 0),
			aarch64::movk_w(20, 0x4000, 16),
			aarch64::movz_w(21, 11, 0),
			aarch64::movz_w(22, 3, 0),
			aarch64::movz_w(23, 0, 0),
			aarch64::movz_w(25, 0, 0),
			aarch64::movz_w(27, 7, 0),
			aarch64::movk_w(27, 2, 16),
			aarch64::stp_w(23, 25, 31, 0x50),
			aarch64::str_w(22, 31, 0x58),
			aarch64::ldr_literal_x(16, 0x30, 0x38),
			aarch64::br(16),
			0x000019f8u,
			0x00000001u,
		};
		static_assert(sizeof(nand_trampoline) == 0x40u);

		// copy of rom that is in sram, overlaid with mmu on top of rom at runtime for patches.
		// however our read/write primitives operate with MMU off, soooo
		auto writableCopyAdjusted = [](uint64_t a) {
			return (uint64_t)((a - 0x100000000ull) + 0x19c378000ull);
		};

		rc = device.Write(writableCopyAdjusted(0x100000000), nand_trampoline, sizeof(nand_trampoline));
		if (rc != LIBUSB_SUCCESS)
		{
			return -1;
		}

		// i cant remember what this does or if its needed
		const uint32_t idk = aarch64::movz_x(2, 0, 0);
		rc = device.Write(writableCopyAdjusted(0x100001ba0ull), &idk, sizeof(idk));
		if (rc != LIBUSB_SUCCESS)
		{
			return -2;
		}

		// aes junk
		const uint32_t ret = aarch64::ARM64_RET;
		rc = device.Write(writableCopyAdjusted(0x100006cd8ull), &ret, sizeof(ret));
		if (rc != LIBUSB_SUCCESS)
		{
			return -3;
		}

		// fuses
		rc = device.Write(writableCopyAdjusted(0x100007e24ull), &ret, sizeof(ret));
		if (rc != LIBUSB_SUCCESS)
		{
			return -4;
		}

		const uint32_t b_28 = aarch64::assemble_b(0, 0x28);

		const uint32_t mov_x18_8 = aarch64::movz_x(18, 0x8000, 0);
		const uint32_t movk_x18_9c38_lsl10 = aarch64::movk_x(18, 0x9c38, 16);
		const uint32_t movk_x18_1_lsl20 = aarch64::movk_x(18, 1, 32);

		const uint32_t br_x18 = aarch64::br(18);

		const uint32_t nop = aarch64::ARM64_NOP;

		// heap's haunted
		// things that call heap_panic:
		rc = device.Write(writableCopyAdjusted(0x10000f4f4), &nop, 4);
		rc |= device.Write(writableCopyAdjusted(0x10000f50c), &nop, 4);
		rc |= device.Write(writableCopyAdjusted(0x10000f528), &nop, 4);
		rc |= device.Write(writableCopyAdjusted(0x10000f538), &nop, 4);
		rc |= device.Write(writableCopyAdjusted(0x10000f53c), &nop, 4);
		rc |= device.Write(writableCopyAdjusted(0x10000f548), &nop, 4);
		rc |= device.Write(writableCopyAdjusted(0x10000f550), &nop, 4);
		rc |= device.Write(writableCopyAdjusted(0x10000f57c), &nop, 4);
		rc |= device.Write(writableCopyAdjusted(0x10000f5b0), &nop, 4);
		rc |= device.Write(writableCopyAdjusted(0x10000f5f0), &nop, 4);
		// lol
		if (rc != LIBUSB_SUCCESS)
		{
			return -10;
		}

		constexpr uint64_t boot_tramp = 0x19c018000ull;
		// rom kil
		rc = device.Write(boot_tramp + 0x60ull, &b_28, sizeof(b_28));

		// branch to payload
		rc |= device.Write(boot_tramp + 0x2f4ull, &mov_x18_8, sizeof(mov_x18_8));
		rc |= device.Write(boot_tramp + 0x2f8ull, &movk_x18_9c38_lsl10, sizeof(movk_x18_9c38_lsl10));
		rc |= device.Write(boot_tramp + 0x2fcull, &movk_x18_1_lsl20, sizeof(movk_x18_1_lsl20));
		rc |= device.Write(boot_tramp + 0x3e8ull, &br_x18, sizeof(br_x18));
		if (rc != LIBUSB_SUCCESS)
		{
			return -5;
		}

		const uint32_t spin = aarch64::assemble_b(0, 0);

		// panic -> b .
		rc = device.Write(writableCopyAdjusted(0x100008978), &spin, 4);
		if (rc != LIBUSB_SUCCESS)
		{
			return -7;
		}

		const uint32_t invert = aarch64::tbz(8, 1, 0, 0xc);
		const uint32_t movx0xzr = aarch64::mov_register_x(0, 31);

		// device.Write(writableCopyAdjusted(0x10000a814), &spin, 4);
		// 10000589c  68000836
		// 1000058a0  e0031faa
		// / Pending more RE; this is fuse/demotion related.
		// one of these is checking something just before the image, the other is checking if a func is 0
		// -- both of these are not true when it runs so we invert the tbz and replace the bl here with mov x0, xzr ^..^
		device.Write(writableCopyAdjusted(0x10000589c), &invert, 4);
		device.Write(writableCopyAdjusted(0x1000058a0), &movx0xzr, 4);
		// device.Write(writableCopyAdjusted(0x10000589c), &spin, 4);

		// demote
		const uint32_t b28 = aarch64::assemble_b(0, 0x28);
		rc = device.Write(writableCopyAdjusted(0x100007c20), &b28, 4); // yes we are secure mr iboot
		rc |= device.Exec(0x100007cf8, nullptr, 0, nullptr); // uncomment to demote before leaving DFU
		if (rc != LIBUSB_SUCCESS)
		{
			return rc;
		}

		return device.SetBootLR(0x100000000);
	}

	int T8027(usb::PwnedDFUDevice& device, const uint8_t* payload, size_t payload_length)
	{
		constexpr uint64_t t8027_payload_pa = 0x19c384000ull;
		const laikadfu_offsets offsets = {
			.magic = LAIKADFU_OFFSETS_MAGIC,
			.usb_clock0 = 0x23b080300ull,
			.usb_clock1 = 0x23b080300ull, // sic
			.usb_clock2 = 0x23b080300ull, // sic
			.usb_complex = 0x25d000000ull,
			.usb_phy = 0x25d000064ull,
			.dwc2_base = 0x25d100000ull,
			.dart_base = 0x25d028000ull,
			.sram_base = 0x19c000000ull,
			.usb_complex_control = 0,
			.usb_phy_cfg0 = 0x27373af3u,
			.dart_tlb_stream_mask = 0xf,
			.dart_wait_for_tlb = 1,
		};

		return 0;
	}

	int T8030(usb::PwnedDFUDevice& device, const uint8_t* payload, size_t payload_length)
	{
		constexpr uint64_t t8030_payload_pa = 0x19c384000ull;
		const laikadfu_offsets offsets = {
			.magic = LAIKADFU_OFFSETS_MAGIC,
			.usb_clock0 = 0x23b0802e8ull,
			.usb_clock1 = 0x23b0802f0ull,
			.usb_clock2 = 0x23b0802f8ull,
			.usb_complex = 0x239000000ull,
			.usb_phy = 0x239000064ull,
			.dwc2_base = 0x239100000ull,
			.dart_base = 0x239028000ull,
			.sram_base = 0x19c000000ull,
			.usb_complex_control = 0,
			.usb_phy_cfg0 = 0x27373bf3u,
			.dart_tlb_stream_mask = 0xf,
			.dart_wait_for_tlb = 1,
		};
		int rc = PrepareStage2Payload(device, t8030_payload_pa, payload, payload_length, offsets);
		if (rc != LIBUSB_SUCCESS) return rc;

		const uint32_t nand_trampoline[] = {
			aarch64::ldr_literal_x(20, 0x00, 0x20),
			aarch64::ldr_literal_x(21, 0x04, 0x28),
			aarch64::ldr_literal_x(23, 0x08, 0x30),
			aarch64::str_w(21, 31, 0x50),
			aarch64::str_w(23, 31, 0x54),
			aarch64::str_w(20, 31, 0x58),
			aarch64::ldr_literal_x(16, 0x18, 0x38),
			aarch64::br(16),
			0x00000003u, 0x00000000u, // x20 = 3
			0x00000000u, 0x00000000u, // x21 = 0
			0x00000000u, 0x00000000u, // x23 = 0
			0x00001ef4u, 0x00000001u, // x16 -> we jump here
		};

		auto romRemapAdjusted = [](uint64_t a) {
			return (uint64_t)((a - ROM_BASE) + ROM_NEW_PA);
		};

		rc = device.Write(romRemapAdjusted(0x10002faa0ull), nand_trampoline, sizeof(nand_trampoline));
		if (rc != LIBUSB_SUCCESS)
		{
			return rc;
		}

		// i cant remember what this does or if its needed
		const uint32_t idk = aarch64::movz_x(2, 0, 0);
		rc = device.Write(romRemapAdjusted(0x1000020c4ull), &idk, sizeof(idk));
		if (rc != LIBUSB_SUCCESS)
		{
			return rc;
		}

		// aes junk
		const uint32_t ret = aarch64::ARM64_RET;
		rc = device.Write(romRemapAdjusted(0x1000073a8), &ret, 4);
		if (rc != LIBUSB_SUCCESS)
		{
			return rc;
		}

		// fuses
		rc = device.Write(romRemapAdjusted(0x100008460), &ret, 4);
		if (rc != LIBUSB_SUCCESS)
		{
			return rc;
		}

		const uint32_t b_28 = aarch64::assemble_b(0, 0x28);

		const uint32_t mov_x18_4 = aarch64::movz_x(18, 0x4000, 0);
		const uint32_t movk_x18_9c38_lsl10 = aarch64::movk_x(18, 0x9c38, 16);
		const uint32_t movk_x18_1_lsl20 = aarch64::movk_x(18, 1, 32);

		const uint32_t br_x18 = aarch64::br(18);

		const uint32_t nop = aarch64::ARM64_NOP;

		// collect your offsets from 0x100007d00
		// but this has already been copied into place by
		//	the time we hit DFU code.
		uint64_t boot_trampoline_inplace = 0x19C018000;

		// rom kil
		rc = device.Write(boot_trampoline_inplace + 0x60, &b_28, 4);

		// branch to payload
		rc |= device.Write(boot_trampoline_inplace + 0x23cull, &mov_x18_4, sizeof(mov_x18_4));
		rc |= device.Write(boot_trampoline_inplace + 0x240ull, &movk_x18_9c38_lsl10, sizeof(movk_x18_9c38_lsl10));
		rc |= device.Write(boot_trampoline_inplace + 0x244ull, &movk_x18_1_lsl20, sizeof(movk_x18_1_lsl20));
		rc |= device.Write(boot_trampoline_inplace + 0x330ull, &br_x18, sizeof(br_x18));
		if (rc != LIBUSB_SUCCESS)
		{
			return rc;
		}

		return device.SetBootLR(0x10002faa0ull);
	}

	int RunPayload(usb::PwnedDFUDevice& device, const uint8_t* payload, size_t payload_length)
	{
		switch (device.CPID())
		{
		case 0x8020:
			return T8020(device, payload, payload_length);
		case 0x8030:
			return T8030(device, payload, payload_length);
		default:
			return LIBUSB_ERROR_NOT_SUPPORTED;
		}
	}
}  // namespace

int iBootPatcherSetup::Run(usb::PwnedDFUDevice& device)
{
	static_assert(sizeof(combined_stage2_payload) == combined_stage2_payload_len);
	return RunPayload(device, combined_stage2_payload, combined_stage2_payload_len);
}

int iBootPatcherSetup::RunUploaded(usb::PwnedDFUDevice& device, size_t payload_length)
{
	// since we have no heap, we pre-upload if we're using a user-supplied stage2
	return RunPayload(device, nullptr, payload_length);
}

uint64_t iBootPatcherSetup::UploadedPayloadAddress(const usb::PwnedDFUDevice& device)
{
	switch (device.CPID())
	{
	case 0x8020:
		return 0x19c388000ull;
	case 0x8030:
		return 0x19c384000ull;
	default:
		L41KA_LOG(logging::Level::Fatal, "missing iboot pf tgt addr iBootPatcherSetup::UploadedPayloadAddress");
		return 0;
	}
}
