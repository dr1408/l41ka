// Copyright (c) 0cyn All Rights Reserved
#include "control/Protocol.h"

namespace control {
	namespace {
		uint16_t ReadU16(const uint8_t* input)
		{
			return static_cast<uint16_t>(input[0]) | static_cast<uint16_t>(input[1] << 8u);
		}

		uint32_t ReadU32(const uint8_t* input)
		{
			return static_cast<uint32_t>(input[0]) | (static_cast<uint32_t>(input[1]) << 8u)
				| (static_cast<uint32_t>(input[2]) << 16u) | (static_cast<uint32_t>(input[3]) << 24u);
		}

		uint64_t ReadU64(const uint8_t* input)
		{
			return static_cast<uint64_t>(ReadU32(input)) | (static_cast<uint64_t>(ReadU32(input + 4)) << 32u);
		}

		void WriteU16(uint8_t* output, uint16_t value)
		{
			output[0] = static_cast<uint8_t>(value);
			output[1] = static_cast<uint8_t>(value >> 8u);
		}

		void WriteU32(uint8_t* output, uint32_t value)
		{
			output[0] = static_cast<uint8_t>(value);
			output[1] = static_cast<uint8_t>(value >> 8u);
			output[2] = static_cast<uint8_t>(value >> 16u);
			output[3] = static_cast<uint8_t>(value >> 24u);
		}

		void WriteU64(uint8_t* output, uint64_t value)
		{
			WriteU32(output, static_cast<uint32_t>(value));
			WriteU32(output + 4, static_cast<uint32_t>(value >> 32u));
		}
	}

	void EncodeHeader(const PacketHeader& header, uint8_t output[PacketHeaderSize])
	{
		WriteU64(output, header.magic);
		output[8] = header.version;
		output[9] = static_cast<uint8_t>(header.type);
		WriteU16(output + 10, header.reserved);
		WriteU32(output + 12, header.opcode);
		WriteU32(output + 16, header.request_id);
		WriteU32(output + 20, header.payload_length);
		WriteU32(output + 24, header.status);
		WriteU32(output + 28, header.payload_crc32);
	}

	HeaderError DecodeRequestHeader(const uint8_t* input, size_t length, PacketHeader* header)
	{
		if (input == nullptr || header == nullptr || length < PacketHeaderSize)
			return HeaderError::TooShort;

		header->magic = ReadU64(input);
		header->version = input[8];
		header->type = static_cast<PacketType>(input[9]);
		header->reserved = ReadU16(input + 10);
		header->opcode = ReadU32(input + 12);
		header->request_id = ReadU32(input + 16);
		header->payload_length = ReadU32(input + 20);
		header->status = ReadU32(input + 24);
		header->payload_crc32 = ReadU32(input + 28);

		if (header->magic != PacketMagic)
			return HeaderError::InvalidMagic;
		if (header->version != ProtocolVersion)
			return HeaderError::UnsupportedVersion;
		if (header->type != PacketType::Request)
			return HeaderError::InvalidType;
		if (header->reserved != 0u)
			return HeaderError::ReservedNonzero;
		if (header->request_id == 0u)
			return HeaderError::InvalidRequestId;
		if (header->status != 0u)
			return HeaderError::InvalidRequestStatus;
		if (header->payload_length > MaxPayloadLength)
			return HeaderError::PayloadTooLarge;
		return HeaderError::None;
	}

	Status HeaderErrorStatus(HeaderError error)
	{
		switch (error)
		{
		case HeaderError::None:
			return Status::Success;
		case HeaderError::InvalidMagic:
			return Status::InvalidMagic;
		case HeaderError::UnsupportedVersion:
			return Status::UnsupportedVersion;
		case HeaderError::InvalidType:
			return Status::InvalidPacketType;
		case HeaderError::ReservedNonzero:
			return Status::ReservedFieldNonzero;
		case HeaderError::InvalidRequestId:
			return Status::InvalidRequestId;
		case HeaderError::InvalidRequestStatus:
			return Status::InvalidRequestStatus;
		case HeaderError::PayloadTooLarge:
			return Status::PayloadTooLarge;
		case HeaderError::TooShort:
			return Status::InvalidPayload;
		}
		return Status::InvalidPayload;
	}

	uint32_t Crc32(const void* data, size_t length, uint32_t previous)
	{
		if (data == nullptr && length != 0u)
			return 0u;
		uint32_t crc = ~previous;
		const auto* bytes = static_cast<const uint8_t*>(data);
		for (size_t i = 0; i < length; ++i)
		{
			crc ^= bytes[i];
			for (unsigned bit = 0; bit < 8u; ++bit)
				crc = (crc >> 1u) ^ (0xedb88320u & (0u - (crc & 1u)));
		}
		return ~crc;
	}
}
