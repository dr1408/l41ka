// Copyright (c) 0cyn All Rights Reserved
#include "usb/LaikaDFUDevice.h"

#include <climits>
#include <cstring>

#include "usb/libusb.h"

namespace usb {
	namespace {
		uint16_t TransferChunkSize(size_t remaining)
		{
			return remaining < LaikaDFUDevice::BulkTransferChunkSize ?
				static_cast<uint16_t>(remaining) :
				LaikaDFUDevice::BulkTransferChunkSize;
		}
	}  // namespace

	LaikaDFUDevice::LaikaDFUDevice(libusb_context* context, libusb_device_handle* handle) : device_(context, handle) {}

	int LaikaDFUDevice::SendBuffer(const void* data, size_t length, unsigned int timeout_ms)
	{
		if (data == nullptr)
		{
			return LIBUSB_ERROR_INVALID_PARAM;
		}

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

	int LaikaDFUDevice::BeginUpload(size_t length, unsigned int timeout_ms)
	{
		if (upload_in_progress_)
		{
			return LIBUSB_ERROR_BUSY;
		}
		if (length == 0u)
		{
			return LIBUSB_ERROR_INVALID_PARAM;
		}
		if (length > MaxPayloadLength || length > static_cast<size_t>(INT_MAX))
		{
			return LIBUSB_ERROR_OVERFLOW;
		}

		int rc = EnsureBulkConfigured();
		if (rc != LIBUSB_SUCCESS)
		{
			return rc;
		}

		uint8_t header[TransferHeaderSize] = {
			'L',
			'A',
			'I',
			'K',
			'A',
			'P',
			'N',
			'G',
		};
		const uint32_t payload_length = static_cast<uint32_t>(length);
		header[8] = static_cast<uint8_t>(payload_length);
		header[9] = static_cast<uint8_t>(payload_length >> 8u);
		header[10] = static_cast<uint8_t>(payload_length >> 16u);
		header[11] = static_cast<uint8_t>(payload_length >> 24u);
		const int header_transferred = SendBulkChunk(header, sizeof(header), timeout_ms);
		if (header_transferred < 0)
		{
			return header_transferred;
		}

		if (header_transferred != static_cast<int>(sizeof(header)))
		{
			return LIBUSB_ERROR_IO;
		}

		upload_length_ = payload_length;
		upload_bytes_transferred_ = 0;
		final_packet_length_ = 0;
		upload_in_progress_ = true;
		return LIBUSB_SUCCESS;
	}

	int LaikaDFUDevice::SendUploadChunk(const void* data, size_t length, unsigned int timeout_ms)
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
		size_t send_length = length;
		if (length == remaining)
		{
			final_packet_length_ = static_cast<uint16_t>(
				length < BulkTransferPacketSize ? length : BulkTransferPacketSize);
			send_length -= final_packet_length_;
			std::memcpy(final_packet_, input + send_length, final_packet_length_);
		}

		size_t offset = 0;
		while (offset < send_length)
		{
			const uint16_t chunk_size = TransferChunkSize(send_length - offset);
			const int transferred = SendBulkChunk(input + offset, chunk_size, timeout_ms);
			if (transferred < 0)
			{
				upload_in_progress_ = false;
				return transferred;
			}

			if (transferred != chunk_size)
			{
				upload_in_progress_ = false;
				return LIBUSB_ERROR_IO;
			}

			offset += chunk_size;
		}

		upload_bytes_transferred_ += static_cast<uint32_t>(length);
		return static_cast<int>(length);
	}

	int LaikaDFUDevice::FinishUpload(unsigned int timeout_ms)
	{
		if (!upload_in_progress_)
		{
			return LIBUSB_ERROR_INVALID_PARAM;
		}
		if (upload_bytes_transferred_ != upload_length_ || final_packet_length_ == 0u)
		{
			return LIBUSB_ERROR_BUSY;
		}

		const int transferred = SendBulkChunk(final_packet_, final_packet_length_, timeout_ms);
		upload_in_progress_ = false;
		if (transferred < 0)
		{
			return transferred;
		}
		if (transferred != final_packet_length_)
		{
			return LIBUSB_ERROR_IO;
		}

		final_packet_length_ = 0;
		return LIBUSB_SUCCESS;
	}

	int LaikaDFUDevice::EnsureBulkConfigured()
	{
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

	int LaikaDFUDevice::SendBulkChunk(const void* data, uint16_t length, unsigned int timeout_ms)
	{
		int transferred = 0;
		const int rc = libusb_bulk_transfer(device_.handle_, BulkOutEndpoint,
			const_cast<unsigned char*>(static_cast<const unsigned char*>(data)), length, &transferred, timeout_ms);
		if (rc < 0)
		{
			return rc;
		}

		return transferred;
	}
}  // namespace usb
