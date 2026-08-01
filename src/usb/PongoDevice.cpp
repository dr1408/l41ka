// Copyright (c) 0cyn All Rights Reserved

#include "usb/PongoDevice.h"

#include <climits>
#include <cstring>

#include "usb/libusb.h"

namespace usb {
	namespace {
		constexpr uint8_t PongoClassOut = static_cast<uint8_t>(LIBUSB_ENDPOINT_OUT)
			| static_cast<uint8_t>(LIBUSB_REQUEST_TYPE_CLASS) | static_cast<uint8_t>(LIBUSB_RECIPIENT_INTERFACE);
		constexpr uint8_t PongoClassIn = static_cast<uint8_t>(LIBUSB_ENDPOINT_IN)
			| static_cast<uint8_t>(LIBUSB_REQUEST_TYPE_CLASS) | static_cast<uint8_t>(LIBUSB_RECIPIENT_INTERFACE);

		constexpr uint8_t RequestUpload = 1;
		constexpr uint8_t RequestReadStdout = 1;
		constexpr uint8_t RequestCommandInProgress = 2;
		constexpr uint8_t RequestWriteStdin = 3;
		constexpr uint8_t RequestIoMode = 4;

		constexpr uint16_t IoModeOutputBlocks = 1;
		constexpr uint16_t IoModeReset = 0xffff;

		uint16_t TransferChunkSize(size_t remaining)
		{
			return remaining < PongoDevice::BulkTransferChunkSize ?
				static_cast<uint16_t>(remaining) :
				PongoDevice::BulkTransferChunkSize;
		}
	}  // namespace

	PongoDevice::PongoDevice(libusb_context* context, libusb_device_handle* handle) : device_(context, handle) {}

	int PongoDevice::SendCommand(const char* command, unsigned int timeout_ms)
	{
		if (command == nullptr)
		{
			return LIBUSB_ERROR_INVALID_PARAM;
		}

		const size_t length = std::strlen(command);
		const bool has_newline = length != 0u && command[length - 1u] == '\n';
		const size_t sent_length = length + (has_newline ? 0u : 1u);
		if (sent_length == 0u || sent_length > MaxCommandLength)
		{
			return LIBUSB_ERROR_INVALID_PARAM;
		}

		int rc = SetOutputBlocking(true, timeout_ms);
		if (rc < 0)
		{
			return rc;
		}

		char buffer[MaxCommandLength] = {0};
		std::memcpy(buffer, command, length);
		if (!has_newline)
		{
			buffer[length] = '\n';
		}

		return WriteStdin(buffer, static_cast<uint16_t>(sent_length), timeout_ms);
	}

	int PongoDevice::WriteStdin(const void* data, uint16_t length, unsigned int timeout_ms)
	{
		if (length != 0u && data == nullptr)
		{
			return LIBUSB_ERROR_INVALID_PARAM;
		}

		if (length == 0u || length > MaxCommandLength)
		{
			return LIBUSB_ERROR_INVALID_PARAM;
		}

		return libusb_control_transfer(device_.handle_, PongoClassOut, RequestWriteStdin, 0, 0,
			const_cast<unsigned char*>(static_cast<const unsigned char*>(data)), length, timeout_ms);
	}

	int PongoDevice::ReadStdout(void* data, uint16_t length, unsigned int timeout_ms)
	{
		if (length != 0u && data == nullptr)
		{
			return LIBUSB_ERROR_INVALID_PARAM;
		}

		if (length != 512u && length != StdoutReadLength)
		{
			return LIBUSB_ERROR_INVALID_PARAM;
		}

		return libusb_control_transfer(device_.handle_, PongoClassIn, RequestReadStdout, 0, 0,
			static_cast<unsigned char*>(data), length, timeout_ms);
	}

	int PongoDevice::CommandInProgress(bool* in_progress, unsigned int timeout_ms)
	{
		if (in_progress == nullptr)
		{
			return LIBUSB_ERROR_INVALID_PARAM;
		}

		uint8_t value = 0;
		const int transferred = libusb_control_transfer(device_.handle_, PongoClassIn, RequestCommandInProgress, 0, 0,
			&value, static_cast<uint16_t>(sizeof(value)), timeout_ms);
		if (transferred < 0)
		{
			return transferred;
		}

		if (transferred != static_cast<int>(sizeof(value)))
		{
			return LIBUSB_ERROR_IO;
		}

		*in_progress = value != 0u;
		return LIBUSB_SUCCESS;
	}

	int PongoDevice::ResetIo(unsigned int timeout_ms)
	{
		return SetIoMode(IoModeReset, timeout_ms);
	}

	int PongoDevice::SetOutputBlocking(bool enabled, unsigned int timeout_ms)
	{
		return SetIoMode(enabled ? IoModeOutputBlocks : IoModeReset, timeout_ms);
	}

	int PongoDevice::SendBuffer(const void* data, size_t length, unsigned int timeout_ms)
	{
		int rc = BeginUpload(length, timeout_ms);
		if (rc != LIBUSB_SUCCESS)
		{
			return rc;
		}

		rc = SendUploadChunk(data, length, timeout_ms);
		if (rc < 0)
		{
			return rc;
		}

		if (rc != static_cast<int>(length))
		{
			return LIBUSB_ERROR_IO;
		}

		rc = FinishUpload(timeout_ms);
		if (rc != LIBUSB_SUCCESS)
		{
			return rc;
		}

		return static_cast<int>(length);
	}

	int PongoDevice::BeginUpload(size_t length, unsigned int timeout_ms)
	{
		if (upload_in_progress_)
		{
			return LIBUSB_ERROR_BUSY;
		}

		if (length == 0u)
		{
			return LIBUSB_ERROR_INVALID_PARAM;
		}

		if (length > MaxUploadLength || length > static_cast<size_t>(INT_MAX))
		{
			return LIBUSB_ERROR_OVERFLOW;
		}

		int rc = EnsureBulkConfigured(timeout_ms);
		if (rc != LIBUSB_SUCCESS)
		{
			return rc;
		}

		rc = ResizeUploadBuffer(static_cast<uint32_t>(length), timeout_ms);
		if (rc != LIBUSB_SUCCESS)
		{
			return rc;
		}

		upload_length_ = static_cast<uint32_t>(length);
		upload_bytes_transferred_ = 0;
		pending_upload_length_ = 0;
		upload_in_progress_ = true;
		return LIBUSB_SUCCESS;
	}

	int PongoDevice::SendUploadChunk(const void* data, size_t length, unsigned int timeout_ms)
	{
		if (!upload_in_progress_ || length == 0u || data == nullptr)
		{
			return LIBUSB_ERROR_INVALID_PARAM;
		}

		const size_t remaining = static_cast<size_t>(upload_length_ - upload_bytes_transferred_);
		if (length > remaining || length > static_cast<size_t>(INT_MAX))
		{
			return LIBUSB_ERROR_OVERFLOW;
		}

		const auto* input = static_cast<const uint8_t*>(data);
		size_t offset = 0;
		if (pending_upload_length_ != 0u)
		{
			const size_t needed = HostBulkPacketSize - pending_upload_length_;
			const size_t copied = length < needed ? length : needed;
			std::memcpy(pending_upload_ + pending_upload_length_, input, copied);
			pending_upload_length_ += static_cast<uint16_t>(copied);
			offset += copied;
			if (pending_upload_length_ == HostBulkPacketSize)
			{
				const int transferred = SendBulkChunk(pending_upload_, HostBulkPacketSize, timeout_ms);
				if (transferred != HostBulkPacketSize)
				{
					upload_in_progress_ = false;
					pending_upload_length_ = 0;
					return transferred < 0 ? transferred : LIBUSB_ERROR_IO;
				}
				pending_upload_length_ = 0;
			}
		}

		while (length - offset >= HostBulkPacketSize)
		{
			uint16_t chunk_size = TransferChunkSize(length - offset);
			chunk_size -= chunk_size % HostBulkPacketSize;
			const int transferred = SendBulkChunk(input + offset, chunk_size, timeout_ms);
			if (transferred < 0)
			{
				upload_in_progress_ = false;
				pending_upload_length_ = 0;
				return transferred;
			}

			if (transferred != chunk_size)
			{
				upload_in_progress_ = false;
				pending_upload_length_ = 0;
				return LIBUSB_ERROR_IO;
			}

			offset += chunk_size;
		}
		if (offset < length)
		{
			pending_upload_length_ = static_cast<uint16_t>(length - offset);
			std::memcpy(pending_upload_, input + offset, pending_upload_length_);
		}

		upload_bytes_transferred_ += static_cast<uint32_t>(length);
		return static_cast<int>(length);
	}

	int PongoDevice::FinishUpload(unsigned int timeout_ms)
	{
		if (!upload_in_progress_)
		{
			return LIBUSB_ERROR_INVALID_PARAM;
		}

		if (upload_bytes_transferred_ != upload_length_)
		{
			return LIBUSB_ERROR_BUSY;
		}

		if (pending_upload_length_ != 0u)
		{
			const int transferred = SendBulkChunk(pending_upload_, pending_upload_length_, timeout_ms);
			if (transferred != pending_upload_length_)
			{
				upload_in_progress_ = false;
				pending_upload_length_ = 0;
				return transferred < 0 ? transferred : LIBUSB_ERROR_IO;
			}
		}
		else if (upload_length_ % HostBulkPacketSize == 0u && upload_length_ % BulkMaxPacketSize != 0u)
		{
			// Pongo rounds its receive request to 512 bytes. A 64-byte-aligned
			// full-speed upload needs a ZLP to terminate that larger request.
			const int transferred = SendBulkChunk(nullptr, 0u, timeout_ms);
			if (transferred != 0)
			{
				upload_in_progress_ = false;
				return transferred < 0 ? transferred : LIBUSB_ERROR_IO;
			}
		}
		pending_upload_length_ = 0;
		upload_in_progress_ = false;
		return LIBUSB_SUCCESS;
	}

	int PongoDevice::EnsureBulkConfigured(unsigned int timeout_ms)
	{
		(void)timeout_ms;

		if (bulk_configured_)
		{
			return LIBUSB_SUCCESS;
		}

		const int rc = libusb_set_configuration(device_.handle_, DefaultConfiguration);
		if (rc != LIBUSB_SUCCESS)
		{
			return rc;
		}

		bulk_configured_ = true;
		return LIBUSB_SUCCESS;
	}

	int PongoDevice::ResizeUploadBuffer(uint32_t length, unsigned int timeout_ms)
	{
		const int transferred = libusb_control_transfer(device_.handle_, PongoClassOut, RequestUpload, 0, 0,
			reinterpret_cast<unsigned char*>(&length), static_cast<uint16_t>(sizeof(length)), timeout_ms);
		if (transferred < 0)
		{
			return transferred;
		}

		return transferred == static_cast<int>(sizeof(length)) ? LIBUSB_SUCCESS : LIBUSB_ERROR_IO;
	}

	int PongoDevice::SendBulkChunk(const void* data, uint16_t length, unsigned int timeout_ms)
	{
		if (length != 0u && data == nullptr)
		{
			return LIBUSB_ERROR_INVALID_PARAM;
		}

		int transferred = 0;
		const int rc = libusb_bulk_transfer(device_.handle_, BulkOutEndpoint,
			const_cast<unsigned char*>(static_cast<const unsigned char*>(data)), length, &transferred, timeout_ms);
		if (rc < 0)
		{
			return rc;
		}

		return transferred;
	}

	int PongoDevice::SetIoMode(uint16_t value, unsigned int timeout_ms)
	{
		const int transferred =
			libusb_control_transfer(device_.handle_, PongoClassOut, RequestIoMode, value, 0, nullptr, 0, timeout_ms);
		if (transferred < 0)
		{
			return transferred;
		}

		return transferred == 0 ? LIBUSB_SUCCESS : LIBUSB_ERROR_IO;
	}
}  // namespace usb
