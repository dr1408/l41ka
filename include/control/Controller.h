#ifndef L41KA_CONTROL_CONTROLLER_H
#define L41KA_CONTROL_CONTROLLER_H

#include <stddef.h>
#include <stdint.h>

#include "control/Decoder.h"
#include "control/TargetWorker.h"

namespace control {
	class Controller {
	public:
		void Run();
		void OnMount();
		void OnUnmount();
		void OnTransmit(uint32_t bytes);

	private:
		Decoder decoder_;
		PacketHeader pending_header_ {};
		bool request_pending_ = false;
		bool mounted_ = false;
		bool responding_ = false;
		bool reject_next_as_pipelined_ = false;
		uint32_t transmitted_ = 0;
		uint32_t generation_ = 1;
		uint8_t transmit_packet_[PacketHeaderSize + MaxPayloadLength] {};
		TargetCommand target_command_ {};
		TargetResult target_result_ {};

		void ServiceUsb();
		void ProcessInput();
		void ProcessTargetResult();
		void Dispatch(const PacketHeader& header, const uint8_t* payload);
		void SendResponse(const PacketHeader& request, Status status, const void* payload = nullptr, size_t length = 0);
		void SendDecoderRejection();
	};
}

#endif
