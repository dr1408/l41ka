//
// Created by Skye on 7/3/26.
//

#ifndef L41KA_SETUPIBOOTPATCHER_H
#define L41KA_SETUPIBOOTPATCHER_H

#include <stddef.h>
#include <stdint.h>

namespace usb {
	class PwnedDFUDevice;
}

class iBootPatcherSetup
{
public:
	static int Run(usb::PwnedDFUDevice& device, uint32_t diag_mode = 0);
	static int RunUploaded(usb::PwnedDFUDevice& device, size_t payload_length);
	static uint64_t UploadedPayloadAddress(const usb::PwnedDFUDevice& device);

private:
	iBootPatcherSetup() = delete;
};


#endif  // L41KA_SETUPIBOOTPATCHER_H
