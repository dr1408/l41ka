// Copyright (c) 0cyn All Rights Reserved
#include "control/Controller.h"

#include <algorithm>
#include <cstring>

#include "control/Fault.h"
#include "../../include/control/Log.h"
#include "generated-payloads/combined_stage2_payload.h"
#include "generated-payloads/pongo_payload.h"
#include "generated-payloads/ramdisk_payload.h"
#include "hardware/watchdog.h"
#include "payloads/iboot/laikadfu/laikadfu_offsets.h"
#include "pico/bootrom.h"
#include "pico/time.h"
#include "tusb.h"
#include "usb/DFUDevice.h"
#include "usb/LaikaDFUDevice.h"
#include "usb/PongoDevice.h"

#define L41KA_STRINGIFY_INNER(value) #value
#define L41KA_STRINGIFY(value) L41KA_STRINGIFY_INNER(value)

namespace {
	control::Controller* ActiveController;
	constexpr char FirmwareVersion[] = L41KA_STRINGIFY(L41KA_VERSION_MAJOR) "."
		L41KA_STRINGIFY(L41KA_VERSION_MINOR) "." L41KA_STRINGIFY(L41KA_VERSION_PATCH);

	void WriteU32(uint8_t* output, uint32_t value)
	{
		output[0] = static_cast<uint8_t>(value);
		output[1] = static_cast<uint8_t>(value >> 8u);
		output[2] = static_cast<uint8_t>(value >> 16u);
		output[3] = static_cast<uint8_t>(value >> 24u);
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
}  // namespace

#undef L41KA_STRINGIFY
#undef L41KA_STRINGIFY_INNER

namespace control {
	void Controller::Run()
	{
		ActiveController = this;
		logging::Init();
		L41KA_LOG(logging::Level::Info,
			"fw boot version=%s board=%s stage2_len=%lu pongo_len=%lu ramdisk_len=%lu",
			FirmwareVersion, L41KA_BOARD_NAME,
			static_cast<unsigned long>(combined_stage2_payload_len),
			static_cast<unsigned long>(pongo_payload_len),
			static_cast<unsigned long>(ramdisk_payload_len));
		TargetWorkerInit();
		const tusb_rhport_init_t device_init = {
			.role = TUSB_ROLE_DEVICE,
			.speed = TUSB_SPEED_AUTO,
		};
		if (!tusb_init(0, &device_init))
			PanicTarget(FaultReason::Bug, 0, 0, 1);

		while (true)
		{
			ServiceUsb();
			ProcessTargetResult();
			if (!request_pending_ && !responding_)
				ProcessInput();
		}
	}

	void Controller::OnMount()
	{
		mounted_ = true;
		L41KA_LOG(logging::Level::Info, "usb control mounted");
	}

	void Controller::OnUnmount()
	{
		L41KA_LOG(logging::Level::Warn, "usb control unmounted");
		mounted_ = false;
		responding_ = false;
		request_pending_ = false;
		reject_next_as_pipelined_ = false;
		transmitted_ = 0;
		decoder_.Reset();
		if (++generation_ == 0u)
			generation_ = 1u;
	}

	void Controller::OnTransmit(uint32_t bytes)
	{
		transmitted_ += bytes;
	}

	void Controller::ServiceUsb()
	{
		tud_task();
		logging::Drain();
		if (tud_cdc_available() != 0u)
			tud_cdc_read_flush();
		if ((responding_ || request_pending_) && tud_vendor_available() != 0u)
			reject_next_as_pipelined_ = true;
	}

	void Controller::ProcessInput()
	{
		if (!mounted_ || tud_vendor_available() == 0u)
			return;
		uint8_t input[512];
		const uint32_t requested = static_cast<uint32_t>(
			std::min<size_t>(sizeof(input), std::min<size_t>(decoder_.BytesNeeded(), tud_vendor_available())));
		if (requested == 0u)
			return;
		const uint32_t received = tud_vendor_read(input, requested);
		size_t consumed = 0;
		const Decoder::Result result = decoder_.Push(input, received, &consumed);
		if (consumed != received)
			PanicTarget(FaultReason::Bug, 0, 0, static_cast<uint32_t>(consumed));
		if (result == Decoder::Result::NeedMore)
			return;
		if (result == Decoder::Result::Rejected)
		{
			SendDecoderRejection();
			decoder_.Reset();
			return;
		}

		const PacketHeader header = decoder_.Header();
		if (reject_next_as_pipelined_)
		{
			reject_next_as_pipelined_ = false;
			SendResponse(header, Status::PipelinedRequest);
		}
		else
			Dispatch(header, decoder_.Payload());
		decoder_.Reset();
	}

	void Controller::ProcessTargetResult()
	{
		while (TargetWorkerTryResult(&target_result_))
		{
			if (target_result_.generation != generation_ || !request_pending_)
				continue;
			if (target_result_.request_id != pending_header_.request_id)
				PanicTarget(FaultReason::Bug, pending_header_.request_id, pending_header_.opcode,
					target_result_.request_id);
			request_pending_ = false;
			SendResponse(pending_header_, target_result_.status, target_result_.data, target_result_.length);
		}
	}

	void Controller::Dispatch(const PacketHeader& header, const uint8_t* payload)
	{
		const Opcode opcode = static_cast<Opcode>(header.opcode);
		auto require_empty = [&]() {
			if (header.payload_length == 0u)
				return true;
			SendResponse(header, Status::InvalidPayload);
			return false;
		};
		auto require_pwned_dfu = [&]() {
			uint32_t open = 0;
			DeviceType type = DeviceType::None;
			TargetWorkerGetConnection(&open, &type);
			if (open == 0u)
			{
				SendResponse(header, Status::NoConnection);
				return false;
			}
			if (type != DeviceType::PwnedDfu)
			{
				SendResponse(header, Status::WrongDeviceType);
				return false;
			}
			return true;
		};
		auto require_dfu = [&]() {
			uint32_t open = 0;
			DeviceType type = DeviceType::None;
			TargetWorkerGetConnection(&open, &type);
			if (open == 0u)
			{
				SendResponse(header, Status::NoConnection);
				return false;
			}
			if (type != DeviceType::Dfu && type != DeviceType::PwnedDfu)
			{
				SendResponse(header, Status::WrongDeviceType);
				return false;
			}
			return true;
		};
		auto require_recovery = [&]() {
			uint32_t open = 0;
			DeviceType type = DeviceType::None;
			TargetWorkerGetConnection(&open, &type);
			if (open == 0u)
			{
				SendResponse(header, Status::NoConnection);
				return false;
			}
			if (type != DeviceType::Recovery)
			{
				SendResponse(header, Status::WrongDeviceType);
				return false;
			}
			return true;
		};
		auto require_device = [&](DeviceType required) {
			uint32_t open = 0;
			DeviceType type = DeviceType::None;
			TargetWorkerGetConnection(&open, &type);
			if (open == 0u)
			{
				SendResponse(header, Status::NoConnection);
				return false;
			}
			if (type != required)
			{
				SendResponse(header, Status::WrongDeviceType);
				return false;
			}
			return true;
		};
		auto submit_synchronous = [&](TargetCommandType type) {
			target_command_ = {};
			target_command_.type = type;
			target_command_.generation = generation_;
			target_command_.request_id = header.request_id;
			target_command_.opcode = header.opcode;
			target_command_.length = header.payload_length;
			if (header.payload_length != 0u)
				std::memcpy(target_command_.data, payload, header.payload_length);
			if (!TargetWorkerSubmit(target_command_))
			{
				SendResponse(header, Status::CommandBusy);
				return;
			}
			pending_header_ = header;
			request_pending_ = true;
		};

		switch (opcode)
		{
		case Opcode::PiCheckErrors:
		{
			if (!require_empty())
				return;
			const ErrorRecord record = ConsumeErrorRecord();
			uint8_t response[32];
			WriteU32(response, record.present);
			WriteU32(response + 4, record.reason);
			WriteU32(response + 8, record.request_id);
			WriteU32(response + 12, record.opcode);
			WriteU32(response + 16, record.protocol_context);
			WriteU32(response + 20, record.observed_value);
			WriteU32(response + 24, record.panic_uptime_ms);
			WriteU32(response + 28, 0);
			return SendResponse(header, Status::Success, response, sizeof(response));
		}
		case Opcode::PiGetInfo:
		{
			if (!require_empty())
				return;
			static_assert(sizeof(BOARD_NAME) <= 64u);
			static_assert(sizeof(FirmwareVersion) <= 64u);
			uint8_t response[128] {};
			std::memcpy(response, BOARD_NAME, sizeof(BOARD_NAME));
			std::memcpy(response + 64u, FirmwareVersion, sizeof(FirmwareVersion));
			return SendResponse(header, Status::Success, response, sizeof(response));
		}
		case Opcode::PiGetOpenConnection:
		{
			if (!require_empty())
				return;
			uint32_t open = 0;
			DeviceType type = DeviceType::None;
			TargetWorkerGetConnection(&open, &type);
			uint8_t response[8];
			WriteU32(response, open);
			WriteU32(response + 4, static_cast<uint32_t>(type));
			return SendResponse(header, Status::Success, response, sizeof(response));
		}
		case Opcode::PiPollForDevice:
		{
			if (!require_empty())
				return;
			submit_synchronous(TargetCommandType::Poll);
			return;
		}
		case Opcode::PiResetPi:
			if (!require_empty())
				return;
			SendResponse(header, Status::Success);
			ClearErrorRecord();
			watchdog_reboot(0, 0, 100);
			while (true)
				ServiceUsb();
		case Opcode::PiEnterBOOTSEL:
		{
			if (!require_empty())
				return;
			SendResponse(header, Status::Success);
			const absolute_time_t deadline = make_timeout_time_ms(100u);
			while (responding_ && mounted_ && !time_reached(deadline))
				ServiceUsb();
			ClearErrorRecord();
			rom_reset_usb_boot(0, 0);
		}
		case Opcode::DeviceDfuExploit:
		{
			if (!require_empty())
				return;
			uint32_t open = 0;
			DeviceType type = DeviceType::None;
			TargetWorkerGetConnection(&open, &type);
			if (open == 0u)
				return SendResponse(header, Status::NoConnection);
			if (type != DeviceType::Dfu)
				return SendResponse(header, Status::WrongDeviceType);
			target_command_ = {};
			target_command_.type = TargetCommandType::Exploit;
			target_command_.generation = generation_;
			target_command_.request_id = header.request_id;
			target_command_.opcode = header.opcode;
			if (!TargetWorkerSubmit(target_command_))
				return SendResponse(header, Status::CommandBusy);
			return SendResponse(header, Status::Success);
		}
		case Opcode::DeviceDfuInfo:
		{
			if (!require_empty())
				return;
			uint32_t open = 0;
			DeviceType type = DeviceType::None;
			TargetWorkerGetConnection(&open, &type);
			if (open == 0u)
				return SendResponse(header, Status::NoConnection);
			if (type != DeviceType::Dfu && type != DeviceType::PwnedDfu)
				return SendResponse(header, Status::WrongDeviceType);
			submit_synchronous(TargetCommandType::DfuInfo);
			return;
		}
		case Opcode::DeviceDfuRawEp0:
		{
			if (header.payload_length < 16u)
				return SendResponse(header, Status::InvalidPayload);
			const uint8_t token_pid = payload[0];
			const uint8_t data_pid = payload[1];
			const uint32_t payload_length = static_cast<uint32_t>(payload[2])
				| (static_cast<uint32_t>(payload[3]) << 8u);
			const uint32_t count = ReadU32(payload + 4);
			const uint32_t delay_us = ReadU32(payload + 8);
			if ((token_pid != 0x2du && token_pid != 0xe1u) || (data_pid != 0xc3u && data_pid != 0x4bu)
				|| payload_length > usb::DFUDevice::RawEp0MaxPayloadLength || count == 0u
				|| count > MaxPayloadLength || delay_us > 1000000u || ReadU32(payload + 12) != 0u
				|| header.payload_length != 16u + payload_length)
				return SendResponse(header, Status::InvalidPayload);
			if (!require_dfu())
				return;
			submit_synchronous(TargetCommandType::DfuRawEp0);
			return;
		}
		case Opcode::DevicePwnedDfuPhysRead:
		{
			if (header.payload_length != 16u)
				return SendResponse(header, Status::InvalidPayload);
			const uint32_t length = ReadU32(payload + 8);
			const uint64_t address = ReadU64(payload);
			if (length == 0u || length > MaxPayloadLength || ReadU32(payload + 12) != 0u
				|| static_cast<uint64_t>(length - 1u) > UINT64_MAX - address)
				return SendResponse(header, Status::InvalidPayload);
			if (!require_pwned_dfu())
				return;
			submit_synchronous(TargetCommandType::PhysicalRead);
			return;
		}
		case Opcode::DevicePwnedDfuPhysWrite:
		{
			if (header.payload_length < 9u)
				return SendResponse(header, Status::InvalidPayload);
			const uint64_t address = ReadU64(payload);
			const uint32_t length = header.payload_length - 8u;
			if (static_cast<uint64_t>(length - 1u) > UINT64_MAX - address)
				return SendResponse(header, Status::InvalidPayload);
			if (!require_pwned_dfu())
				return;
			submit_synchronous(TargetCommandType::PhysicalWrite);
			return;
		}
		case Opcode::DevicePwnedDfuExecute:
		{
			if (header.payload_length < 80u || header.payload_length > 272u)
				return SendResponse(header, Status::InvalidPayload);
			const uint32_t flags = ReadU32(payload + 8);
			const uint32_t argument_count = ReadU32(payload + 12);
			if ((flags & ~1u) != 0u || argument_count > 8u)
				return SendResponse(header, Status::InvalidPayload);
			for (size_t i = argument_count; i < 8u; ++i)
				if (ReadU64(payload + 16u + i * sizeof(uint64_t)) != 0u)
					return SendResponse(header, Status::InvalidPayload);
			if (!require_pwned_dfu())
				return;
			submit_synchronous(TargetCommandType::Execute);
			return;
		}
		case Opcode::DeviceRecoveryInfo:
			if (!require_empty() || !require_recovery())
				return;
			submit_synchronous(TargetCommandType::RecoveryInfo);
			return;
		case Opcode::DeviceRecoveryReboot:
			if (!require_empty() || !require_recovery())
				return;
			submit_synchronous(TargetCommandType::RecoveryReboot);
			return;
		case Opcode::DeviceLaikaDfuSendPayload:
			if (header.payload_length != 8u || ReadU32(payload) == 0u
				|| ReadU32(payload) > usb::LaikaDFUDevice::MaxPayloadLength)
				return SendResponse(header, Status::InvalidPayload);
			if (!require_device(DeviceType::LaikaDfu))
				return;
			submit_synchronous(TargetCommandType::LaikaBeginPayload);
			return;
		case Opcode::DeviceLaikaDfuTriggerPayload:
			if (!require_empty() || !require_device(DeviceType::LaikaDfu))
				return;
			submit_synchronous(TargetCommandType::LaikaTriggerPayload);
			return;
		case Opcode::DeviceLaikaDfuSendEmbeddedPongoOs:
			if (!require_empty() || !require_device(DeviceType::LaikaDfu))
				return;
			submit_synchronous(TargetCommandType::LaikaSendEmbeddedPongo);
			return;
		case Opcode::DevicePongoSendModule:
			if (header.payload_length != 8u || ReadU32(payload) == 0u
				|| ReadU32(payload) > usb::PongoDevice::MaxUploadLength)
				return SendResponse(header, Status::InvalidPayload);
			if (!require_device(DeviceType::Pongo))
				return;
			submit_synchronous(TargetCommandType::PongoBeginModule);
			return;
		case Opcode::DevicePongoTriggerModule:
			if (!require_empty() || !require_device(DeviceType::Pongo))
				return;
			submit_synchronous(TargetCommandType::PongoTriggerModule);
			return;
		case Opcode::DevicePongoBeginUploadFile:
			if (header.payload_length != 8u || ReadU32(payload) == 0u
				|| ReadU32(payload) > usb::PongoDevice::MaxUploadLength)
				return SendResponse(header, Status::InvalidPayload);
			if (!require_device(DeviceType::Pongo))
				return;
			submit_synchronous(TargetCommandType::PongoBeginUploadFile);
			return;
		case Opcode::DevicePongoFinishUploadFile:
			if (!require_empty() || !require_device(DeviceType::Pongo))
				return;
			submit_synchronous(TargetCommandType::PongoFinishUploadFile);
			return;
		case Opcode::DevicePongoSendAndTriggerEmbeddedKPF:
			if (!require_empty() || !require_device(DeviceType::Pongo))
				return;
			submit_synchronous(TargetCommandType::PongoSendAndTriggerEmbeddedKPF);
			return;
		case Opcode::DevicePongoSendAndTriggerEmbeddedRamdisk:
			if (!require_empty() || !require_device(DeviceType::Pongo))
				return;
			submit_synchronous(TargetCommandType::PongoSendAndTriggerEmbeddedRamdisk);
			return;
		case Opcode::GenericUploadChunk:
			if (header.payload_length < 5u)
				return SendResponse(header, Status::InvalidPayload);
			submit_synchronous(TargetCommandType::UploadChunk);
			return;
		case Opcode::DevicePwnedDfuSendIbootPatchfinder:
			if (header.payload_length != 8u || ReadU32(payload) == 0u || ReadU32(payload) >= 16384u)
				return SendResponse(header, Status::InvalidPayload);
			if (!require_pwned_dfu())
				return;
			submit_synchronous(TargetCommandType::IbootBeginPatchfinder);
			return;
		case Opcode::DevicePwnedDfuTriggerIbootPatchfinder:
			if (!require_empty() || !require_pwned_dfu())
				return;
			submit_synchronous(TargetCommandType::IbootTriggerPatchfinder);
			return;
		case Opcode::DevicePwnedDfuSendEmbeddedIbootPatchfinderAndBoot:
			if (!require_empty() || !require_pwned_dfu())
				return;
			submit_synchronous(TargetCommandType::IbootEmbeddedPatchfinder);
			return;
		case Opcode::DevicePwnedDfuSendEmbeddedIbootPatchfinderDiag:
			if (header.payload_length != 8u)
				return SendResponse(header, Status::InvalidPayload);
			if (!require_pwned_dfu())
				return;
			submit_synchronous(TargetCommandType::IbootEmbeddedPatchfinderDiag);
			return;
		case Opcode::DevicePwnedDfuLaikaDfuDryRun:
			if (header.payload_length != 8u)
				return SendResponse(header, Status::InvalidPayload);
			if (ReadU32(payload + 4) > 6u)
				return SendResponse(header, Status::InvalidPayload);
			if (!require_pwned_dfu())
				return;
			submit_synchronous(TargetCommandType::LaikaDfuDryRun);
			return;
		case Opcode::DevicePongoReadOutput:
			if (!require_empty() || !require_device(DeviceType::Pongo))
				return;
			submit_synchronous(TargetCommandType::PongoReadOutput);
			return;
		case Opcode::DevicePongoSendCommand:
		{
			if (header.payload_length < 5u || header.payload_length > 515u)
				return SendResponse(header, Status::InvalidPayload);
			const uint32_t timeout_ms = ReadU32(payload);
			const size_t command_length = header.payload_length - 4u;
			if (timeout_ms == 0u || timeout_ms > 60000u || std::memchr(payload + 4, 0, command_length) != nullptr)
				return SendResponse(header, Status::InvalidPayload);
			if (!require_device(DeviceType::Pongo))
				return;
			submit_synchronous(TargetCommandType::PongoSendCommand);
			return;
		}
		default:
			(void)payload;
			return SendResponse(header, Status::UnknownOpcode);
		}
	}

	void Controller::SendResponse(const PacketHeader& request, Status status, const void* payload, size_t length)
	{
		if (length > MaxPayloadLength || (payload == nullptr && length != 0u))
			PanicTarget(
				FaultReason::Bug, request.request_id, request.opcode, static_cast<uint32_t>(length));
		PacketHeader response {
			.magic = PacketMagic,
			.version = ProtocolVersion,
			.type = PacketType::Response,
			.reserved = 0,
			.opcode = request.opcode,
			.request_id = request.request_id,
			.payload_length = static_cast<uint32_t>(length),
			.status = static_cast<uint32_t>(status),
			.payload_crc32 = Crc32(payload, length),
		};
		EncodeHeader(response, transmit_packet_);
		if (length != 0u)
			std::memcpy(transmit_packet_ + PacketHeaderSize, payload, length);

		responding_ = true;
		transmitted_ = 0;
		const uint32_t total = static_cast<uint32_t>(PacketHeaderSize + length);
		uint32_t queued = 0;
		while (mounted_ && transmitted_ < total)
		{
			ServiceUsb();
			if (queued < total)
			{
				queued += tud_vendor_write(transmit_packet_ + queued, total - queued);
				tud_vendor_write_flush();
			}
		}
		responding_ = false;
	}

	void Controller::SendDecoderRejection()
	{
		PacketHeader request = decoder_.Header();
		if (request.magic != PacketMagic)
		{
			request.opcode = 0;
			request.request_id = 0;
		}
		else if (request.request_id == 0u)
			request.request_id = 0;
		SendResponse(request, decoder_.RejectionStatus());
		tud_vendor_read_flush();
	}
}  // namespace control

void tud_mount_cb()
{
	if (ActiveController != nullptr)
		ActiveController->OnMount();
}

void tud_umount_cb()
{
	if (ActiveController != nullptr)
		ActiveController->OnUnmount();
}

void tud_suspend_cb(bool remote_wakeup_enabled)
{
	(void)remote_wakeup_enabled;
}

void tud_resume_cb() {}

void tud_vendor_tx_cb(uint8_t interface, uint32_t sent_bytes)
{
	(void)interface;
	if (ActiveController != nullptr)
		ActiveController->OnTransmit(sent_bytes);
}
