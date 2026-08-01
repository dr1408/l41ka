//
// Created by Skye on 7/7/26.
//

#ifndef L41KA_PONGODEVICE_H
#define L41KA_PONGODEVICE_H

#include <stddef.h>
#include <stdint.h>

#include "usb/DeviceHandle.h"

namespace usb {
    class PongoDevice {
    public:
        static constexpr unsigned int DefaultTimeoutMs = 10000;
        static constexpr uint16_t MaxCommandLength = 512;
        static constexpr uint32_t MaxUploadLength = 1024u * 1024u * 128u;
        static constexpr uint8_t DefaultConfiguration = 1;
        static constexpr uint8_t BulkOutEndpoint = 0x02;
        static constexpr uint16_t BulkMaxPacketSize = 512;
        static constexpr uint16_t HostBulkPacketSize = 64;
        static constexpr uint16_t BulkTransferChunkSize = 0x8000;
        static constexpr uint16_t StdoutReadLength = 0x1000;

        PongoDevice(const PongoDevice &other) = delete;
        PongoDevice &operator=(const PongoDevice &other) = delete;
        PongoDevice(PongoDevice &&other) noexcept = default;
        PongoDevice &operator=(PongoDevice &&other) noexcept = default;

        int SendCommand(const char *command, unsigned int timeout_ms = DefaultTimeoutMs);
        int WriteStdin(const void *data, uint16_t length, unsigned int timeout_ms = DefaultTimeoutMs);
        int ReadStdout(void *data, uint16_t length = StdoutReadLength, unsigned int timeout_ms = DefaultTimeoutMs);
        int CommandInProgress(bool *in_progress, unsigned int timeout_ms = DefaultTimeoutMs);
        int ResetIo(unsigned int timeout_ms = DefaultTimeoutMs);
        int SetOutputBlocking(bool enabled, unsigned int timeout_ms = DefaultTimeoutMs);
        int BeginUpload(size_t length, unsigned int timeout_ms = DefaultTimeoutMs);
        int SendUploadChunk(const void *data, size_t length, unsigned int timeout_ms = DefaultTimeoutMs);
        int FinishUpload(unsigned int timeout_ms = DefaultTimeoutMs);
        bool UploadInProgress() const { return upload_in_progress_; }
        uint32_t UploadLength() const { return upload_length_; }
        uint32_t UploadBytesTransferred() const { return upload_bytes_transferred_; }
        int SendBuffer(const void *data, size_t length, unsigned int timeout_ms = DefaultTimeoutMs);

    private:
        friend class Device;

        PongoDevice(libusb_context *context, libusb_device_handle *handle);

        DeviceHandle device_;
        bool bulk_configured_ = false;
        bool upload_in_progress_ = false;
        uint32_t upload_length_ = 0;
        uint32_t upload_bytes_transferred_ = 0;
        uint8_t pending_upload_[HostBulkPacketSize] = {0};
        uint16_t pending_upload_length_ = 0;

        int EnsureBulkConfigured(unsigned int timeout_ms);
        int ResizeUploadBuffer(uint32_t length, unsigned int timeout_ms);
        int SendBulkChunk(const void *data, uint16_t length, unsigned int timeout_ms);
        int SetIoMode(uint16_t value, unsigned int timeout_ms);
    };
} // usb

#endif //L41KA_PONGODEVICE_H
