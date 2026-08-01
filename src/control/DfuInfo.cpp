// Copyright (c) 0cyn All Rights Reserved
#include "control/DfuInfo.h"

#include <cstring>

#include "control/AppleDeviceDatabase.h"

namespace control {
	namespace {
		int HexDigit(char value)
		{
			if (value >= '0' && value <= '9') return value - '0';
			if (value >= 'a' && value <= 'f') return value - 'a' + 10;
			if (value >= 'A' && value <= 'F') return value - 'A' + 10;
			return -1;
		}

		const char* FindTag(const char* input, const char* tag)
		{
			if (input == nullptr) return nullptr;
			const size_t length = std::strlen(tag);
			for (const char* cursor = input; *cursor != 0; ++cursor)
				if (std::strncmp(cursor, tag, length) == 0) return cursor + length;
			return nullptr;
		}

		bool ParseHex(const char* input, uint64_t* value)
		{
			if (input == nullptr || value == nullptr) return false;
			uint64_t result = 0;
			size_t digits = 0;
			for (; digits < 16u; ++digits)
			{
				const int digit = HexDigit(input[digits]);
				if (digit < 0) break;
				result = (result << 4u) | static_cast<uint64_t>(digit);
			}
			if (digits == 0u) return false;
			*value = result;
			return true;
		}

		template <size_t N>
		bool ParseBracketed(const char* input, const char* tag, char (&output)[N])
		{
			const char* value = FindTag(input, tag);
			if (value == nullptr) return false;
			const char* end = std::strchr(value, ']');
			if (end == nullptr || end == value || static_cast<size_t>(end - value) >= N) return false;
			std::memcpy(output, value, static_cast<size_t>(end - value));
			output[end - value] = 0;
			return true;
		}

		bool ParseNonce(const char* input, const char* tag, uint8_t* output, uint32_t* output_length)
		{
			const char* value = FindTag(input, tag);
			if (value == nullptr) return false;
			uint32_t length = 0;
			while (length < 64u)
			{
				const int high = HexDigit(value[length * 2u]);
				const int low = HexDigit(value[length * 2u + 1u]);
				if (high < 0 || low < 0) break;
				output[length++] = static_cast<uint8_t>((high << 4u) | low);
			}
			if (length == 0u) return false;
			*output_length = length;
			return true;
		}

		void WriteU32(uint8_t* output, uint32_t value)
		{
			output[0] = static_cast<uint8_t>(value);
			output[1] = static_cast<uint8_t>(value >> 8u);
			output[2] = static_cast<uint8_t>(value >> 16u);
			output[3] = static_cast<uint8_t>(value >> 24u);
		}

		void CopyField(uint8_t* output, const void* input, size_t length)
		{
			std::memcpy(output, input, length);
		}
	}

	void ParseDfuInfo(const char* serial, const char* nonce_string, DfuInfo* info)
	{
		if (info == nullptr) return;
		*info = {};
		if (serial == nullptr) return;
		std::strncpy(info->serial_string, serial, sizeof(info->serial_string) - 1u);

		struct NumericTag { const char* tag; uint32_t bit; uint32_t* destination; };
		NumericTag tags[] = {
			{"CPID:", DfuHasCpid, &info->cpid}, {"CPRV:", DfuHasCprv, &info->cprv},
			{"CPFM:", DfuHasCpfm, &info->cpfm}, {"SCEP:", DfuHasScep, &info->scep},
			{"BDID:", DfuHasBdid, &info->bdid}, {"IBFL:", DfuHasIbfl, &info->ibfl},
		};
		for (const auto& tag : tags)
		{
			uint64_t value = 0;
			if (ParseHex(FindTag(serial, tag.tag), &value) && value <= UINT32_MAX)
			{
				*tag.destination = static_cast<uint32_t>(value);
				info->present_fields |= tag.bit;
			}
		}
		if (ParseHex(FindTag(serial, "ECID:"), &info->ecid)) info->present_fields |= DfuHasEcid;
		if (ParseBracketed(serial, "SRTG:[", info->srtg)) info->present_fields |= DfuHasSrtg;
		if (ParseBracketed(serial, "SRNM:[", info->srnm)) info->present_fields |= DfuHasSrnm;
		if (ParseBracketed(serial, "IMEI:[", info->imei)) info->present_fields |= DfuHasImei;
		if (FindTag(serial, "PWND:[") != nullptr)
		{
			info->pwned = 1;
			info->present_fields |= DfuHasPwnd;
		}
		if (ParseNonce(nonce_string, "NONC:", info->ap_nonce, &info->ap_nonce_length))
			info->present_fields |= DfuHasApNonce;
		if (ParseNonce(nonce_string, "SNON:", info->sep_nonce, &info->sep_nonce_length))
			info->present_fields |= DfuHasSepNonce;

		if ((info->present_fields & (DfuHasCpid | DfuHasBdid)) == (DfuHasCpid | DfuHasBdid))
		{
			const AppleDevice* device = FindAppleDevice(info->cpid, info->bdid);
			if (device != nullptr)
			{
				std::strncpy(info->product_type, device->product_type, sizeof(info->product_type) - 1u);
				std::strncpy(info->hardware_model, device->hardware_model, sizeof(info->hardware_model) - 1u);
				std::strncpy(info->display_name, device->display_name, sizeof(info->display_name) - 1u);
				info->present_fields |= DfuHasProductType | DfuHasHardwareModel | DfuHasDisplayName;
			}
		}
	}

	void EncodeDfuInfo(const DfuInfo& info, uint8_t output[DfuInfoWireSize])
	{
		std::memset(output, 0, DfuInfoWireSize);
		WriteU32(output, info.present_fields);
		WriteU32(output + 4, info.cpid); WriteU32(output + 8, info.cprv);
		WriteU32(output + 12, info.cpfm); WriteU32(output + 16, info.scep);
		WriteU32(output + 20, info.bdid); WriteU32(output + 24, info.ibfl);
		WriteU32(output + 32, static_cast<uint32_t>(info.ecid));
		WriteU32(output + 36, static_cast<uint32_t>(info.ecid >> 32u));
		WriteU32(output + 40, info.pid); WriteU32(output + 44, info.interface_number);
		WriteU32(output + 48, info.pwned); WriteU32(output + 52, info.ap_nonce_length);
		WriteU32(output + 56, info.sep_nonce_length);
		CopyField(output + 60, info.srtg, sizeof(info.srtg));
		CopyField(output + 124, info.srnm, sizeof(info.srnm));
		CopyField(output + 156, info.imei, sizeof(info.imei));
		CopyField(output + 188, info.ap_nonce, sizeof(info.ap_nonce));
		CopyField(output + 252, info.sep_nonce, sizeof(info.sep_nonce));
		CopyField(output + 316, info.serial_string, sizeof(info.serial_string));
		CopyField(output + 444, info.product_type, sizeof(info.product_type));
		CopyField(output + 476, info.hardware_model, sizeof(info.hardware_model));
		CopyField(output + 508, info.display_name, sizeof(info.display_name));
	}
}
