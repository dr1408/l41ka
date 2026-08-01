//
// Created by Skye on 7/2/26.
//

#ifndef L41KA_DEVICEHANDLE_H
#define L41KA_DEVICEHANDLE_H

typedef struct libusb_context libusb_context;
typedef struct libusb_device_handle libusb_device_handle;

namespace usb {
    class DFUDevice;
    class LaikaDFUDevice;
    class PongoDevice;
    class PwnedDFUDevice;
    class RecoveryDevice;

    class DeviceHandle {
    public:
        ~DeviceHandle();

        DeviceHandle(const DeviceHandle &other) = delete;
        DeviceHandle &operator=(const DeviceHandle &other) = delete;
        DeviceHandle(DeviceHandle &&other) noexcept;
        DeviceHandle &operator=(DeviceHandle &&other) noexcept;

        bool IsOpen() const;

    private:
        friend class DFUDevice;
        friend class LaikaDFUDevice;
        friend class PongoDevice;
        friend class PwnedDFUDevice;
        friend class RecoveryDevice;

        DeviceHandle(libusb_context *context, libusb_device_handle *handle);

        void Close();

        libusb_context *context_;
        libusb_device_handle *handle_;
    };
} // usb

#endif //L41KA_DEVICEHANDLE_H
