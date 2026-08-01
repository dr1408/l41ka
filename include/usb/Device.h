//
// Created by Skye on 7/2/26.
//

#ifndef L41KA_DEVICE_H
#define L41KA_DEVICE_H

#include <cstdint>
#include <variant>

#include "usb/DFUDevice.h"
#include "usb/LaikaDFUDevice.h"
#include "usb/PongoDevice.h"
#include "usb/PwnedDFUDevice.h"
#include "usb/RecoveryDevice.h"

typedef struct libusb_context libusb_context;
typedef struct libusb_device_handle libusb_device_handle;

namespace usb {
    class Device {
    public:
        enum class Mode : uint8_t {
            None,
            DFU,
            Recovery,
            Pongo,
            LaikaDFU,
        };

        Device() = default;
        ~Device();

        Device(const Device &other) = delete;
        Device &operator=(const Device &other) = delete;
        Device(Device &&other) noexcept;
        Device &operator=(Device &&other) noexcept;

        static int Open(Device *device);
        static bool IsConnected();

        bool IsOpen() const;
        void Close();
        bool IsDFU() const;
        bool IsPwnedDFU() const;
        bool IsRecovery() const;
        bool IsPongo() const;
        bool IsLaikaDFU() const;
        Mode GetMode() const;

        DFUDevice *AsDFU();
        const DFUDevice *AsDFU() const;
        PwnedDFUDevice *AsPwnedDFU();
        const PwnedDFUDevice *AsPwnedDFU() const;
        RecoveryDevice *AsRecovery();
        const RecoveryDevice *AsRecovery() const;
        PongoDevice *AsPongo();
        const PongoDevice *AsPongo() const;
        LaikaDFUDevice *AsLaikaDFU();
        const LaikaDFUDevice *AsLaikaDFU() const;

    private:
        using Storage = std::variant<std::monostate, DFUDevice, PwnedDFUDevice, RecoveryDevice, PongoDevice, LaikaDFUDevice>;

        Storage storage_;
    };
} // usb

#endif //L41KA_DEVICE_H
