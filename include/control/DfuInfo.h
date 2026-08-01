#ifndef L41KA_CONTROL_DFU_INFO_H
#define L41KA_CONTROL_DFU_INFO_H

#include <stddef.h>
#include <stdint.h>

namespace control {
	constexpr size_t DfuInfoWireSize = 576;

	enum DfuInfoField : uint32_t {
		DfuHasCpid = 1u << 0,
		DfuHasCprv = 1u << 1,
		DfuHasCpfm = 1u << 2,
		DfuHasScep = 1u << 3,
		DfuHasBdid = 1u << 4,
		DfuHasEcid = 1u << 5,
		DfuHasIbfl = 1u << 6,
		DfuHasSrtg = 1u << 7,
		DfuHasSrnm = 1u << 8,
		DfuHasImei = 1u << 9,
		DfuHasApNonce = 1u << 10,
		DfuHasSepNonce = 1u << 11,
		DfuHasPwnd = 1u << 12,
		DfuHasProductType = 1u << 13,
		DfuHasHardwareModel = 1u << 14,
		DfuHasDisplayName = 1u << 15,
	};

	struct DfuInfo {
		uint32_t present_fields;
		uint32_t cpid;
		uint32_t cprv;
		uint32_t cpfm;
		uint32_t scep;
		uint32_t bdid;
		uint32_t ibfl;
		uint64_t ecid;
		uint32_t pid;
		uint32_t interface_number;
		uint32_t pwned;
		uint32_t ap_nonce_length;
		uint32_t sep_nonce_length;
		char srtg[64];
		char srnm[32];
		char imei[32];
		uint8_t ap_nonce[64];
		uint8_t sep_nonce[64];
		char serial_string[128];
		char product_type[32];
		char hardware_model[32];
		char display_name[64];
	};

	void ParseDfuInfo(const char* serial, const char* nonce_string, DfuInfo* info);
	void EncodeDfuInfo(const DfuInfo& info, uint8_t output[DfuInfoWireSize]);
}

#endif
