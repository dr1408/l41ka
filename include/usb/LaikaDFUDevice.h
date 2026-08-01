#ifndef L41KA_LAIKADFUDEVICE_H
#define L41KA_LAIKADFUDEVICE_H

#include <stddef.h>
#include <stdint.h>

#include "usb/DeviceHandle.h"

namespace usb {
    class LaikaDFUDevice {
    public:
        static constexpr unsigned int DefaultTimeoutMs = 10000;
        static constexpr size_t MaxPayloadLength = 0x200000;
        static constexpr uint8_t DefaultConfiguration = 1;
        static constexpr uint8_t BulkOutEndpoint = 0x02;
        static constexpr uint16_t BulkTransferPacketSize = 64;
        static constexpr uint16_t BulkTransferChunkSize = BulkTransferPacketSize;
        static constexpr uint16_t TransferHeaderSize = 16;

        LaikaDFUDevice(const LaikaDFUDevice &other) = delete;
        LaikaDFUDevice &operator=(const LaikaDFUDevice &other) = delete;
        LaikaDFUDevice(LaikaDFUDevice &&other) noexcept = default;
        LaikaDFUDevice &operator=(LaikaDFUDevice &&other) noexcept = default;

        int BeginUpload(size_t length, unsigned int timeout_ms = DefaultTimeoutMs);
        int SendUploadChunk(const void *data, size_t length, unsigned int timeout_ms = DefaultTimeoutMs);
        int FinishUpload(unsigned int timeout_ms = DefaultTimeoutMs);
        bool UploadInProgress() const { return upload_in_progress_; }
        uint32_t UploadLength() const { return upload_length_; }
        uint32_t UploadBytesTransferred() const { return upload_bytes_transferred_; }
        int SendBuffer(const void *data, size_t length, unsigned int timeout_ms = DefaultTimeoutMs);

    private:
        friend class Device;

        LaikaDFUDevice(libusb_context *context, libusb_device_handle *handle);

        DeviceHandle device_;
        bool bulk_configured_ = false;
        bool upload_in_progress_ = false;
        uint32_t upload_length_ = 0;
        uint32_t upload_bytes_transferred_ = 0;
        uint8_t final_packet_[BulkTransferPacketSize] = {0};
        uint16_t final_packet_length_ = 0;

        int EnsureBulkConfigured();
        int SendBulkChunk(const void *data, uint16_t length, unsigned int timeout_ms);
    };
} // usb

#endif // L41KA_LAIKADFUDEVICE_H
