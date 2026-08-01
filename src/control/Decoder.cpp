// Copyright (c) 0cyn All Rights Reserved
#include "control/Decoder.h"

#include <cstring>

namespace control {
	Decoder::Result Decoder::Push(const uint8_t* data, size_t length, size_t* consumed)
	{
		if (consumed == nullptr)
			return Result::Rejected;
		*consumed = 0;
		if (data == nullptr && length != 0u)
		{
			rejection_status_ = Status::InvalidPayload;
			return Result::Rejected;
		}

		if (!header_decoded_)
		{
			const size_t remaining = PacketHeaderSize - header_received_;
			const size_t copy_length = length < remaining ? length : remaining;
			std::memcpy(header_bytes_ + header_received_, data, copy_length);
			header_received_ += copy_length;
			*consumed += copy_length;
			data += copy_length;
			length -= copy_length;
			if (header_received_ != PacketHeaderSize)
				return Result::NeedMore;

			const HeaderError error = DecodeRequestHeader(header_bytes_, sizeof(header_bytes_), &header_);
			if (error != HeaderError::None)
			{
				rejection_status_ = HeaderErrorStatus(error);
				return Result::Rejected;
			}
			header_decoded_ = true;
			if (header_.payload_length == 0u)
			{
				if (header_.payload_crc32 != 0u)
				{
					rejection_status_ = Status::PayloadCrcMismatch;
					return Result::Rejected;
				}
				return Result::Complete;
			}
		}

		const size_t remaining = header_.payload_length - payload_received_;
		const size_t copy_length = length < remaining ? length : remaining;
		std::memcpy(payload_ + payload_received_, data, copy_length);
		payload_received_ += copy_length;
		*consumed += copy_length;
		if (payload_received_ != header_.payload_length)
			return Result::NeedMore;
		if (Crc32(payload_, payload_received_) != header_.payload_crc32)
		{
			rejection_status_ = Status::PayloadCrcMismatch;
			return Result::Rejected;
		}
		return Result::Complete;
	}

	void Decoder::Reset()
	{
		header_ = {};
		header_received_ = 0;
		payload_received_ = 0;
		rejection_status_ = Status::Success;
		header_decoded_ = false;
	}
}
