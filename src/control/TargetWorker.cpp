// Copyright (c) 0cyn All Rights Reserved
#include "control/TargetWorker.h"

#include <cstdio>
#include <cstring>

#include "control/Fault.h"
#include "control/DfuInfo.h"
#include "../../include/control/Log.h"
#include "exploits/USBLiter8.h"
#include "generated-payloads/checkra1n_kpf_payload.h"
#include "generated-payloads/combined_stage2_payload.h"
#include "generated-payloads/pongo_payload.h"
#include "generated-payloads/ramdisk_payload.h"
#include "pico/multicore.h"
#include "pico/mutex.h"
#include "pico/stdlib.h"
#include "pico/util/queue.h"
#include "payloads/iboot/laikadfu/laikadfu_offsets.h"
#include "payloads/iboot/laikadfu/diag.h"
#include "rom-patcher/iBootPatcherSetup.h"
#include "usb/Device.h"
#include "usb/libusb.h"

namespace control {
	namespace {
		queue_t CommandQueue;
		queue_t ResultQueue;
		mutex_t ConnectionMutex;
		alignas(16) uint32_t WorkerStack[32u * 1024u / sizeof(uint32_t)];
		uint32_t ConnectionOpen;
		DeviceType ConnectedType = DeviceType::None;

		enum class UploadTarget : uint8_t { None, Iboot, Laika, PongoModule, PongoFile };
		struct UploadSession {
			UploadTarget target;
			uint32_t total;
			uint32_t received;
			uint32_t expected_crc;
			uint32_t rolling_crc;
		};
		UploadSession Upload;
		constexpr size_t PongoOutputCapacity = 64u * 1024u;
		uint8_t PongoOutput[PongoOutputCapacity];
		size_t PongoOutputHead;
		size_t PongoOutputSize;
		uint32_t PongoOutputDropped;

		uint32_t ReadU32(const uint8_t* input)
		{
			return static_cast<uint32_t>(input[0]) | (static_cast<uint32_t>(input[1]) << 8u)
				| (static_cast<uint32_t>(input[2]) << 16u) | (static_cast<uint32_t>(input[3]) << 24u);
		}

		uint64_t ReadU64(const uint8_t* input)
		{
			return static_cast<uint64_t>(ReadU32(input)) | (static_cast<uint64_t>(ReadU32(input + 4)) << 32u);
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

		void AppendPongoOutput(const uint8_t* data, size_t length)
		{
			for (size_t i = 0; i < length; ++i)
			{
				if (PongoOutputSize == PongoOutputCapacity)
				{
					PongoOutputHead = (PongoOutputHead + 1u) % PongoOutputCapacity;
					--PongoOutputSize;
					++PongoOutputDropped;
				}
				const size_t tail = (PongoOutputHead + PongoOutputSize) % PongoOutputCapacity;
				PongoOutput[tail] = data[i];
				++PongoOutputSize;
			}
		}

		size_t ConsumePongoOutput(uint8_t* output, size_t capacity)
		{
			const size_t length = PongoOutputSize < capacity ? PongoOutputSize : capacity;
			for (size_t i = 0; i < length; ++i)
				output[i] = PongoOutput[(PongoOutputHead + i) % PongoOutputCapacity];
			PongoOutputHead = (PongoOutputHead + length) % PongoOutputCapacity;
			PongoOutputSize -= length;
			return length;
		}

		DeviceType GetDeviceType(const usb::Device& device)
		{
			if (!device.IsOpen())
				return DeviceType::None;
			if (device.IsPwnedDFU())
				return DeviceType::PwnedDfu;
			if (device.IsDFU())
				return DeviceType::Dfu;
			if (device.IsRecovery())
				return DeviceType::Recovery;
			if (device.IsLaikaDFU())
				return DeviceType::LaikaDfu;
			if (device.IsPongo())
				return DeviceType::Pongo;
			return DeviceType::None;
		}

		void PublishConnection(const usb::Device& device);

		const char* DeviceTypeName(DeviceType type)
		{
			switch (type)
			{
			case DeviceType::Dfu: return "dfu";
			case DeviceType::PwnedDfu: return "pwned-dfu";
			case DeviceType::Recovery: return "recovery";
			case DeviceType::LaikaDfu: return "laika-dfu";
			case DeviceType::Pongo: return "pongo";
			default: return "none";
			}
		}

		bool WaitForDevice(usb::Device& device, DeviceType wanted, uint32_t timeout_ms, const char* label)
		{
			L41KA_LOG(logging::Level::Info, "%s wait begin timeout=%lu want=%s", label,
				static_cast<unsigned long>(timeout_ms), DeviceTypeName(wanted));
			const absolute_time_t started = get_absolute_time();
			const absolute_time_t deadline = delayed_by_ms(started, timeout_ms);
			DeviceType last_type = DeviceType::None;
			bool last_connected = false;
			bool first = true;
			while (!time_reached(deadline))
			{
				if (device.IsOpen() && !usb::Device::IsConnected())
					device.Close();

				if (!device.IsOpen() && usb::Device::IsConnected())
				{
					const int open_rc = usb::Device::Open(&device);
					if (open_rc != LIBUSB_SUCCESS && open_rc != LIBUSB_ERROR_NOT_FOUND)
						L41KA_LOG(logging::Level::Warn, "%s open rc=%d", label, open_rc);
				}

				const bool connected = usb::Device::IsConnected();
				const DeviceType type = GetDeviceType(device);
				if (first || connected != last_connected || type != last_type)
				{
					const int64_t elapsed = absolute_time_diff_us(started, get_absolute_time()) / 1000;
					L41KA_LOG(logging::Level::Info, "%s poll %lldms connected=%lu type=%s", label,
						static_cast<long long>(elapsed), static_cast<unsigned long>(connected ? 1u : 0u),
						DeviceTypeName(type));
					first = false;
					last_connected = connected;
					last_type = type;
				}
				PublishConnection(device);
				if (type == wanted)
				{
					const int64_t elapsed = absolute_time_diff_us(started, get_absolute_time()) / 1000;
					L41KA_LOG(logging::Level::Info, "%s ok after=%lldms", label, static_cast<long long>(elapsed));
					return true;
				}
				sleep_ms(500);
			}
			L41KA_LOG(logging::Level::Warn, "%s timeout last=%s connected=%lu", label, DeviceTypeName(last_type),
				static_cast<unsigned long>(last_connected ? 1u : 0u));
			return false;
		}

		void PassiveWaitForUsb(usb::Device& device, uint32_t timeout_ms, const char* label)
		{
			if (device.IsOpen())
				device.Close();
			PublishConnection(device);
			L41KA_LOG(logging::Level::Info, "%s passive wait begin timeout=%lu", label,
				static_cast<unsigned long>(timeout_ms));
			const absolute_time_t started = get_absolute_time();
			const absolute_time_t deadline = delayed_by_ms(started, timeout_ms);
			bool last_connected = false;
			bool first = true;
			while (!time_reached(deadline))
			{
				const bool connected = usb::Device::IsConnected();
				if (first || connected != last_connected)
				{
					const int64_t elapsed = absolute_time_diff_us(started, get_absolute_time()) / 1000;
					L41KA_LOG(logging::Level::Info, "%s passive poll %lldms connected=%lu", label,
						static_cast<long long>(elapsed), static_cast<unsigned long>(connected ? 1u : 0u));
					first = false;
					last_connected = connected;
				}
				sleep_ms(500);
			}
			L41KA_LOG(logging::Level::Info, "%s passive timeout connected=%lu", label,
				static_cast<unsigned long>(last_connected ? 1u : 0u));
		}

		void PublishConnection(const usb::Device& device)
		{
			const DeviceType type = GetDeviceType(device);
			mutex_enter_blocking(&ConnectionMutex);
			ConnectedType = type;
			ConnectionOpen = type == DeviceType::None ? 0u : 1u;
			mutex_exit(&ConnectionMutex);
		}

		void SendPollResult(const TargetCommand& command, const usb::Device& device)
		{
			TargetResult result {};
			result.generation = command.generation;
			result.request_id = command.request_id;
			result.status = Status::Success;
			result.length = 8;
			const uint32_t open = device.IsOpen() ? 1u : 0u;
			const uint32_t type = static_cast<uint32_t>(GetDeviceType(device));
			std::memcpy(result.data, &open, sizeof(open));
			std::memcpy(result.data + 4, &type, sizeof(type));
			queue_add_blocking(&ResultQueue, &result);
		}

		void SendDfuInfo(const TargetCommand& command, usb::Device& device)
		{
			if (!device.IsOpen() || !device.IsDFU())
				PanicTarget(FaultReason::TargetStateChanged, command.request_id, command.opcode);
			char serial[usb::DFUDevice::MaxSerialLength] {};
			const int serial_length = device.AsDFU()->Serial(serial, sizeof(serial));
			if (serial_length < 0)
				PanicTarget(FaultReason::TargetUsbFailure, command.request_id, command.opcode,
					static_cast<uint32_t>(serial_length));
			char nonce_string[usb::DFUDevice::MaxSerialLength] {};
			const int nonce_length = device.AsDFU()->StringDescriptor(1, nonce_string, sizeof(nonce_string));
			if (nonce_length < 0)
				nonce_string[0] = 0;
			DfuInfo info {};
			ParseDfuInfo(serial, nonce_string, &info);
			info.pid = 0x1227;
			info.interface_number = device.AsDFU()->InterfaceNumber();
			TargetResult result {};
			result.generation = command.generation;
			result.request_id = command.request_id;
			result.status = Status::Success;
			result.length = static_cast<uint32_t>(DfuInfoWireSize);
			EncodeDfuInfo(info, result.data);
			queue_add_blocking(&ResultQueue, &result);
		}

		void Poll(const TargetCommand& command, usb::Device& device)
		{
			if (device.IsOpen() && usb::Device::IsConnected())
			{
				PublishConnection(device);
				return SendPollResult(command, device);
			}
			if (device.IsOpen())
			{
				device.Close();
				Upload = {};
			}
			if (!usb::Device::IsConnected())
			{
				Upload = {};
				PublishConnection(device);
				return SendPollResult(command, device);
			}

			const int rc = usb::Device::Open(&device);
			if (rc != LIBUSB_SUCCESS)
			{
				// A target can disappear or still be enumerating while this probe resets EP0.
				// Treat that as an empty sample; a later probe can retry without rebooting the Pi.
				device.Close();
				PublishConnection(device);
				return SendPollResult(command, device);
			}
			PublishConnection(device);
			L41KA_LOG(logging::Level::Info, "target opened: %s", DeviceTypeName(GetDeviceType(device)));
			SendPollResult(command, device);
		}

		void Exploit(const TargetCommand& command, usb::Device& device)
		{
			if (!device.IsOpen() || !device.IsDFU() || device.IsPwnedDFU())
				PanicTarget(FaultReason::TargetStateChanged, command.request_id, command.opcode);
			L41KA_LOG(logging::Level::Info, "dfu exploit started");
			const USBLiter8::Result result = USBLiter8::Run(*device.AsDFU());
			if (result.usb_code != LIBUSB_SUCCESS)
			{
				L41KA_LOG(logging::Level::Fatal, "dfu exploit failed in %s, l41ka will reset now.", result.error_message);
				PanicTarget(FaultReason::ExploitFailed, command.request_id, command.opcode,
					static_cast<uint32_t>(result.usb_code));
			}
			device.Close();
			PublishConnection(device);
			L41KA_LOG(logging::Level::Info, "dfu exploit completed");
			WaitForDevice(device, DeviceType::PwnedDfu, 15000, "wait-pwned-dfu");
		}

		void DfuRawEp0(const TargetCommand& command, usb::Device& device)
		{
			usb::DFUDevice* dfu = device.AsDFU();
			if (!device.IsOpen() || dfu == nullptr)
				PanicTarget(FaultReason::TargetStateChanged, command.request_id, command.opcode);
			const auto token_pid = static_cast<usb::DFUDevice::RawTokenPid>(command.data[0]);
			const auto data_pid = static_cast<usb::DFUDevice::RawDataPid>(command.data[1]);
			const uint16_t payload_length = static_cast<uint16_t>(command.data[2])
				| static_cast<uint16_t>(command.data[3] << 8u);
			const uint32_t count = ReadU32(command.data + 4);
			const uint32_t delay_us = ReadU32(command.data + 8);
			TargetResult result {};
			result.generation = command.generation;
			result.request_id = command.request_id;
			result.status = Status::Success;
			result.length = count;
			for (uint32_t i = 0; i < count; ++i)
			{
				usb::DFUDevice::RawHandshake handshake = usb::DFUDevice::RawHandshake::Timeout;
				const int rc = dfu->RawEp0DataTransfer(
					token_pid, data_pid, command.data + 16, payload_length, &handshake);
				if (rc != LIBUSB_SUCCESS)
					PanicTarget(FaultReason::TargetUsbFailure, command.request_id, command.opcode,
						static_cast<uint32_t>(rc));
				result.data[i] = static_cast<uint8_t>(handshake);
				if (delay_us != 0u && i + 1u != count)
					sleep_us(delay_us);
			}
			queue_add_blocking(&ResultQueue, &result);
		}

		usb::PwnedDFUDevice& RequirePwnedDfu(const TargetCommand& command, usb::Device& device)
		{
			if (!device.IsOpen() || !device.IsPwnedDFU())
				PanicTarget(FaultReason::TargetStateChanged, command.request_id, command.opcode);
			return *device.AsPwnedDFU();
		}

		void PhysicalRead(const TargetCommand& command, usb::Device& device)
		{
			auto& pwned = RequirePwnedDfu(command, device);
			TargetResult result {};
			result.generation = command.generation;
			result.request_id = command.request_id;
			result.status = Status::Success;
			result.length = ReadU32(command.data + 8);
			const int rc = pwned.Read(ReadU64(command.data), result.data, result.length);
			if (rc != LIBUSB_SUCCESS)
				PanicTarget(FaultReason::TargetUsbFailure, command.request_id, command.opcode,
					static_cast<uint32_t>(rc));
			queue_add_blocking(&ResultQueue, &result);
		}

		void PhysicalWrite(const TargetCommand& command, usb::Device& device)
		{
			auto& pwned = RequirePwnedDfu(command, device);
			const int rc = pwned.Write(ReadU64(command.data), command.data + 8, command.length - 8u);
			if (rc != LIBUSB_SUCCESS)
				PanicTarget(FaultReason::TargetUsbFailure, command.request_id, command.opcode,
					static_cast<uint32_t>(rc));
			TargetResult result {};
			result.generation = command.generation;
			result.request_id = command.request_id;
			result.status = Status::Success;
			queue_add_blocking(&ResultQueue, &result);
		}

		void Execute(const TargetCommand& command, usb::Device& device)
		{
			auto& pwned = RequirePwnedDfu(command, device);
			const uint32_t flags = ReadU32(command.data + 8);
			const uint32_t argument_count = ReadU32(command.data + 12);
			uint64_t arguments[8] {};
			for (size_t i = 0; i < 8u; ++i)
				arguments[i] = ReadU64(command.data + 16u + i * sizeof(uint64_t));
			const uint16_t suffix_length = command.length > 80u ?
				static_cast<uint16_t>(command.length - 80u) : 0u;
			const void* suffix = suffix_length != 0u ? command.data + 80u : nullptr;
			usb::PwnedDFUDevice::ExecResult execution {};
			const int rc = (flags & 1u) != 0u ?
				pwned.ExecEl1(ReadU64(command.data), arguments, argument_count, &execution, suffix, suffix_length) :
				pwned.Exec(ReadU64(command.data), arguments, argument_count, &execution, suffix, suffix_length);
			if (rc != LIBUSB_SUCCESS)
				PanicTarget(FaultReason::TargetUsbFailure, command.request_id, command.opcode,
					static_cast<uint32_t>(rc));
			TargetResult result {};
			result.generation = command.generation;
			result.request_id = command.request_id;
			result.status = Status::Success;
			result.length = 68u + execution.body_length;
			for (size_t i = 0; i < 8u; ++i)
				WriteU64(result.data + i * sizeof(uint64_t), execution.registers[i]);
			WriteU32(result.data + 64, execution.body_length);
			std::memcpy(result.data + 68, execution.body, execution.body_length);
			queue_add_blocking(&ResultQueue, &result);
		}

		struct LaikaDfuDryRunOffsets {
			const char* soc;
			uint64_t usb_clock0;
			uint64_t usb_clock1;
			uint64_t usb_clock2;
			uint64_t usb_complex;
			uint64_t usb_phy;
			uint64_t dwc2_base;
			uint64_t dart_base;
			uint32_t usb_complex_control;
			uint32_t usb_phy_cfg0;
			uint32_t dart_tlb_stream_mask;
			uint32_t dart_wait_for_tlb;
		};

		bool LaikaDfuDryRunGetOffsets(uint32_t cpid, LaikaDfuDryRunOffsets* offsets)
		{
			if (offsets == nullptr)
				return false;
			if (cpid == 0x8020u)
			{
				*offsets = {
					"t8020",
					0x23b0802a8ull, 0x23b0802b0ull, 0x23b0802a8ull,
					0x239000000ull, 0x239000064ull, 0x239100000ull, 0x239900000ull,
					1u, 0x27373af3u, 1u, 0u
				};
				return true;
			}
			if (cpid == 0x8030u)
			{
				*offsets = {
					"t8030",
					0x23b0802e8ull, 0x23b0802f0ull, 0x23b0802f8ull,
					0x239000000ull, 0x239000064ull, 0x239100000ull, 0x239028000ull,
					0u, 0x27373bf3u, 0xfu, 1u
				};
				return true;
			}
			return false;
		}

		int PwnedRead32(usb::PwnedDFUDevice& pwned, uint64_t address, uint32_t* value)
		{
			return pwned.Read(address, value, sizeof(*value), 1000u);
		}

		int PwnedWrite32(usb::PwnedDFUDevice& pwned, uint64_t address, uint32_t value)
		{
			return pwned.Write(address, &value, sizeof(value), 1000u);
		}

		int DryRunReadLog(usb::PwnedDFUDevice& pwned, const char* name, uint64_t address, uint32_t* value)
		{
			const int rc = PwnedRead32(pwned, address, value);
			L41KA_LOG(rc == LIBUSB_SUCCESS ? logging::Level::Info : logging::Level::Warn,
				"laikadfu dry-run read %-14s addr=0x%llx rc=%d value=0x%08lx",
				name, static_cast<unsigned long long>(address), rc,
				static_cast<unsigned long>(rc == LIBUSB_SUCCESS ? *value : 0u));
			return rc;
		}

		int DryRunWriteLog(usb::PwnedDFUDevice& pwned, const char* name, uint64_t address, uint32_t value)
		{
			L41KA_LOG(logging::Level::Info, "laikadfu dry-run write %-13s addr=0x%llx value=0x%08lx",
				name, static_cast<unsigned long long>(address), static_cast<unsigned long>(value));
			const int rc = PwnedWrite32(pwned, address, value);
			uint32_t readback = 0;
			const int read_rc = rc == LIBUSB_SUCCESS ? PwnedRead32(pwned, address, &readback) : rc;
			L41KA_LOG(read_rc == LIBUSB_SUCCESS ? logging::Level::Info : logging::Level::Warn,
				"laikadfu dry-run verify %-12s rc=%d read=0x%08lx match=%lu",
				name, read_rc, static_cast<unsigned long>(readback),
				static_cast<unsigned long>(read_rc == LIBUSB_SUCCESS && readback == value ? 1u : 0u));
			return rc == LIBUSB_SUCCESS ? read_rc : rc;
		}

		int DryRunClockGate(usb::PwnedDFUDevice& pwned, const char* name, uint64_t address, bool enable,
			uint32_t* observed)
		{
			uint32_t value = 0;
			int rc = DryRunReadLog(pwned, name, address, &value);
			if (rc != LIBUSB_SUCCESS)
				return rc;
			value = enable ? value | 0x0fu : (value & ~0x0fu) | 4u;
			rc = DryRunWriteLog(pwned, name, address, value);
			if (rc != LIBUSB_SUCCESS)
				return rc;
			for (uint32_t timeout = 0x1000u; timeout != 0; --timeout)
			{
				rc = PwnedRead32(pwned, address, &value);
				if (rc != LIBUSB_SUCCESS)
					return rc;
				if ((value & 0x0fu) == ((value >> 4u) & 0x0fu))
					break;
			}
			if (observed != nullptr)
				*observed = value;
			L41KA_LOG(logging::Level::Info, "laikadfu dry-run clock %-12s enable=%lu final=0x%08lx settled=%lu",
				name, static_cast<unsigned long>(enable ? 1u : 0u), static_cast<unsigned long>(value),
				static_cast<unsigned long>((value & 0x0fu) == ((value >> 4u) & 0x0fu) ? 1u : 0u));
			return LIBUSB_SUCCESS;
		}

		void LaikaDfuDryRun(const TargetCommand& command, usb::Device& device)
		{
			auto& pwned = RequirePwnedDfu(command, device);
			const uint32_t cpid = ReadU32(command.data);
			const uint32_t stage = ReadU32(command.data + 4);
			LaikaDfuDryRunOffsets offsets {};
			if (!LaikaDfuDryRunGetOffsets(cpid == 0u ? pwned.CPID() : cpid, &offsets))
			{
				TargetResult result {};
				result.generation = command.generation;
				result.request_id = command.request_id;
				result.status = Status::InvalidPayload;
				queue_add_blocking(&ResultQueue, &result);
				return;
			}

			uint32_t last_stage = 0;
			uint32_t last_rc = LIBUSB_SUCCESS;
			uint32_t snapshot[8] {};
			auto fail = [&](uint32_t failed_stage, int rc) {
				last_stage = failed_stage;
				last_rc = static_cast<uint32_t>(rc);
				L41KA_LOG(logging::Level::Warn, "laikadfu dry-run fail stage=%lu rc=%d",
					static_cast<unsigned long>(failed_stage), rc);
				return rc;
			};

			L41KA_LOG(logging::Level::Info,
				"laikadfu dry-run begin soc=%s cpid=0x%lx stage=%lu clock0=0x%llx clock1=0x%llx clock2=0x%llx",
				offsets.soc, static_cast<unsigned long>(cpid == 0u ? pwned.CPID() : cpid),
				static_cast<unsigned long>(stage),
				static_cast<unsigned long long>(offsets.usb_clock0),
				static_cast<unsigned long long>(offsets.usb_clock1),
				static_cast<unsigned long long>(offsets.usb_clock2));
			L41KA_LOG(logging::Level::Info,
				"laikadfu dry-run bases complex=0x%llx phy=0x%llx dwc2=0x%llx dart=0x%llx",
				static_cast<unsigned long long>(offsets.usb_complex),
				static_cast<unsigned long long>(offsets.usb_phy),
				static_cast<unsigned long long>(offsets.dwc2_base),
				static_cast<unsigned long long>(offsets.dart_base));

			last_stage = 1;
			if (DryRunReadLog(pwned, "clk0", offsets.usb_clock0, &snapshot[0]) != LIBUSB_SUCCESS
				|| DryRunReadLog(pwned, "clk1", offsets.usb_clock1, &snapshot[1]) != LIBUSB_SUCCESS
				|| DryRunReadLog(pwned, "clk2", offsets.usb_clock2, &snapshot[2]) != LIBUSB_SUCCESS
				|| DryRunReadLog(pwned, "complex0", offsets.usb_complex, &snapshot[3]) != LIBUSB_SUCCESS
				|| DryRunReadLog(pwned, "phy0", offsets.usb_phy, &snapshot[4]) != LIBUSB_SUCCESS
				|| DryRunReadLog(pwned, "grstctl", offsets.dwc2_base + 0x10u, &snapshot[5]) != LIBUSB_SUCCESS
				|| DryRunReadLog(pwned, "gintsts", offsets.dwc2_base + 0x14u, &snapshot[6]) != LIBUSB_SUCCESS
				|| DryRunReadLog(pwned, "dctl", offsets.dwc2_base + 0x804u, &snapshot[7]) != LIBUSB_SUCCESS)
				fail(last_stage, LIBUSB_ERROR_IO);

			if (last_rc == LIBUSB_SUCCESS && stage >= 2u)
			{
				last_stage = 2;
				if (DryRunClockGate(pwned, "clk0", offsets.usb_clock0, false, &snapshot[0]) != LIBUSB_SUCCESS
					|| DryRunClockGate(pwned, "clk1", offsets.usb_clock1, false, &snapshot[1]) != LIBUSB_SUCCESS
					|| DryRunClockGate(pwned, "clk2", offsets.usb_clock2, false, &snapshot[2]) != LIBUSB_SUCCESS)
					fail(last_stage, LIBUSB_ERROR_IO);
				sleep_us(1000u);
			}

			if (last_rc == LIBUSB_SUCCESS && stage >= 3u)
			{
				last_stage = 3;
				if (DryRunClockGate(pwned, "clk0", offsets.usb_clock0, true, &snapshot[0]) != LIBUSB_SUCCESS
					|| DryRunClockGate(pwned, "clk1", offsets.usb_clock1, true, &snapshot[1]) != LIBUSB_SUCCESS
					|| DryRunClockGate(pwned, "clk2", offsets.usb_clock2, true, &snapshot[2]) != LIBUSB_SUCCESS
					|| DryRunWriteLog(pwned, "complex+0", offsets.usb_complex, offsets.usb_complex_control) != LIBUSB_SUCCESS
					|| DryRunWriteLog(pwned, "complex+48", offsets.usb_complex + 0x48u, 0x03000088u) != LIBUSB_SUCCESS
					|| DryRunWriteLog(pwned, "complex+68", offsets.usb_complex + 0x68u, offsets.usb_phy_cfg0) != LIBUSB_SUCCESS
					|| DryRunWriteLog(pwned, "complex+6c", offsets.usb_complex + 0x6cu, 0x00020c44u) != LIBUSB_SUCCESS)
					fail(last_stage, LIBUSB_ERROR_IO);
				uint32_t value = 0;
				if (last_rc == LIBUSB_SUCCESS && PwnedRead32(pwned, offsets.usb_complex + 0x60u, &value) == LIBUSB_SUCCESS)
				{
					DryRunWriteLog(pwned, "complex+60", offsets.usb_complex + 0x60u, value | 1u);
					sleep_us(20u);
					PwnedRead32(pwned, offsets.usb_complex + 0x60u, &value);
					DryRunWriteLog(pwned, "complex+60", offsets.usb_complex + 0x60u, value & ~0x0cu);
					sleep_us(20u);
					PwnedRead32(pwned, offsets.usb_complex + 0x60u, &value);
					DryRunWriteLog(pwned, "complex+60", offsets.usb_complex + 0x60u, value & ~1u);
					sleep_us(20u);
					PwnedRead32(pwned, offsets.usb_complex + 0x64u, &value);
					DryRunWriteLog(pwned, "complex+64", offsets.usb_complex + 0x64u, value & ~2u);
					sleep_us(1500u);
				}
			}

			if (last_rc == LIBUSB_SUCCESS && stage >= 4u)
			{
				last_stage = 4;
				if (DryRunWriteLog(pwned, "dart+200", offsets.dart_base + 0x200u, 0) != LIBUSB_SUCCESS
					|| DryRunWriteLog(pwned, "dart+204", offsets.dart_base + 0x204u, 0) != LIBUSB_SUCCESS
					|| DryRunWriteLog(pwned, "dart+208", offsets.dart_base + 0x208u, 0) != LIBUSB_SUCCESS
					|| DryRunWriteLog(pwned, "dart+20c", offsets.dart_base + 0x20cu, 0) != LIBUSB_SUCCESS
					|| DryRunWriteLog(pwned, "dart+34", offsets.dart_base + 0x34u, offsets.dart_tlb_stream_mask) != LIBUSB_SUCCESS
					|| DryRunWriteLog(pwned, "dart+20", offsets.dart_base + 0x20u, 0) != LIBUSB_SUCCESS)
					fail(last_stage, LIBUSB_ERROR_IO);
				uint32_t value = 0;
				if (last_rc == LIBUSB_SUCCESS && offsets.dart_wait_for_tlb != 0u)
					for (uint32_t timeout = 0x1000u; timeout != 0; --timeout)
						if (PwnedRead32(pwned, offsets.dart_base + 0x20u, &value) != LIBUSB_SUCCESS || (value & 4u) == 0)
							break;
				if (last_rc == LIBUSB_SUCCESS
					&& PwnedRead32(pwned, offsets.dart_base + 0x40u, &value) == LIBUSB_SUCCESS
					&& DryRunWriteLog(pwned, "dart+40", offsets.dart_base + 0x40u, value) == LIBUSB_SUCCESS
					&& PwnedRead32(pwned, offsets.dart_base + 0x100u, &value) == LIBUSB_SUCCESS)
					DryRunWriteLog(pwned, "dart+100", offsets.dart_base + 0x100u, (value & 0xff00fe7fu) | 0x00010100u);
			}

			if (last_rc == LIBUSB_SUCCESS && stage >= 5u)
			{
				last_stage = 5;
				if (DryRunWriteLog(pwned, "grstctl", offsets.dwc2_base + 0x10u, 1u) != LIBUSB_SUCCESS)
					fail(last_stage, LIBUSB_ERROR_IO);
				uint32_t value = 0;
				uint32_t timeout = 0x1000u;
				for (; timeout != 0; --timeout)
					if (PwnedRead32(pwned, offsets.dwc2_base + 0x10u, &value) != LIBUSB_SUCCESS || (value & 1u) == 0)
						break;
				L41KA_LOG(logging::Level::Info, "laikadfu dry-run grstctl reset-final=0x%08lx timeout_left=0x%lx",
					static_cast<unsigned long>(value), static_cast<unsigned long>(timeout));
				if (PwnedRead32(pwned, offsets.dwc2_base + 0x804u, &value) == LIBUSB_SUCCESS
					&& DryRunWriteLog(pwned, "dctl-disconnect", offsets.dwc2_base + 0x804u, value | 2u) == LIBUSB_SUCCESS)
				{
					timeout = 0x1000u;
					for (; timeout != 0; --timeout)
						if (PwnedRead32(pwned, offsets.dwc2_base + 0x10u, &value) != LIBUSB_SUCCESS
							|| (value & 0x80000000u) != 0)
							break;
					L41KA_LOG(logging::Level::Info, "laikadfu dry-run ahb-idle grstctl=0x%08lx timeout_left=0x%lx",
						static_cast<unsigned long>(value), static_cast<unsigned long>(timeout));
				}
			}

			if (last_rc == LIBUSB_SUCCESS && stage >= 6u)
			{
				last_stage = 6;
				uint32_t value = 0;
				DryRunWriteLog(pwned, "gahbcfg", offsets.dwc2_base + 0x008u, 0x2eu);
				DryRunWriteLog(pwned, "gusbcfg", offsets.dwc2_base + 0x00cu, 0x1408u);
				DryRunWriteLog(pwned, "dcfg", offsets.dwc2_base + 0x800u, 4u);
				DryRunWriteLog(pwned, "gintmsk", offsets.dwc2_base + 0x018u, 0u);
				DryRunWriteLog(pwned, "doepmsk", offsets.dwc2_base + 0x814u, 0u);
				DryRunWriteLog(pwned, "diepmsk", offsets.dwc2_base + 0x810u, 0u);
				DryRunWriteLog(pwned, "daintmsk", offsets.dwc2_base + 0x81cu, 0u);
				DryRunWriteLog(pwned, "diepint0", offsets.dwc2_base + 0x908u, 0x1fu);
				DryRunWriteLog(pwned, "doepint0", offsets.dwc2_base + 0xb08u, 0x0fu);
				DryRunWriteLog(pwned, "gintmsk", offsets.dwc2_base + 0x018u, 0x1000u);
				DryRunWriteLog(pwned, "gintsts", offsets.dwc2_base + 0x014u, 0x1000u);
				if (PwnedRead32(pwned, offsets.dwc2_base + 0x804u, &value) == LIBUSB_SUCCESS)
					DryRunWriteLog(pwned, "dctl-connect", offsets.dwc2_base + 0x804u, value & ~2u);
				if (PwnedRead32(pwned, offsets.usb_phy, &value) == LIBUSB_SUCCESS)
					DryRunWriteLog(pwned, "phy-connect", offsets.usb_phy, value | 2u);
				uint32_t gintsts = 0;
				PwnedRead32(pwned, offsets.dwc2_base + 0x014u, &gintsts);
				L41KA_LOG(logging::Level::Info, "laikadfu dry-run connect-post gintsts=0x%08lx enumdone=%lu",
					static_cast<unsigned long>(gintsts), static_cast<unsigned long>((gintsts & 0x1000u) != 0 ? 1u : 0u));
				snapshot[5] = value;
				snapshot[6] = gintsts;
			}

			TargetResult result {};
			result.generation = command.generation;
			result.request_id = command.request_id;
			result.status = Status::Success;
			result.length = 40u;
			WriteU32(result.data, last_stage);
			WriteU32(result.data + 4, last_rc);
			for (size_t i = 0; i < 8u; ++i)
				WriteU32(result.data + 8u + i * sizeof(uint32_t), snapshot[i]);
			L41KA_LOG(logging::Level::Info, "laikadfu dry-run done last_stage=%lu rc=%ld",
				static_cast<unsigned long>(last_stage), static_cast<long>(static_cast<int32_t>(last_rc)));
			queue_add_blocking(&ResultQueue, &result);
		}

		usb::RecoveryDevice& RequireRecovery(const TargetCommand& command, usb::Device& device)
		{
			if (!device.IsOpen() || !device.IsRecovery())
				PanicTarget(FaultReason::TargetStateChanged, command.request_id, command.opcode);
			return *device.AsRecovery();
		}

		void RecoveryInfo(const TargetCommand& command, usb::Device& device)
		{
			auto& recovery = RequireRecovery(command, device);
			char serial[usb::RecoveryDevice::MaxSerialLength] {};
			const int serial_length = recovery.Serial(serial, sizeof(serial));
			if (serial_length < 0)
				PanicTarget(FaultReason::TargetUsbFailure, command.request_id, command.opcode,
					static_cast<uint32_t>(serial_length));
			char nonce_string[usb::RecoveryDevice::MaxSerialLength] {};
			if (recovery.StringDescriptor(1, nonce_string, sizeof(nonce_string)) < 0)
				nonce_string[0] = 0;
			DfuInfo info {};
			ParseDfuInfo(serial, nonce_string, &info);
			info.pid = 0x1281;
			TargetResult result {};
			result.generation = command.generation;
			result.request_id = command.request_id;
			result.status = Status::Success;
			result.length = static_cast<uint32_t>(DfuInfoWireSize);
			EncodeDfuInfo(info, result.data);
			queue_add_blocking(&ResultQueue, &result);
		}

		void RecoveryReboot(const TargetCommand& command, usb::Device& device)
		{
			auto& recovery = RequireRecovery(command, device);
			const int rc = recovery.Reboot();
			if (rc < 0)
				PanicTarget(FaultReason::TargetUsbFailure, command.request_id, command.opcode,
					static_cast<uint32_t>(rc));
			device.Close();
			PublishConnection(device);
			TargetResult result {};
			result.generation = command.generation;
			result.request_id = command.request_id;
			result.status = Status::Success;
			queue_add_blocking(&ResultQueue, &result);
		}

		void SendStatus(const TargetCommand& command, Status status, uint32_t value = 0, bool include_value = false)
		{
			TargetResult result {};
			result.generation = command.generation;
			result.request_id = command.request_id;
			result.status = status;
			if (include_value)
			{
				result.length = 4;
				WriteU32(result.data, value);
			}
			queue_add_blocking(&ResultQueue, &result);
		}

		void BeginUpload(const TargetCommand& command, usb::Device& device, UploadTarget target)
		{
			if (Upload.target != UploadTarget::None)
				return SendStatus(command, Status::UploadAlreadyActive);
			const uint32_t total = ReadU32(command.data);
			const uint32_t expected_crc = ReadU32(command.data + 4);
			int rc = LIBUSB_ERROR_INVALID_PARAM;
			if (target == UploadTarget::Iboot)
			{
				if (!device.IsOpen() || !device.IsPwnedDFU())
					PanicTarget(FaultReason::TargetStateChanged, command.request_id, command.opcode);
				if (iBootPatcherSetup::UploadedPayloadAddress(*device.AsPwnedDFU()) == 0u)
					return SendStatus(command, Status::InvalidPayload);
				rc = LIBUSB_SUCCESS;
			}
			else if (target == UploadTarget::Laika)
			{
				if (!device.IsOpen() || !device.IsLaikaDFU())
					PanicTarget(FaultReason::TargetStateChanged, command.request_id, command.opcode);
				rc = device.AsLaikaDFU()->BeginUpload(total);
			}
			else
			{
				if (!device.IsOpen() || !device.IsPongo())
					PanicTarget(FaultReason::TargetStateChanged, command.request_id, command.opcode);
				rc = device.AsPongo()->BeginUpload(total);
			}
			if (rc != LIBUSB_SUCCESS)
				PanicTarget(FaultReason::TargetUsbFailure, command.request_id, command.opcode,
					static_cast<uint32_t>(rc));
			Upload = {target, total, 0, expected_crc, 0};
			SendStatus(command, Status::Success);
		}

		void UploadChunk(const TargetCommand& command, usb::Device& device)
		{
			if (Upload.target == UploadTarget::None)
				return SendStatus(command, Status::UploadNotActive);
			const uint32_t offset = ReadU32(command.data);
			const uint32_t length = command.length - 4u;
			if (offset != Upload.received)
				return SendStatus(command, Status::UploadWrongOffset);
			if (length > Upload.total - Upload.received)
				return SendStatus(command, Status::UploadExceedsLength);

			int rc = LIBUSB_ERROR_INVALID_PARAM;
			if (Upload.target == UploadTarget::Iboot)
			{
				if (!device.IsOpen() || !device.IsPwnedDFU())
					PanicTarget(FaultReason::TargetStateChanged, command.request_id, command.opcode);
				const uint64_t address = iBootPatcherSetup::UploadedPayloadAddress(*device.AsPwnedDFU());
				rc = address == 0u ? LIBUSB_ERROR_NOT_SUPPORTED :
					device.AsPwnedDFU()->Write(address + offset, command.data + 4, length);
			}
			else if (Upload.target == UploadTarget::Laika)
			{
				if (!device.IsOpen() || !device.IsLaikaDFU())
					PanicTarget(FaultReason::TargetStateChanged, command.request_id, command.opcode);
				rc = device.AsLaikaDFU()->SendUploadChunk(command.data + 4, length);
			}
			else
			{
				if (!device.IsOpen() || !device.IsPongo())
					PanicTarget(FaultReason::TargetStateChanged, command.request_id, command.opcode);
				rc = device.AsPongo()->SendUploadChunk(command.data + 4, length);
			}
			const bool complete = Upload.target == UploadTarget::Iboot ? rc == LIBUSB_SUCCESS :
				rc == static_cast<int>(length);
			if (rc < 0 || !complete)
				PanicTarget(FaultReason::TargetUsbFailure, command.request_id, command.opcode,
					static_cast<uint32_t>(rc < 0 ? rc : LIBUSB_ERROR_IO));
			Upload.rolling_crc = Crc32(command.data + 4, length, Upload.rolling_crc);
			Upload.received += length;
			SendStatus(command, Status::Success, Upload.received, true);
		}

		bool ValidateUploadTrigger(const TargetCommand& command, usb::Device& device, UploadTarget target)
		{
			if (Upload.target == UploadTarget::None)
			{
				SendStatus(command, Status::UploadNotActive);
				return false;
			}
			if (Upload.target != target)
			{
				SendStatus(command, Status::WrongDeviceType);
				return false;
			}
			if (Upload.received != Upload.total)
			{
				SendStatus(command, Status::UploadIncomplete);
				return false;
			}
			if (Upload.rolling_crc != Upload.expected_crc)
			{
				SendStatus(command, Status::UploadCrcMismatch);
				Upload = {};
				device.Close();
				PublishConnection(device);
				return false;
			}
			return true;
		}

		void TriggerLaika(const TargetCommand& command, usb::Device& device)
		{
			if (!ValidateUploadTrigger(command, device, UploadTarget::Laika)) return;
			SendStatus(command, Status::Success);
			const int rc = device.AsLaikaDFU()->FinishUpload();
			Upload = {};
			if (rc != LIBUSB_SUCCESS)
				PanicTarget(FaultReason::TargetUsbFailure, command.request_id, command.opcode,
					static_cast<uint32_t>(rc));
			device.Close();
			PublishConnection(device);
		}

		void SendEmbeddedPongo(const TargetCommand& command, usb::Device& device)
		{
			if (!device.IsOpen() || !device.IsLaikaDFU())
				PanicTarget(FaultReason::TargetStateChanged, command.request_id, command.opcode);
			SendStatus(command, Status::Success);
			const int rc = device.AsLaikaDFU()->SendBuffer(pongo_payload, pongo_payload_len);
			if (rc < 0)
				PanicTarget(FaultReason::TargetUsbFailure, command.request_id, command.opcode,
					static_cast<uint32_t>(rc));
			device.Close();
			PublishConnection(device);
		}

		void TriggerPongo(const TargetCommand& command, usb::Device& device)
		{
			if (!ValidateUploadTrigger(command, device, UploadTarget::PongoModule)) return;
			SendStatus(command, Status::Success);
			int rc = device.AsPongo()->FinishUpload();
			if (rc == LIBUSB_SUCCESS)
				rc = device.AsPongo()->SendCommand("modload");
			Upload = {};
			if (rc < 0)
				PanicTarget(FaultReason::TargetUsbFailure, command.request_id, command.opcode,
					static_cast<uint32_t>(rc));
			if (rc != 8)
				PanicTarget(FaultReason::TargetShortTransfer, command.request_id, command.opcode,
					static_cast<uint32_t>(rc));
		}

		void FinishPongoUploadFile(const TargetCommand& command, usb::Device& device)
		{
			if (!ValidateUploadTrigger(command, device, UploadTarget::PongoFile)) return;
			SendStatus(command, Status::Success);
			const int rc = device.AsPongo()->FinishUpload();
			Upload = {};
			if (rc != LIBUSB_SUCCESS)
				PanicTarget(FaultReason::TargetUsbFailure, command.request_id, command.opcode,
					static_cast<uint32_t>(rc));
		}

		usb::PongoDevice& RequirePongo(const TargetCommand& command, usb::Device& device)
		{
			if (!device.IsOpen() || !device.IsPongo())
				PanicTarget(FaultReason::TargetStateChanged, command.request_id, command.opcode);
			return *device.AsPongo();
		}

		void WaitForPongoCommand(const TargetCommand& command, usb::PongoDevice& pongo)
		{
			uint8_t output[usb::PongoDevice::StdoutReadLength];
			const absolute_time_t deadline = make_timeout_time_ms(60000u);
			bool running = true;
			while (running)
			{
				if (time_reached(deadline))
					PanicTarget(FaultReason::TargetUsbFailure, command.request_id, command.opcode,
						static_cast<uint32_t>(LIBUSB_ERROR_TIMEOUT));
				int rc = pongo.CommandInProgress(&running, 100u);
				if (rc == LIBUSB_SUCCESS)
					rc = pongo.ReadStdout(output, sizeof(output), 100u);
				if (rc < 0)
					PanicTarget(FaultReason::TargetUsbFailure, command.request_id, command.opcode,
						static_cast<uint32_t>(rc));
				AppendPongoOutput(output, static_cast<size_t>(rc));
			}
		}

		void PongoUploadFile(const TargetCommand& command, usb::PongoDevice& pongo,
			const void* data, size_t length)
		{
			const int rc = pongo.SendBuffer(data, length);
			if (rc < 0)
				PanicTarget(FaultReason::TargetUsbFailure, command.request_id, command.opcode,
					static_cast<uint32_t>(rc));
			if (rc != static_cast<int>(length))
				PanicTarget(FaultReason::TargetShortTransfer, command.request_id, command.opcode,
					static_cast<uint32_t>(rc));
		}

		void SendAndTriggerEmbeddedKPF(const TargetCommand& command, usb::Device& device)
		{
			auto& pongo = RequirePongo(command, device);
			PongoUploadFile(command, pongo, checkra1n_kpf_payload, checkra1n_kpf_payload_len);
			int rc = pongo.SendCommand("modload");
			if (rc < 0)
				PanicTarget(FaultReason::TargetUsbFailure, command.request_id, command.opcode,
					static_cast<uint32_t>(rc));
			if (rc != 8)
				PanicTarget(FaultReason::TargetShortTransfer, command.request_id, command.opcode,
					static_cast<uint32_t>(rc));
			WaitForPongoCommand(command, pongo);
			SendStatus(command, Status::Success);
		}

		void SendAndTriggerEmbeddedRamdisk(const TargetCommand& command, usb::Device& device)
		{
			auto& pongo = RequirePongo(command, device);
			if (ramdisk_payload_len == 0u)
			{
				SendStatus(command, Status::InvalidPayload);
				return;
			}

			PongoUploadFile(command, pongo, ramdisk_payload, ramdisk_payload_len);

			char ramdisk_command[32];
			const int command_length = std::snprintf(
				ramdisk_command, sizeof(ramdisk_command), "ramdisk %u",
				static_cast<unsigned int>(L41KA_RAMDISK_UNCOMPRESSED_SIZE));
			if (command_length <= 0 || command_length >= static_cast<int>(sizeof(ramdisk_command)))
				PanicTarget(FaultReason::Bug, command.request_id, command.opcode);
			int rc = pongo.SendCommand(ramdisk_command);
			if (rc < 0)
				PanicTarget(FaultReason::TargetUsbFailure, command.request_id, command.opcode,
					static_cast<uint32_t>(rc));
			if (rc != command_length + 1)
				PanicTarget(FaultReason::TargetShortTransfer, command.request_id, command.opcode,
					static_cast<uint32_t>(rc));
			WaitForPongoCommand(command, pongo);
			SendStatus(command, Status::Success);
		}

		void TriggerIboot(const TargetCommand& command, usb::Device& device)
		{
			if (!ValidateUploadTrigger(command, device, UploadTarget::Iboot)) return;
			if (Upload.total < sizeof(laikadfu_offsets))
			{
				Upload = {};
				SendStatus(command, Status::InvalidPayload);
				return;
			}
			const uint32_t length = Upload.total;
			SendStatus(command, Status::Success);
			L41KA_LOG(logging::Level::Info, "setup-iboot uploaded begin len=%lu", static_cast<unsigned long>(length));
			int rc = iBootPatcherSetup::RunUploaded(*device.AsPwnedDFU(), length);
			L41KA_LOG(logging::Level::Info, "setup-iboot uploaded run rc=%d", rc);
			if (rc == LIBUSB_SUCCESS)
			{
				L41KA_LOG(logging::Level::Info, "setup-iboot uploaded dfu-abort submit");
				rc = device.AsPwnedDFU()->Abort();
				L41KA_LOG(logging::Level::Info, "setup-iboot uploaded dfu-abort rc=%d", rc);
			}
			Upload = {};
			if (rc != LIBUSB_SUCCESS)
				PanicTarget(FaultReason::TargetUsbFailure, command.request_id, command.opcode,
					static_cast<uint32_t>(rc));
			device.Close();
			PublishConnection(device);
			WaitForDevice(device, DeviceType::LaikaDfu, 30000, "wait-laikadfu");
		}

		void EmbeddedIbootWithMode(const TargetCommand& command, usb::Device& device, uint32_t diag_mode, bool active_wait)
		{
			if (!device.IsOpen() || !device.IsPwnedDFU())
				PanicTarget(FaultReason::TargetStateChanged, command.request_id, command.opcode);
			SendStatus(command, Status::Success);
			uint8_t sanity[16] {};
			int sanity_rc = device.AsPwnedDFU()->Read(0x100000200ull, sanity, sizeof(sanity), 1000);
			L41KA_LOG(logging::Level::Info,
				"pwneddfu read-test addr=0x100000200 len=16 rc=%d bytes=%02x%02x%02x%02x",
				sanity_rc, sanity[0], sanity[1], sanity[2], sanity[3]);
			L41KA_LOG(logging::Level::Info, "setup-iboot embedded begin cpid=0x%lx payload=%lu diag_mode=%lu active_wait=%lu",
				static_cast<unsigned long>(device.AsPwnedDFU()->CPID()),
				static_cast<unsigned long>(combined_stage2_payload_len),
				static_cast<unsigned long>(diag_mode), static_cast<unsigned long>(active_wait ? 1u : 0u));
			int rc = iBootPatcherSetup::Run(*device.AsPwnedDFU(), diag_mode);
			L41KA_LOG(logging::Level::Info, "setup-iboot embedded run rc=%d", rc);
			if (rc == LIBUSB_SUCCESS)
			{
				L41KA_LOG(logging::Level::Info, "setup-iboot embedded dfu-abort submit");
				rc = device.AsPwnedDFU()->Abort();
				L41KA_LOG(logging::Level::Info, "setup-iboot embedded dfu-abort rc=%d", rc);
			}
			if (rc != LIBUSB_SUCCESS)
				PanicTarget(FaultReason::TargetUsbFailure, command.request_id, command.opcode,
					static_cast<uint32_t>(rc));
			device.Close();
			PublishConnection(device);
			if (active_wait)
				WaitForDevice(device, DeviceType::LaikaDfu, 30000, "wait-laikadfu");
			else
				PassiveWaitForUsb(device, 30000, "wait-laikadfu");
		}

		void EmbeddedIboot(const TargetCommand& command, usb::Device& device)
		{
			EmbeddedIbootWithMode(command, device, LAIKADFU_DIAG_NONE, true);
		}

		void EmbeddedIbootDiag(const TargetCommand& command, usb::Device& device)
		{
			if (command.length != 8u)
				PanicTarget(FaultReason::Bug, command.request_id, command.opcode, command.length);
			const uint32_t diag_mode = ReadU32(command.data);
			const bool active_wait = ReadU32(command.data + 4) != 0;
			EmbeddedIbootWithMode(command, device, diag_mode, active_wait);
		}

		void PongoSendCommand(const TargetCommand& command, usb::Device& device)
		{
			auto& pongo = RequirePongo(command, device);
			const uint32_t timeout_ms = ReadU32(command.data);
			char text[512] {};
			std::memcpy(text, command.data + 4, command.length - 4u);
			const size_t text_length = command.length - 4u;
			const int expected = static_cast<int>(
				text_length + (text[text_length - 1u] == '\n' ? 0u : 1u));
			const bool resets_target = std::strcmp(text, "reset") == 0 || std::strcmp(text, "reset\n") == 0;
			int rc = pongo.ResetIo(1000u);
			if (rc != LIBUSB_SUCCESS)
				PanicTarget(FaultReason::TargetUsbFailure, command.request_id, command.opcode,
					static_cast<uint32_t>(rc));
			rc = pongo.SendCommand(text, 1000u);
			if (resets_target && (rc < 0 || rc == expected))
			{
				device.Close();
				PublishConnection(device);
				SendStatus(command, Status::Success);
				return;
			}
			if (rc < 0)
				PanicTarget(FaultReason::TargetUsbFailure, command.request_id, command.opcode,
					static_cast<uint32_t>(rc));
			if (rc != expected)
				PanicTarget(FaultReason::TargetShortTransfer, command.request_id, command.opcode,
					static_cast<uint32_t>(rc));

			const absolute_time_t deadline = make_timeout_time_ms(timeout_ms);
			bool running = true;
			uint8_t output[512];
			while (running)
			{
				if (time_reached(deadline))
					PanicTarget(FaultReason::TargetUsbFailure, command.request_id, command.opcode,
						static_cast<uint32_t>(LIBUSB_ERROR_TIMEOUT));
				rc = pongo.CommandInProgress(&running, 100u);
				if (rc == LIBUSB_SUCCESS)
					rc = pongo.ReadStdout(output, sizeof(output), 100u);
				if (rc < 0)
					PanicTarget(FaultReason::TargetUsbFailure, command.request_id, command.opcode,
						static_cast<uint32_t>(rc));
				AppendPongoOutput(output, static_cast<size_t>(rc));
			}
			SendStatus(command, Status::Success);
		}

		void PongoReadOutput(const TargetCommand& command, usb::Device& device)
		{
			auto& pongo = RequirePongo(command, device);
			bool running = false;
			int rc = pongo.CommandInProgress(&running, 100u);
			uint8_t target_output[usb::PongoDevice::StdoutReadLength];
			if (rc == LIBUSB_SUCCESS)
				rc = pongo.ReadStdout(target_output, sizeof(target_output), 100u);
			if (rc < 0)
				PanicTarget(FaultReason::TargetUsbFailure, command.request_id, command.opcode,
					static_cast<uint32_t>(rc));
			AppendPongoOutput(target_output, static_cast<size_t>(rc));

			TargetResult result {};
			result.generation = command.generation;
			result.request_id = command.request_id;
			result.status = Status::Success;
			const uint32_t dropped = PongoOutputDropped;
			PongoOutputDropped = 0;
			const size_t returned = ConsumePongoOutput(result.data + 12, MaxPayloadLength - 12u);
			const bool more = PongoOutputSize != 0u || running || rc == usb::PongoDevice::StdoutReadLength;
			WriteU32(result.data, static_cast<uint32_t>(returned));
			WriteU32(result.data + 4, more ? 1u : 0u);
			WriteU32(result.data + 8, dropped);
			result.length = static_cast<uint32_t>(12u + returned);
			queue_add_blocking(&ResultQueue, &result);
		}

		void WorkerEntry()
		{
			usb::Device device;
			while (true)
			{
				TargetCommand command {};
				queue_remove_blocking(&CommandQueue, &command);
				switch (command.type)
				{
				case TargetCommandType::Poll:
					Poll(command, device);
					break;
				case TargetCommandType::DfuInfo:
					SendDfuInfo(command, device);
					break;
				case TargetCommandType::DfuRawEp0:
					DfuRawEp0(command, device);
					break;
				case TargetCommandType::Exploit:
					Exploit(command, device);
					break;
				case TargetCommandType::PhysicalRead:
					PhysicalRead(command, device);
					break;
				case TargetCommandType::PhysicalWrite:
					PhysicalWrite(command, device);
					break;
				case TargetCommandType::Execute:
					Execute(command, device);
					break;
				case TargetCommandType::RecoveryInfo:
					RecoveryInfo(command, device);
					break;
				case TargetCommandType::RecoveryReboot:
					RecoveryReboot(command, device);
					break;
				case TargetCommandType::LaikaBeginPayload:
					BeginUpload(command, device, UploadTarget::Laika);
					break;
				case TargetCommandType::LaikaTriggerPayload:
					TriggerLaika(command, device);
					break;
				case TargetCommandType::LaikaSendEmbeddedPongo:
					SendEmbeddedPongo(command, device);
					break;
				case TargetCommandType::PongoBeginModule:
					BeginUpload(command, device, UploadTarget::PongoModule);
					break;
				case TargetCommandType::PongoTriggerModule:
					TriggerPongo(command, device);
					break;
				case TargetCommandType::PongoBeginUploadFile:
					BeginUpload(command, device, UploadTarget::PongoFile);
					break;
				case TargetCommandType::PongoFinishUploadFile:
					FinishPongoUploadFile(command, device);
					break;
				case TargetCommandType::PongoSendAndTriggerEmbeddedKPF:
					SendAndTriggerEmbeddedKPF(command, device);
					break;
				case TargetCommandType::PongoSendAndTriggerEmbeddedRamdisk:
					SendAndTriggerEmbeddedRamdisk(command, device);
					break;
				case TargetCommandType::UploadChunk:
					UploadChunk(command, device);
					break;
				case TargetCommandType::IbootBeginPatchfinder:
					BeginUpload(command, device, UploadTarget::Iboot);
					break;
				case TargetCommandType::IbootTriggerPatchfinder:
					TriggerIboot(command, device);
					break;
				case TargetCommandType::IbootEmbeddedPatchfinder:
					EmbeddedIboot(command, device);
					break;
				case TargetCommandType::IbootEmbeddedPatchfinderDiag:
					EmbeddedIbootDiag(command, device);
					break;
				case TargetCommandType::LaikaDfuDryRun:
					LaikaDfuDryRun(command, device);
					break;
				case TargetCommandType::PongoReadOutput:
					PongoReadOutput(command, device);
					break;
				case TargetCommandType::PongoSendCommand:
					PongoSendCommand(command, device);
					break;
				default:
					PanicTarget(FaultReason::Bug, command.request_id, command.opcode);
				}
			}
		}
	}

	void TargetWorkerInit()
	{
		queue_init(&CommandQueue, sizeof(TargetCommand), 1);
		queue_init(&ResultQueue, sizeof(TargetResult), 1);
		mutex_init(&ConnectionMutex);
		multicore_launch_core1_with_stack(WorkerEntry, WorkerStack, sizeof(WorkerStack));
	}

	bool TargetWorkerSubmit(const TargetCommand& command)
	{
		return queue_try_add(&CommandQueue, &command);
	}

	bool TargetWorkerTryResult(TargetResult* result)
	{
		return result != nullptr && queue_try_remove(&ResultQueue, result);
	}

	void TargetWorkerGetConnection(uint32_t* connection_open, DeviceType* device_type)
	{
		mutex_enter_blocking(&ConnectionMutex);
		if (connection_open != nullptr)
			*connection_open = ConnectionOpen;
		if (device_type != nullptr)
			*device_type = ConnectedType;
		mutex_exit(&ConnectionMutex);
	}
}
