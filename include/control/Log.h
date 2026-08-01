#ifndef L41KA_DEBUG_LOG_H
#define L41KA_DEBUG_LOG_H

#include <stdint.h>

namespace logging {
	enum class Level : unsigned char { Info, Warn, Fatal };

	void Init();
	void WriteAt(Level level, const char* source_file, uint32_t source_line, const char* format, ...);
	void Drain();
}

#define L41KA_LOG(level, ...) ::logging::WriteAt((level), __FILE__, __LINE__, __VA_ARGS__)

#endif
