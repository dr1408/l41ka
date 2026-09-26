#ifndef L41KA_CONTROL_PROTOCOL_H
#define L41KA_CONTROL_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

namespace control {
	constexpr uint64_t PacketMagic = UINT64_C(0x4e5943414b31344c);
	constexpr uint8_t ProtocolVersion = 1;
	constexpr size_t PacketHeaderSize = 32;
	constexpr uint32_t MaxPayloadLength = 4096;

	enum class PacketType : uint8_t {
		Request = 1,
		Response = 2,
	};

	enum class Status : uint32_t {
		Success = 0x00000000,

		InvalidMagic = 0x01000001,
		UnsupportedVersion = 0x01000002,
		InvalidPacketType = 0x01000003,
		ReservedFieldNonzero = 0x01000004,
		InvalidRequestId = 0x01000005,
		InvalidRequestStatus = 0x01000006,
		PayloadTooLarge = 0x01000007,
		PayloadCrcMismatch = 0x01000008,
		UnknownOpcode = 0x01000009,
		InvalidPayload = 0x0100000a,
		PipelinedRequest = 0x0100000b,

		NoConnection = 0x02000001,
		WrongDeviceType = 0x02000002,
		CommandBusy = 0x02000003,

		UploadNotActive = 0x03000001,
		UploadAlreadyActive = 0x03000002,
		UploadWrongOffset = 0x03000003,
		UploadExceedsLength = 0x03000004,
		UploadIncomplete = 0x03000005,
		UploadCrcMismatch = 0x03000006,
	};

	enum class Opcode : uint32_t {
		PiCheckErrors = 0xff000001,
		PiGetInfo = 0xff000002,
		PiGetOpenConnection = 0xff000003,
		PiPollForDevice = 0xff000004,
		PiResetPi = 0xff000005,
		PiEnterBOOTSEL = 0xff0000ff,

		DeviceDfuInfo = 0x01000001,
		DeviceDfuExploit = 0x01000002,
		DeviceDfuRawEp0 = 0x01000003,

		DevicePwnedDfuPhysRead = 0x02000001,
		DevicePwnedDfuPhysWrite = 0x02000002,
		DevicePwnedDfuExecute = 0x02000003,

		DevicePwnedDfuSendIbootPatchfinder = 0x03000001,
		DevicePwnedDfuTriggerIbootPatchfinder = 0x03000002,
		DevicePwnedDfuSendEmbeddedIbootPatchfinderAndBoot = 0x03000003,
		DevicePwnedDfuSendEmbeddedIbootPatchfinderDiag = 0x03000004,

		DeviceRecoveryReboot = 0x04000001,
		DeviceRecoveryInfo = 0x04000002,

		DeviceLaikaDfuSendPayload = 0x05000001,
		DeviceLaikaDfuTriggerPayload = 0x05000002,
		DeviceLaikaDfuSendEmbeddedPongoOs = 0x05000003,

		DevicePongoReadOutput = 0x06000001,
		DevicePongoSendCommand = 0x06000002,
		DevicePongoSendModule = 0x06000003,
		DevicePongoTriggerModule = 0x06000004,
		DevicePongoBeginUploadFile = 0x06000005,
		DevicePongoFinishUploadFile = 0x06000006,
		DevicePongoSendAndTriggerEmbeddedKPF = 0x060000f1,
		DevicePongoSendAndTriggerEmbeddedRamdisk = 0x060000f2,

		GenericUploadChunk = 0xf1000001,
	};

	enum class DeviceType : uint32_t {
		Dfu = 0x00000001,
		PwnedDfu = 0x00000002,
		// 0x03 in opcodes is reserved for iboot pf sending. 
		Recovery = 0x00000004,
		LaikaDfu = 0x00000005,
		Pongo = 0x00000006,
		None = 0xffffffff,
	};

	struct PacketHeader {
		uint64_t magic = PacketMagic;
		uint8_t version = ProtocolVersion;
		PacketType type = PacketType::Request;
		uint16_t reserved = 0;
		uint32_t opcode = 0;
		uint32_t request_id = 0;
		uint32_t payload_length = 0;
		uint32_t status = 0;
		uint32_t payload_crc32 = 0;
	};

	enum class HeaderError : uint8_t {
		None,
		TooShort,
		InvalidMagic,
		UnsupportedVersion,
		InvalidType,
		ReservedNonzero,
		InvalidRequestId,
		InvalidRequestStatus,
		PayloadTooLarge,
	};

	void EncodeHeader(const PacketHeader& header, uint8_t output[PacketHeaderSize]);
	HeaderError DecodeRequestHeader(const uint8_t* input, size_t length, PacketHeader* header);
	Status HeaderErrorStatus(HeaderError error);
	uint32_t Crc32(const void* data, size_t length, uint32_t previous = 0);
}

#endif
