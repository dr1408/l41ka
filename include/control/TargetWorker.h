#ifndef L41KA_CONTROL_TARGET_WORKER_H
#define L41KA_CONTROL_TARGET_WORKER_H

#include <stdint.h>

#include "control/Protocol.h"

namespace control {
	enum class TargetCommandType : uint8_t {
		Poll,
		DfuInfo,
		DfuRawEp0,
		Exploit,
		PhysicalRead,
		PhysicalWrite,
		Execute,
		RecoveryInfo,
		RecoveryReboot,
		LaikaBeginPayload,
		LaikaTriggerPayload,
		LaikaSendEmbeddedPongo,
		PongoBeginModule,
		PongoTriggerModule,
		PongoBeginUploadFile,
		PongoFinishUploadFile,
		UploadChunk,
		IbootBeginPatchfinder,
		IbootTriggerPatchfinder,
		IbootEmbeddedPatchfinder,
		IbootEmbeddedPatchfinderDiag,
		PongoReadOutput,
		PongoSendCommand,
		PongoSendAndTriggerEmbeddedKPF,
		PongoSendAndTriggerEmbeddedRamdisk,
	};

	struct TargetCommand {
		TargetCommandType type;
		uint32_t generation;
		uint32_t request_id;
		uint32_t opcode;
		uint32_t length;
		uint8_t data[MaxPayloadLength];
	};

	struct TargetResult {
		uint32_t generation;
		uint32_t request_id;
		Status status;
		uint32_t length;
		uint8_t data[MaxPayloadLength];
	};

	void TargetWorkerInit();
	bool TargetWorkerSubmit(const TargetCommand& command);
	bool TargetWorkerTryResult(TargetResult* result);
	void TargetWorkerGetConnection(uint32_t* connection_open, DeviceType* device_type);
}

#endif
