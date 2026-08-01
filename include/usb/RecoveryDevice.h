//
// Created by Skye on 7/2/26.
//

#ifndef L41KA_RECOVERYDEVICE_H
#define L41KA_RECOVERYDEVICE_H

#include <stdint.h>

#include "usb/DeviceHandle.h"

namespace usb {
    class RecoveryDevice {
    public:
        static constexpr unsigned int DefaultTimeoutMs = 10000;
        static constexpr uint16_t MaxSerialLength = 128;
        RecoveryDevice(const RecoveryDevice &other) = delete;
        RecoveryDevice &operator=(const RecoveryDevice &other) = delete;
        RecoveryDevice(RecoveryDevice &&other) noexcept = default;
        RecoveryDevice &operator=(RecoveryDevice &&other) noexcept = default;

        int Reboot(unsigned int timeout_ms = DefaultTimeoutMs);
        int Serial(char *serial, uint16_t length, unsigned int timeout_ms = DefaultTimeoutMs);
        int StringDescriptor(uint8_t index, char *value, uint16_t length,
            unsigned int timeout_ms = DefaultTimeoutMs);

    private:
        friend class Device;

        RecoveryDevice(libusb_context *context, libusb_device_handle *handle);

        DeviceHandle device_;
    };
} // usb

#endif //L41KA_RECOVERYDEVICE_H
