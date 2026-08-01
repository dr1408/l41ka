//
// Created by Skye on 7/2/26.
//

#ifndef L41KA_DFUDEVICE_H
#define L41KA_DFUDEVICE_H

#include <stdint.h>

#include "usb/DeviceHandle.h"

namespace usb {
    class DFUDevice {
    public:
        enum class StatusCode : uint8_t {
            Ok = 0x00,
            ErrorTarget = 0x01,
            ErrorFile = 0x02,
            ErrorWrite = 0x03,
            ErrorErase = 0x04,
            ErrorCheckErased = 0x05,
            ErrorProgram = 0x06,
            ErrorVerify = 0x07,
            ErrorAddress = 0x08,
            ErrorNotDone = 0x09,
            ErrorFirmware = 0x0a,
            ErrorVendor = 0x0b,
            ErrorUsbReset = 0x0c,
            ErrorPowerOnReset = 0x0d,
            ErrorUnknown = 0x0e,
            ErrorStalledPacket = 0x0f,
        };

        enum class State : uint8_t {
            AppIdle = 0,
            AppDetach = 1,
            DfuIdle = 2,
            DfuDownloadSync = 3,
            DfuDownloadBusy = 4,
            DfuDownloadIdle = 5,
            DfuManifestSync = 6,
            DfuManifest = 7,
            DfuManifestWaitReset = 8,
            DfuUploadIdle = 9,
            DfuError = 10,
        };

        enum class RawTokenPid : uint8_t {
            Out = 0xe1,
            Setup = 0x2d,
        };

        enum class RawDataPid : uint8_t {
            Data0 = 0xc3,
            Data1 = 0x4b,
        };

        enum class RawHandshake : uint8_t {
            Timeout = 0x00,
            Ack = 0xd2,
            Nak = 0x5a,
            Stall = 0x1e,
        };

        struct Status {
            StatusCode code;
            uint32_t poll_timeout_ms;
            State state;
            uint8_t string_index;
        };

        static constexpr unsigned int DefaultTimeoutMs = 1000;
        static constexpr uint16_t DefaultDetachTimeoutMs = 1000;
        static constexpr uint16_t MaxSerialLength = 128;
        static constexpr uint8_t DefaultEp0MaxPacketSize = 64;
        static constexpr uint16_t RawEp0MaxPayloadLength = 64;
        static constexpr const char *PwnedSerialMarker = "L41kA:[+]";

        DFUDevice(const DFUDevice &other) = delete;
        DFUDevice &operator=(const DFUDevice &other) = delete;
        DFUDevice(DFUDevice &&other) noexcept = default;
        DFUDevice &operator=(DFUDevice &&other) noexcept = default;

        [[nodiscard]] uint16_t InterfaceNumber() const;
        void SetInterfaceNumber(uint16_t interface_number);
        [[nodiscard]] uint64_t CPID() const;
        [[nodiscard]] bool IsPwned() const;
        [[nodiscard]] bool IdentityValid() const;

        int Serial(char *serial, uint16_t length, unsigned int timeout_ms = DefaultTimeoutMs);
        int StringDescriptor(uint8_t index, char *value, uint16_t length,
            unsigned int timeout_ms = DefaultTimeoutMs);
        int Detach(uint16_t detach_timeout_ms = DefaultDetachTimeoutMs, unsigned int timeout_ms = DefaultTimeoutMs);
        int Download(uint16_t block_number, const void *data, uint16_t length, unsigned int timeout_ms = DefaultTimeoutMs);
        int Upload(uint16_t block_number, void *data, uint16_t length, unsigned int timeout_ms = DefaultTimeoutMs);
        int GetStatus(Status *status, unsigned int timeout_ms = DefaultTimeoutMs);
        int ClearStatus(unsigned int timeout_ms = DefaultTimeoutMs);
        int GetState(State *state, unsigned int timeout_ms = DefaultTimeoutMs);
        int Abort(unsigned int timeout_ms = DefaultTimeoutMs);
        int ResetEp0(uint8_t max_packet_size = DefaultEp0MaxPacketSize);
        int RawEp0Transfer(
            RawTokenPid token_pid,
            const void *encoded_packet,
            uint16_t encoded_length,
            RawHandshake *handshake
        );
        int RawEp0DataTransfer(
            RawTokenPid token_pid,
            RawDataPid data_pid,
            const void *payload,
            uint16_t payload_length,
            RawHandshake *handshake
        );

    private:
        friend class Device;

        DFUDevice(libusb_context *context, libusb_device_handle *handle);

        DeviceHandle device_;
        uint16_t interface_number_ = 0;
        uint64_t cpid_ = 0;
        bool pwnd_ = false;
        bool identity_valid_ = false;
    };
} // usb

#endif //L41KA_DFUDEVICE_H
