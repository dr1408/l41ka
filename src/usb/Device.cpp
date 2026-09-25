// Copyright (c) 0cyn All Rights Reserved

#include "usb/Device.h"

#include <utility>

#include "control/Log.h"
#include "usb/libusb.h"
#include "usb/pico_libusb.h"

namespace usb {
	namespace {
		constexpr uint16_t AppleVendorId = 0x05ac;
		constexpr uint16_t DFUProductId = 0x1227;
		constexpr uint16_t RecoveryProductId = 0x1281;
		constexpr uint16_t PongoProductId = 0x4141;
		constexpr uint16_t LaikaDFUProductId = 0x4c41;

		libusb_context* context = nullptr;
		bool initialized = false;
		bool device_open = false;
		uint16_t last_unexpected_vendor_id = 0xffff;
		uint16_t last_unexpected_product_id = 0xffff;

		int EnsureInitialized()
		{
			if (initialized)
			{
				return LIBUSB_SUCCESS;
			}

			const int rc = libusb_init(&context);
			if (rc != LIBUSB_SUCCESS)
			{
				return rc;
			}

			initialized = true;
			return LIBUSB_SUCCESS;
		}

		void ResetUnexpectedDeviceLog()
		{
			last_unexpected_vendor_id = 0xffff;
			last_unexpected_product_id = 0xffff;
		}
	}  // namespace

	DeviceHandle::DeviceHandle(libusb_context* context, libusb_device_handle* handle) :
		context_(context), handle_(handle)
	{
		device_open = handle != nullptr;
	}

	DeviceHandle::~DeviceHandle()
	{
		Close();
	}

	DeviceHandle::DeviceHandle(DeviceHandle&& other) noexcept : context_(other.context_), handle_(other.handle_)
	{
		other.context_ = nullptr;
		other.handle_ = nullptr;
	}

	DeviceHandle& DeviceHandle::operator=(DeviceHandle&& other) noexcept
	{
		if (this != &other)
		{
			Close();
			context_ = other.context_;
			handle_ = other.handle_;
			other.context_ = nullptr;
			other.handle_ = nullptr;
		}

		return *this;
	}

	bool DeviceHandle::IsOpen() const
	{
		return handle_ != nullptr;
	}

	void DeviceHandle::Close()
	{
		if (handle_ != nullptr)
		{
			libusb_close(handle_);
			handle_ = nullptr;
			device_open = false;
		}

		context_ = nullptr;
	}

	Device::~Device() = default;
	Device::Device(Device&& other) noexcept = default;
	Device& Device::operator=(Device&& other) noexcept = default;

	int Device::Open(Device* device)
	{
		if (device == nullptr)
		{
			return LIBUSB_ERROR_INVALID_PARAM;
		}

		if (device->IsOpen())
		{
			device->storage_ = std::monostate {};
		}
		else if (device_open)
		{
			return LIBUSB_ERROR_BUSY;
		}

		const int rc = EnsureInitialized();
		if (rc != LIBUSB_SUCCESS)
		{
			return rc;
		}

		uint16_t vendor_id = 0;
		uint16_t product_id = 0;
		libusb_device_handle* handle = nullptr;
		const int open_rc = pico_libusb_open_device(context, &handle, &vendor_id, &product_id);
		if (open_rc != LIBUSB_SUCCESS)
		{
			return open_rc;
		}
		if (vendor_id != AppleVendorId
			|| (product_id != DFUProductId && product_id != RecoveryProductId && product_id != PongoProductId
				&& product_id != LaikaDFUProductId))
		{
			if (vendor_id != last_unexpected_vendor_id || product_id != last_unexpected_product_id)
			{
				L41KA_LOG(logging::Level::Warn, "target unexpected vid=%04x pid=%04x",
					static_cast<unsigned int>(vendor_id), static_cast<unsigned int>(product_id));
				last_unexpected_vendor_id = vendor_id;
				last_unexpected_product_id = product_id;
			}
			libusb_close(handle);
			return LIBUSB_ERROR_NOT_FOUND;
		}

		if (product_id == DFUProductId)
		{
			ResetUnexpectedDeviceLog();
			DFUDevice dfu(context, handle);
			if (dfu.IsPwned())
			{
				device->storage_ = PwnedDFUDevice(std::move(dfu));
			}
			else
			{
				device->storage_ = std::move(dfu);
			}
			return LIBUSB_SUCCESS;
		}

		if (product_id == RecoveryProductId)
		{
			ResetUnexpectedDeviceLog();
			const int configuration_rc = libusb_set_configuration(handle, 1);
			if (configuration_rc != LIBUSB_SUCCESS)
			{
				libusb_close(handle);
				return configuration_rc;
			}
			device->storage_ = RecoveryDevice(context, handle);
			return LIBUSB_SUCCESS;
		}

		if (product_id == PongoProductId)
		{
			ResetUnexpectedDeviceLog();
			device->storage_ = PongoDevice(context, handle);
			return LIBUSB_SUCCESS;
		}

		if (product_id == LaikaDFUProductId)
		{
			ResetUnexpectedDeviceLog();
			device->storage_ = LaikaDFUDevice(context, handle);
			return LIBUSB_SUCCESS;
		}

		libusb_close(handle);
		return LIBUSB_ERROR_NOT_SUPPORTED;
	}

	bool Device::IsConnected()
	{
		return EnsureInitialized() == LIBUSB_SUCCESS && pico_libusb_device_connected(context);
	}

	bool Device::IsOpen() const
	{
		return !std::holds_alternative<std::monostate>(storage_);
	}

	void Device::Close()
	{
		storage_ = std::monostate {};
	}

	bool Device::IsDFU() const
	{
		return std::holds_alternative<DFUDevice>(storage_) || std::holds_alternative<PwnedDFUDevice>(storage_);
	}

	bool Device::IsPwnedDFU() const
	{
		return std::holds_alternative<PwnedDFUDevice>(storage_);
	}

	bool Device::IsRecovery() const
	{
		return std::holds_alternative<RecoveryDevice>(storage_);
	}

	bool Device::IsPongo() const
	{
		return std::holds_alternative<PongoDevice>(storage_);
	}

	bool Device::IsLaikaDFU() const
	{
		return std::holds_alternative<LaikaDFUDevice>(storage_);
	}

	Device::Mode Device::GetMode() const
	{
		if (IsDFU())
		{
			return Mode::DFU;
		}

		if (IsRecovery())
		{
			return Mode::Recovery;
		}

		if (IsPongo())
		{
			return Mode::Pongo;
		}

		if (IsLaikaDFU())
		{
			return Mode::LaikaDFU;
		}

		return Mode::None;
	}

	DFUDevice* Device::AsDFU()
	{
		if (DFUDevice* dfu = std::get_if<DFUDevice>(&storage_))
		{
			return dfu;
		}

		if (PwnedDFUDevice* dfu = std::get_if<PwnedDFUDevice>(&storage_))
		{
			return dfu;
		}

		return nullptr;
	}

	const DFUDevice* Device::AsDFU() const
	{
		if (const DFUDevice* dfu = std::get_if<DFUDevice>(&storage_))
		{
			return dfu;
		}

		if (const PwnedDFUDevice* dfu = std::get_if<PwnedDFUDevice>(&storage_))
		{
			return dfu;
		}

		return nullptr;
	}

	PwnedDFUDevice* Device::AsPwnedDFU()
	{
		return std::get_if<PwnedDFUDevice>(&storage_);
	}

	const PwnedDFUDevice* Device::AsPwnedDFU() const
	{
		return std::get_if<PwnedDFUDevice>(&storage_);
	}

	RecoveryDevice* Device::AsRecovery()
	{
		return std::get_if<RecoveryDevice>(&storage_);
	}

	const RecoveryDevice* Device::AsRecovery() const
	{
		return std::get_if<RecoveryDevice>(&storage_);
	}

	PongoDevice* Device::AsPongo()
	{
		return std::get_if<PongoDevice>(&storage_);
	}

	const PongoDevice* Device::AsPongo() const
	{
		return std::get_if<PongoDevice>(&storage_);
	}

	LaikaDFUDevice* Device::AsLaikaDFU()
	{
		return std::get_if<LaikaDFUDevice>(&storage_);
	}

	const LaikaDFUDevice* Device::AsLaikaDFU() const
	{
		return std::get_if<LaikaDFUDevice>(&storage_);
	}
}  // namespace usb
