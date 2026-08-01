#ifndef L41KA_CONTROL_FAULT_H
#define L41KA_CONTROL_FAULT_H

#include <stdint.h>

namespace control {
	enum class FaultReason : uint32_t {
		TargetDisconnected = 1,
		TargetUsbFailure = 2,
		TargetShortTransfer = 3,
		TargetStateChanged = 4,
		ExploitFailed = 5,
		Bug = 6,
	};

	struct ErrorRecord {
		uint32_t present;
		uint32_t reason;
		uint32_t request_id;
		uint32_t opcode;
		uint32_t protocol_context;
		uint32_t observed_value;
		uint32_t panic_uptime_ms;
		uint32_t reserved;
	};

	ErrorRecord ConsumeErrorRecord();
	void ClearErrorRecord();
	[[noreturn]] void PanicTargetAt(const char* source_file, uint32_t source_line,
		FaultReason reason, uint32_t request_id, uint32_t opcode, uint32_t observed_value = 0);
}

#define PanicTarget(...) PanicTargetAt(__FILE__, __LINE__, __VA_ARGS__)

#endif
