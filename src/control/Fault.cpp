//
// Copyright (c) 0cyn All Rights Reserved.
//
// we get 8 uint32 nv regs whenever we panic
// we put some barebones info in them before panic to make debugging possible
//
// probably you can scream over the debug channel here if you need, we're not so low level here that
//	anything has been torn down.
//
#include "control/Fault.h"

#include "../../include/control/Log.h"
#include "hardware/structs/watchdog.h"
#include "hardware/watchdog.h"
#include "pico/stdlib.h"

namespace control {
	namespace {
		constexpr uint32_t BreadcrumbMagic = 0x4c455252; // "LERR"
		constexpr uint32_t BreadcrumbVersion = 1;
	}

	ErrorRecord ConsumeErrorRecord()
	{
		ErrorRecord record {};
		if (!watchdog_caused_reboot() || watchdog_hw->scratch[0] != BreadcrumbMagic
			|| watchdog_hw->scratch[1] != BreadcrumbVersion)
			return record;

		record.present = 1;
		record.reason = watchdog_hw->scratch[2];
		record.request_id = watchdog_hw->scratch[3];
		record.opcode = watchdog_hw->scratch[4];
		record.protocol_context = watchdog_hw->scratch[5];
		record.observed_value = watchdog_hw->scratch[6];
		record.panic_uptime_ms = watchdog_hw->scratch[7];
		watchdog_hw->scratch[0] = 0;
		return record;
	}

	void ClearErrorRecord()
	{
		for (auto& scratch : watchdog_hw->scratch)
			scratch = 0;
	}

	[[noreturn]] void PanicTargetAt(const char* source_file, uint32_t source_line,
		FaultReason reason, uint32_t request_id, uint32_t opcode, uint32_t observed_value)
	{
		logging::WriteAt(logging::Level::Fatal, source_file, source_line,
			"reason=%lu request=%lu opcode=%08lx value=%08lx",
			static_cast<unsigned long>(reason), static_cast<unsigned long>(request_id),
			static_cast<unsigned long>(opcode), static_cast<unsigned long>(observed_value));

		watchdog_enable(100, false);
		watchdog_hw->scratch[0] = BreadcrumbMagic;
		watchdog_hw->scratch[1] = BreadcrumbVersion;
		watchdog_hw->scratch[2] = static_cast<uint32_t>(reason);
		watchdog_hw->scratch[3] = request_id;
		watchdog_hw->scratch[4] = opcode;
		watchdog_hw->scratch[5] = 0;
		watchdog_hw->scratch[6] = observed_value;
		watchdog_hw->scratch[7] = to_ms_since_boot(get_absolute_time());
		panic("fatal target operation failure");
		while (true)
			tight_loop_contents();
	}
}
