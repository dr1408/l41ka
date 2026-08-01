#ifndef L41KA_CONTROL_APPLE_DEVICE_DATABASE_H
#define L41KA_CONTROL_APPLE_DEVICE_DATABASE_H

#include <stddef.h>
#include <stdint.h>

namespace control {
	struct AppleDevice {
		const char* product_type;
		const char* hardware_model;
		const char* display_name;
	};

	const AppleDevice* FindAppleDevice(uint32_t chip_id, uint32_t board_id);
	size_t AppleDeviceCount();
}

#endif
