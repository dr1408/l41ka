// Copyright (c) 0cyn All Rights Reserved
#include "control/Log.h"

#include <cstdarg>
#include <cstdio>

#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "pico/util/queue.h"
#include "tusb.h"

namespace logging {
	namespace {
		constexpr size_t MessageLength = 160;
		constexpr size_t LineLength = 240;
		struct Record {
			uint32_t uptime_ms;
			uint32_t source_line;
			const char* source_file;
			uint8_t core;
			Level level;
			char message[MessageLength];
		};

		queue_t Queue;
		bool Initialized;

		const char* LevelName(Level level)
		{
			switch (level)
			{
			case Level::Info: return "info";
			case Level::Warn: return "warn";
			case Level::Fatal: return "fatal";
			}
			return "unknown";
		}

		const char* SourceName(const char* path)
		{
			const char* name = path != nullptr ? path : "unknown";
			for (const char* cursor = name; *cursor != '\0'; ++cursor)
			{
				if (*cursor == '/' || *cursor == '\\')
					name = cursor + 1;
			}
			return name;
		}
	}

	void Init()
	{
		queue_init(&Queue, sizeof(Record), 64);
		Initialized = true;
	}

	void WriteAt(Level level, const char* source_file, uint32_t source_line, const char* format, ...)
	{
		if (!Initialized || format == nullptr)
			return;
		Record record {};
		record.uptime_ms = to_ms_since_boot(get_absolute_time());
		record.source_line = source_line;
		record.source_file = source_file;
		record.core = static_cast<uint8_t>(get_core_num());
		record.level = level;
		va_list arguments;
		va_start(arguments, format);
		std::vsnprintf(record.message, sizeof(record.message), format, arguments);
		va_end(arguments);
		(void)queue_try_add(&Queue, &record);
	}

	void Drain()
	{
		if (!Initialized)
			return;
		Record record {};
		while (queue_try_remove(&Queue, &record))
		{
			if (!tud_cdc_connected())
				continue;
			char line[LineLength];
			const int length = std::snprintf(line, sizeof(line), "[%08lu] c%u %-5s %s:%lu %s\r\n",
				static_cast<unsigned long>(record.uptime_ms), record.core, LevelName(record.level),
				SourceName(record.source_file), static_cast<unsigned long>(record.source_line), record.message);
			if (length <= 0 || static_cast<size_t>(length) >= sizeof(line)
				|| tud_cdc_write_available() < static_cast<uint32_t>(length))
				continue;
			tud_cdc_write(line, static_cast<uint32_t>(length));
			tud_cdc_write_flush();
		}
	}
}
