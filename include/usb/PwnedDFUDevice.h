//
// Created by Skye on 7/3/26.
//

#ifndef L41KA_PWNEDDFUDEVICE_H
#define L41KA_PWNEDDFUDEVICE_H

#include <stddef.h>
#include <stdint.h>

#include "usb/DFUDevice.h"

namespace usb {
	class PwnedDFUDevice final : public DFUDevice
	{
	public:
		static constexpr uint16_t ControlMessageBodySize = 0x100;
		static constexpr uint16_t ControlMessageHeaderSize = 0x20;
		static constexpr uint16_t ControlMessageSize = ControlMessageHeaderSize + ControlMessageBodySize;
		static constexpr uint8_t ExecRegisterCount = 8;
		static constexpr uint16_t ExecBodySize =
			ControlMessageBodySize - ExecRegisterCount * sizeof(uint64_t);

		struct ExecResult
		{
			uint64_t registers[ExecRegisterCount] = {0};
			uint8_t body[ExecBodySize] = {0};
			uint16_t body_length = 0;
		};

		PwnedDFUDevice(const PwnedDFUDevice& other) = delete;
		PwnedDFUDevice& operator=(const PwnedDFUDevice& other) = delete;
		PwnedDFUDevice(PwnedDFUDevice&& other) noexcept = default;
		PwnedDFUDevice& operator=(PwnedDFUDevice&& other) noexcept = default;

		int Read(uint64_t address, void* data, size_t length, unsigned int timeout_ms = DFUDevice::DefaultTimeoutMs);
		int Write(
			uint64_t address, const void* data, size_t length, unsigned int timeout_ms = DFUDevice::DefaultTimeoutMs);

		int Exec(uint64_t address, const uint64_t* arguments, size_t argument_count, ExecResult* result,
			const void* suffix = nullptr, uint16_t suffix_length = 0,
			unsigned int timeout_ms = DFUDevice::DefaultTimeoutMs);
		int ExecEl1(uint64_t address, const uint64_t* arguments, size_t argument_count, ExecResult* result,
			const void* suffix = nullptr, uint16_t suffix_length = 0,
			unsigned int timeout_ms = DFUDevice::DefaultTimeoutMs);
		int SetBootLR(uint64_t target_lr, unsigned int timeout_ms = DFUDevice::DefaultTimeoutMs);

	private:
		friend class Device;

		enum class MessageType : uint32_t
		{
			Read = 'r',
			Write = 'w',
			Exec = 'x',
			ExecEl1 = 'X',
			SetBootLR = 'b',
		};

		explicit PwnedDFUDevice(DFUDevice&& dfu);

		int ReadMemory(uint64_t address, void* data, size_t length, unsigned int timeout_ms);
		int WriteMemory(uint64_t address, const void* data, size_t length, unsigned int timeout_ms);
		int ExecMessage(MessageType type, uint64_t address, const uint64_t* arguments, size_t argument_count,
			ExecResult* result, const void* suffix, uint16_t suffix_length, unsigned int timeout_ms);
		int ControlMessage(MessageType type, uint64_t arg, const void* body, uint16_t body_length,
			uint16_t message_length, uint8_t response[ControlMessageSize], unsigned int timeout_ms);
	};
}  // namespace usb

#endif  // L41KA_PWNEDDFUDEVICE_H
