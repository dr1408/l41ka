#ifndef L41KA_CONTROL_DECODER_H
#define L41KA_CONTROL_DECODER_H

#include <stddef.h>
#include <stdint.h>

#include "control/Protocol.h"

namespace control {
	class Decoder {
	public:
		enum class Result : uint8_t {
			NeedMore,
			Complete,
			Rejected,
		};

		Result Push(const uint8_t* data, size_t length, size_t* consumed);
		void Reset();

		const PacketHeader& Header() const { return header_; }
		const uint8_t* Payload() const { return payload_; }
		Status RejectionStatus() const { return rejection_status_; }
		size_t BytesNeeded() const
		{
			return header_decoded_ ? header_.payload_length - payload_received_ : PacketHeaderSize - header_received_;
		}

	private:
		uint8_t header_bytes_[PacketHeaderSize] {};
		uint8_t payload_[MaxPayloadLength] {};
		PacketHeader header_ {};
		size_t header_received_ = 0;
		size_t payload_received_ = 0;
		Status rejection_status_ = Status::Success;
		bool header_decoded_ = false;
	};
}

#endif
