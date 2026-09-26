// Copyright (c) 0cyn All Rights Reserved

#include "rom-patcher/iBootPatcherSetup.h"

#include <cstring>
#include <pf_aarch64.h>

#include "payloads/iboot/laikadfu/laikadfu_offsets.h"
#include "payloads/iboot/laikadfu/diag.h"
#include "../payloads/shellcode/t8030/offsets.h"
#include "control/Fault.h"
#include "../../include/control/Log.h"

#include <stdint.h>

#include "generated-payloads/combined_stage2_payload.h"
#include "usb/Device.h"
#include "usb/PwnedDFUDevice.h"
#include "usb/libusb.h"

namespace {
	uint32_t Fnv1aUpdate(uint32_t hash, const void* data, size_t length)
	{
		const uint8_t* bytes = static_cast<const uint8_t*>(data);
		for (size_t i = 0; i < length; ++i)
			hash = (hash ^ bytes[i]) * 16777619u;
		return hash;
	}

	uint32_t Fnv1a(const void* data, size_t length)
	{
		return Fnv1aUpdate(2166136261u, data, length);
	}

	uint32_t ExpectedStage2Fnv(const uint8_t* payload, size_t payload_length, const laikadfu_offsets& offsets)
	{
		uint32_t hash = 2166136261u;
		const size_t offsets_offset = payload_length - sizeof(laikadfu_offsets);
		hash = Fnv1aUpdate(hash, payload, offsets_offset);
		hash = Fnv1aUpdate(hash, &offsets, sizeof(offsets));
		return hash;
	}

	int ReadRangeFnv(usb::PwnedDFUDevice& device, uint64_t address, size_t length, uint32_t* out)
	{
		if (out == nullptr) return LIBUSB_ERROR_INVALID_PARAM;
		uint8_t buffer[256];
		uint32_t hash = 2166136261u;
		size_t offset = 0;
		while (offset < length)
		{
			const size_t chunk = (length - offset) < sizeof(buffer) ? (length - offset) : sizeof(buffer);
			const int rc = device.Read(address + offset, buffer, chunk);
			if (rc != LIBUSB_SUCCESS) return rc;
			hash = Fnv1aUpdate(hash, buffer, chunk);
			offset += chunk;
		}
		*out = hash;
		return LIBUSB_SUCCESS;
	}

	void LogOffsets(const char* soc, uint64_t payload_target, const laikadfu_offsets& offsets)
	{
		L41KA_LOG(logging::Level::Info,
			"laikadfu offsets soc=%s payload=0x%llx magic=0x%llx sram=0x%llx dwc2=0x%llx dart=0x%llx",
			soc, static_cast<unsigned long long>(payload_target), static_cast<unsigned long long>(offsets.magic),
			static_cast<unsigned long long>(offsets.sram_base), static_cast<unsigned long long>(offsets.dwc2_base),
			static_cast<unsigned long long>(offsets.dart_base));
		L41KA_LOG(logging::Level::Info,
			"laikadfu usb clocks c0=0x%llx c1=0x%llx c2=0x%llx complex=0x%llx phy=0x%llx phy_cfg0=0x%llx",
			static_cast<unsigned long long>(offsets.usb_clock0),
			static_cast<unsigned long long>(offsets.usb_clock1),
			static_cast<unsigned long long>(offsets.usb_clock2),
			static_cast<unsigned long long>(offsets.usb_complex),
			static_cast<unsigned long long>(offsets.usb_phy),
			static_cast<unsigned long long>(offsets.usb_phy_cfg0));
		L41KA_LOG(logging::Level::Info, "laikadfu dart stream_mask=0x%llx wait=%llu complex_control=%llu",
			static_cast<unsigned long long>(offsets.dart_tlb_stream_mask),
			static_cast<unsigned long long>(offsets.dart_wait_for_tlb),
			static_cast<unsigned long long>(offsets.usb_complex_control));
	}

	size_t FindDiagOffset(const uint8_t* payload, size_t payload_length)
	{
		if (payload == nullptr || payload_length < sizeof(laikadfu_diag_config)) return SIZE_MAX;
		for (size_t offset = 0; offset <= payload_length - sizeof(laikadfu_diag_config); offset += 8)
		{
			uint64_t magic = 0;
			std::memcpy(&magic, payload + offset, sizeof(magic));
			if (magic == LAIKADFU_DIAG_MAGIC) return offset;
		}
		return SIZE_MAX;
	}

	int PrepareStage2Payload(usb::PwnedDFUDevice& device, uint64_t target, const uint8_t* payload, size_t payload_length,
		const laikadfu_offsets& offsets, uint32_t diag_mode)
	{
		L41KA_LOG(logging::Level::Info, "stage2 prepare target=0x%llx payload_len=%lu offsets_len=%lu",
			static_cast<unsigned long long>(target), static_cast<unsigned long>(payload_length),
			static_cast<unsigned long>(sizeof(laikadfu_offsets)));
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
		L41KA_LOG(logging::Level::Info, "stage2 offsets marker found=0x%llx expected=0x%llx offset=0x%lx",
			static_cast<unsigned long long>(offsets_magic), static_cast<unsigned long long>(LAIKADFU_OFFSETS_MAGIC),
			static_cast<unsigned long>(offsets_offset));
		if (offsets_magic != LAIKADFU_OFFSETS_MAGIC)
			return LIBUSB_ERROR_OTHER;

		if (payload != nullptr)
		{
			L41KA_LOG(logging::Level::Info, "stage2 payload write target=0x%llx len=%lu fnv=%08lx",
				static_cast<unsigned long long>(target), static_cast<unsigned long>(payload_length),
				static_cast<unsigned long>(Fnv1a(payload, payload_length)));
			const int rc = device.Write(target, payload, payload_length);
			if (rc != LIBUSB_SUCCESS) return rc;
		}
		const int offsets_rc = device.Write(target + offsets_offset, &offsets, sizeof(offsets));
		if (offsets_rc != LIBUSB_SUCCESS) return offsets_rc;
		if (diag_mode != LAIKADFU_DIAG_NONE)
		{
			const size_t diag_offset = payload != nullptr ? FindDiagOffset(payload, payload_length) : SIZE_MAX;
			if (diag_offset == SIZE_MAX)
			{
				L41KA_LOG(logging::Level::Warn, "stage2 diag marker not found mode=%lu", static_cast<unsigned long>(diag_mode));
				return LIBUSB_ERROR_NOT_FOUND;
			}
			laikadfu_diag_config diag {
				.magic = LAIKADFU_DIAG_MAGIC,
				.mode = diag_mode,
				.checkpoint = 0,
				.version = LAIKADFU_DIAG_CONFIG_VERSION,
			};
			L41KA_LOG(logging::Level::Info, "stage2 diag mode=%lu offset=0x%lx target=0x%llx size=%lu",
				static_cast<unsigned long>(diag_mode), static_cast<unsigned long>(diag_offset),
				static_cast<unsigned long long>(target + diag_offset),
				static_cast<unsigned long>(sizeof(diag)));
			const int diag_rc = device.Write(target + diag_offset, &diag, sizeof(diag));
			if (diag_rc != LIBUSB_SUCCESS) return diag_rc;
		}

		uint32_t readback = 0;
		const int readback_rc = ReadRangeFnv(device, target, payload_length, &readback);
		if (payload != nullptr)
		{
			const uint32_t expected = ExpectedStage2Fnv(payload, payload_length, offsets);
			L41KA_LOG(readback_rc == LIBUSB_SUCCESS && readback == expected ? logging::Level::Info : logging::Level::Warn,
				"stage2 readback rc=%d expected_fnv=%08lx read_fnv=%08lx match=%lu", readback_rc,
				static_cast<unsigned long>(expected), static_cast<unsigned long>(readback),
				static_cast<unsigned long>(readback_rc == LIBUSB_SUCCESS && readback == expected ? 1u : 0u));
		}
		else
		{
			L41KA_LOG(logging::Level::Info, "stage2 uploaded readback rc=%d fnv=%08lx", readback_rc,
				static_cast<unsigned long>(readback));
		}
		return readback_rc;
	}

	int T8020(usb::PwnedDFUDevice& device, const uint8_t* payload, size_t payload_length, uint32_t diag_mode)
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
		LogOffsets("t8020", t8020_payload_pa, offsets);
		int rc = PrepareStage2Payload(device, t8020_payload_pa, payload, payload_length, offsets, diag_mode);
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

		L41KA_LOG(logging::Level::Info, "setup-iboot handoff t8020 SetBootLR=0x100000000");
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

	int T8030(usb::PwnedDFUDevice& device, const uint8_t* payload, size_t payload_length, uint32_t diag_mode)
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
		LogOffsets("t8030", t8030_payload_pa, offsets);
		int rc = PrepareStage2Payload(device, t8030_payload_pa, payload, payload_length, offsets, diag_mode);
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

		L41KA_LOG(logging::Level::Info, "setup-iboot handoff t8030 SetBootLR=0x10002faa0");
		return device.SetBootLR(0x10002faa0ull);
	}

	int RunPayload(usb::PwnedDFUDevice& device, const uint8_t* payload, size_t payload_length, uint32_t diag_mode)
	{
		L41KA_LOG(logging::Level::Info, "setup-iboot dispatch cpid=0x%lx payload_len=%lu diag_mode=%lu",
			static_cast<unsigned long>(device.CPID()), static_cast<unsigned long>(payload_length),
			static_cast<unsigned long>(diag_mode));
		switch (device.CPID())
		{
		case 0x8020:
			return T8020(device, payload, payload_length, diag_mode);
		case 0x8030:
			return T8030(device, payload, payload_length, diag_mode);
		default:
			return LIBUSB_ERROR_NOT_SUPPORTED;
		}
	}
}  // namespace

int iBootPatcherSetup::Run(usb::PwnedDFUDevice& device, uint32_t diag_mode)
{
	static_assert(sizeof(combined_stage2_payload) == combined_stage2_payload_len);
	return RunPayload(device, combined_stage2_payload, combined_stage2_payload_len, diag_mode);
}

int iBootPatcherSetup::RunUploaded(usb::PwnedDFUDevice& device, size_t payload_length)
{
	// since we have no heap, we pre-upload if we're using a user-supplied stage2
	return RunPayload(device, nullptr, payload_length, LAIKADFU_DIAG_NONE);
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
