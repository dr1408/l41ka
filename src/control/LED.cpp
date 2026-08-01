// Copyright (c) 0cyn All Rights Reserved
#include "../../include/control/LED.h"

#include <cstdint>

#include "hardware/sync.h"
#include "pico/time.h"

#if LED_NEOPIXEL
	#include "pico/status_led.h"
#elif LED_RGB_PWM
	#include "hardware/gpio.h"
	#include "hardware/pwm.h"
#elif LED_SINGLE_COLOR
	#include "hardware/gpio.h"
	#include "hardware/pwm.h"
#endif

namespace led {
	namespace {

		constexpr uint32_t DefaultPeriodMs = 1000;
		constexpr uint32_t FrameIntervalMs = 20;

#if LED_RGB_PWM || LED_SINGLE_COLOR
		constexpr uint16_t PwmWrap = 255;
#endif

		struct Context
		{
			Pattern pattern = Pattern::Off();
			repeating_timer_t timer = {};
			uint32_t started_ms = 0;
			bool initialized = false;
			bool timer_active = false;
		};

		Context g_ctx;

		uint32_t NowMs()
		{
			return to_ms_since_boot(get_absolute_time());
		}

		uint32_t PeriodOrDefault(uint32_t period_ms)
		{
			return period_ms == 0 ? DefaultPeriodMs : period_ms;
		}

		uint32_t ClampU32(uint32_t value, uint32_t min, uint32_t max)
		{
			if (max < min)
			{
				return max;
			}

			if (value < min)
			{
				return min;
			}

			if (value > max)
			{
				return max;
			}

			return value;
		}

		uint8_t MaxU8(uint8_t a, uint8_t b)
		{
			return a > b ? a : b;
		}

		uint8_t Scale8(uint8_t value, uint8_t scale)
		{
			return static_cast<uint8_t>((static_cast<uint16_t>(value) * scale + 127u) / 255u);
		}

		Color Scale(Color color, uint8_t scale)
		{
			return {
				Scale8(color.r, scale),
				Scale8(color.g, scale),
				Scale8(color.b, scale),
			};
		}

		Color Blend(Color from, Color to, uint8_t amount_to)
		{
			const uint16_t amount_from = 255u - amount_to;

			return {
				static_cast<uint8_t>((from.r * amount_from + to.r * amount_to + 127u) / 255u),
				static_cast<uint8_t>((from.g * amount_from + to.g * amount_to + 127u) / 255u),
				static_cast<uint8_t>((from.b * amount_from + to.b * amount_to + 127u) / 255u),
			};
		}

		uint8_t TriangleWave(uint32_t elapsed_ms, uint32_t period_ms)
		{
			const uint32_t period = PeriodOrDefault(period_ms);
			const uint32_t phase = elapsed_ms % period;
			const uint32_t rising_ms = period / 2u;
			const uint32_t falling_ms = period - rising_ms;

			if (rising_ms == 0 || falling_ms == 0)
			{
				return 255;
			}

			if (phase < rising_ms)
			{
				return static_cast<uint8_t>((phase * 255u) / rising_ms);
			}

			return static_cast<uint8_t>(((period - phase) * 255u) / falling_ms);
		}

		uint8_t EaseInOut(uint8_t value)
		{
			const uint32_t t = value;
			return static_cast<uint8_t>((t * t * (765u - 2u * t) + 32512u) / 65025u);
		}

		uint32_t Hash32(uint32_t value)
		{
			value ^= value >> 16u;
			value *= 0x7feb352du;
			value ^= value >> 15u;
			value *= 0x846ca68bu;
			value ^= value >> 16u;
			return value;
		}

		uint8_t PulseAt(uint32_t phase, uint32_t start, uint32_t duration, uint8_t peak)
		{
			if (phase < start || phase >= start + duration || duration == 0)
			{
				return 0;
			}

			const uint32_t local = phase - start;
			const uint32_t rise = duration / 4u == 0 ? 1u : duration / 4u;

			if (local < rise)
			{
				return static_cast<uint8_t>((local * peak) / rise);
			}

			const uint32_t fall = duration - rise == 0 ? 1u : duration - rise;
			return static_cast<uint8_t>(((duration - local) * peak) / fall);
		}

		bool IsAnimated(Mode mode)
		{
			return mode != Mode::Off && mode != Mode::Solid;
		}

		Pattern Normalize(Pattern pattern)
		{
			if (pattern.mode == Mode::Off)
			{
				pattern.primary = colors::Off;
				pattern.secondary = colors::Off;
				pattern.brightness = 0;
				pattern.period_ms = 0;
				return pattern;
			}

			if (pattern.mode == Mode::Solid)
			{
				pattern.period_ms = 0;
				return pattern;
			}

			pattern.period_ms = PeriodOrDefault(pattern.period_ms);

			if (pattern.mode == Mode::ColorCycle && (pattern.palette == nullptr || pattern.palette_count == 0))
			{
				pattern.palette = palettes::Rainbow;
				pattern.palette_count = sizeof(palettes::Rainbow) / sizeof(palettes::Rainbow[0]);
			}

			return pattern;
		}

		Color RenderColorCycle(const Pattern& pattern, uint32_t elapsed_ms)
		{
			const Color* palette = pattern.palette;
			std::size_t count = pattern.palette_count;

			if (palette == nullptr || count == 0)
			{
				palette = palettes::Rainbow;
				count = sizeof(palettes::Rainbow) / sizeof(palettes::Rainbow[0]);
			}

			if (count == 1)
			{
				return palette[0];
			}

			const uint32_t period = PeriodOrDefault(pattern.period_ms);
			const uint32_t phase = elapsed_ms % period;
			const uint64_t scaled = (static_cast<uint64_t>(phase) * count * 256u) / period;
			const std::size_t index = static_cast<std::size_t>((scaled / 256u) % count);
			const std::size_t next = (index + 1u) % count;
			const uint8_t amount = static_cast<uint8_t>(scaled & 0xffu);

			return Blend(palette[index], palette[next], amount);
		}

		Color RenderPattern(const Pattern& pattern, uint32_t elapsed_ms)
		{
			switch (pattern.mode)
			{
			case Mode::Off:
				return colors::Off;

			case Mode::Solid:
				return pattern.primary;

			case Mode::Blink:
			{
				const uint32_t period = PeriodOrDefault(pattern.period_ms);
				return (elapsed_ms % period) < (period / 2u) ? pattern.primary : pattern.secondary;
			}

			case Mode::Breathe:
			{
				const uint8_t amount = EaseInOut(TriangleWave(elapsed_ms, pattern.period_ms));
				return Blend(pattern.secondary, pattern.primary, amount);
			}

			case Mode::Pulse:
			{
				const uint32_t period = PeriodOrDefault(pattern.period_ms);
				const uint32_t phase = elapsed_ms % period;
				const uint32_t rise = period / 5u == 0 ? 1u : period / 5u;
				uint8_t amount = 0;

				if (phase < rise)
				{
					amount = static_cast<uint8_t>((phase * 255u) / rise);
				}
				else
				{
					const uint32_t fall = period - rise == 0 ? 1u : period - rise;
					amount = static_cast<uint8_t>(((period - phase) * 255u) / fall);
				}

				return Blend(pattern.secondary, pattern.primary, EaseInOut(amount));
			}

			case Mode::Heartbeat:
			{
				const uint32_t period = PeriodOrDefault(pattern.period_ms);
				const uint32_t phase = elapsed_ms % period;
				const uint32_t first_duration = ClampU32(period * 15u / 100u, 35u, period);
				const uint32_t second_start = ClampU32(period * 23u / 100u, first_duration, period);
				const uint32_t second_duration = ClampU32(period * 18u / 100u, 35u, period - second_start);
				const uint8_t first = PulseAt(phase, 0, first_duration, 255);
				const uint8_t second = PulseAt(phase, second_start, second_duration, 180);

				return Blend(pattern.secondary, pattern.primary, EaseInOut(MaxU8(first, second)));
			}

			case Mode::Strobe:
			{
				const uint32_t period = PeriodOrDefault(pattern.period_ms);
				const uint32_t on_ms = ClampU32(period / 8u, 20u, period / 2u == 0 ? 1u : period / 2u);
				return (elapsed_ms % period) < on_ms ? pattern.primary : pattern.secondary;
			}

			case Mode::Fade:
			{
				const uint8_t amount = EaseInOut(TriangleWave(elapsed_ms, pattern.period_ms));
				return Blend(pattern.primary, pattern.secondary, amount);
			}

			case Mode::Rainbow:
			{
				const uint32_t period = PeriodOrDefault(pattern.period_ms);
				const uint16_t hue = static_cast<uint16_t>(((elapsed_ms % period) * 360u) / period);
				return Color::HSV(hue);
			}

			case Mode::ColorCycle:
				return RenderColorCycle(pattern, elapsed_ms);

			case Mode::Sparkle:
			{
				const uint32_t period = PeriodOrDefault(pattern.period_ms);
				const uint32_t frame_ms = ClampU32(period / 32u, FrameIntervalMs, 120u);
				const uint32_t frame = elapsed_ms / frame_ms;
				const uint32_t hash = Hash32(frame);
				const bool sparkle = (hash & 0x7u) == 0;
				const uint8_t amount = sparkle ? static_cast<uint8_t>(160u + ((hash >> 8u) & 0x5fu)) : 0;

				return Blend(pattern.secondary, pattern.primary, amount);
			}

			case Mode::Candle:
			{
				const uint32_t period = PeriodOrDefault(pattern.period_ms);
				const uint32_t frame_ms = ClampU32(period / 48u, FrameIntervalMs, 80u);
				const uint32_t frame = elapsed_ms / frame_ms;
				const uint32_t hash = Hash32(frame);
				const uint8_t amount = static_cast<uint8_t>(120u + (hash & 0x7fu));

				return Blend(pattern.secondary, pattern.primary, amount);
			}
			}

			return colors::Off;
		}

#if LED_NEOPIXEL

		extern "C" bool set_ws2812(uint32_t value);

		uint32_t PackNeoPixel(Color color)
		{
	#if LED_RED_GREEN_SWAPPED
			return (static_cast<uint32_t>(color.g) << 16u) | (static_cast<uint32_t>(color.r) << 8u) | color.b;
	#else
			return (static_cast<uint32_t>(color.r) << 16u) | (static_cast<uint32_t>(color.g) << 8u) | color.b;
	#endif
		}

		void BackendInit()
		{
			status_led_init();
		}

		void BackendWrite(Color color)
		{
			set_ws2812(PackNeoPixel(color));
		}

#elif LED_RGB_PWM

		struct RgbPwmContext
		{
			uint r_slice = 0;
			uint r_chan = 0;
			uint g_slice = 0;
			uint g_chan = 0;
			uint b_slice = 0;
			uint b_chan = 0;
		};

		RgbPwmContext g_rgb_pwm;

		void EnablePwmPin(uint pin, uint* slice, uint* channel)
		{
			gpio_set_function(pin, GPIO_FUNC_PWM);
			*slice = pwm_gpio_to_slice_num(pin);
			*channel = pwm_gpio_to_channel(pin);
			pwm_set_wrap(*slice, PwmWrap);
		}

		void BackendInit()
		{
			EnablePwmPin(LED_RGB_RED_PIN, &g_rgb_pwm.r_slice, &g_rgb_pwm.r_chan);
			EnablePwmPin(LED_RGB_GREEN_PIN, &g_rgb_pwm.g_slice, &g_rgb_pwm.g_chan);
			EnablePwmPin(LED_RGB_BLUE_PIN, &g_rgb_pwm.b_slice, &g_rgb_pwm.b_chan);

	#if LED_RGB_ACTIVE_LOW
			pwm_set_output_polarity(g_rgb_pwm.r_slice, true, true);
			pwm_set_output_polarity(g_rgb_pwm.g_slice, true, true);
			pwm_set_output_polarity(g_rgb_pwm.b_slice, true, true);
	#endif

			pwm_set_chan_level(g_rgb_pwm.r_slice, g_rgb_pwm.r_chan, 0);
			pwm_set_chan_level(g_rgb_pwm.g_slice, g_rgb_pwm.g_chan, 0);
			pwm_set_chan_level(g_rgb_pwm.b_slice, g_rgb_pwm.b_chan, 0);

			pwm_set_enabled(g_rgb_pwm.r_slice, true);
			pwm_set_enabled(g_rgb_pwm.g_slice, true);
			pwm_set_enabled(g_rgb_pwm.b_slice, true);
		}

		void BackendWrite(Color color)
		{
			pwm_set_chan_level(g_rgb_pwm.r_slice, g_rgb_pwm.r_chan, color.r);
			pwm_set_chan_level(g_rgb_pwm.g_slice, g_rgb_pwm.g_chan, color.g);
			pwm_set_chan_level(g_rgb_pwm.b_slice, g_rgb_pwm.b_chan, color.b);
		}

#elif LED_SINGLE_COLOR

		uint8_t Luminance(Color color)
		{
			return static_cast<uint8_t>((77u * color.r + 150u * color.g + 29u * color.b) >> 8u);
		}

		struct SinglePwmContext
		{
			uint slice = 0;
			uint chan = 0;
		};

		SinglePwmContext g_single_pwm;

		void BackendInit()
		{
			gpio_set_function(LED_PIN, GPIO_FUNC_PWM);

			g_single_pwm.slice = pwm_gpio_to_slice_num(LED_PIN);
			g_single_pwm.chan = pwm_gpio_to_channel(LED_PIN);

			pwm_set_wrap(g_single_pwm.slice, PwmWrap);

	#if LED_SINGLE_ACTIVE_LOW || LED_ACTIVE_LOW || PICO_DEFAULT_LED_PIN_INVERTED
			pwm_set_output_polarity(g_single_pwm.slice, true, true);
	#endif
			pwm_set_chan_level(g_single_pwm.slice, g_single_pwm.chan, 0);
			pwm_set_enabled(g_single_pwm.slice, true);
		}

		void BackendWrite(Color color)
		{
			pwm_set_chan_level(g_single_pwm.slice, g_single_pwm.chan, Luminance(color));
		}

#else

		void BackendInit() {}
		void BackendWrite(Color color)
		{
			(void)color;
		}

#endif

		void WriteRendered(const Pattern& pattern, uint32_t elapsed_ms)
		{
			BackendWrite(Scale(RenderPattern(pattern, elapsed_ms), pattern.brightness));
		}

		bool AnimationTimerCallback(repeating_timer_t* timer)
		{
			(void)timer;

			Pattern pattern;
			uint32_t started_ms = 0;

			const uint32_t irq = save_and_disable_interrupts();
			pattern = g_ctx.pattern;
			started_ms = g_ctx.started_ms;
			restore_interrupts(irq);

			WriteRendered(pattern, NowMs() - started_ms);
			return true;
		}

		void StopTimer()
		{
			if (!g_ctx.timer_active)
			{
				return;
			}

			cancel_repeating_timer(&g_ctx.timer);
			g_ctx.timer_active = false;
		}

	}  // namespace

	Color Color::HSV(uint16_t hue, uint8_t saturation, uint8_t value)
	{
		hue %= 360u;

		if (saturation == 0)
		{
			return {value, value, value};
		}

		const uint8_t region = static_cast<uint8_t>(hue / 60u);
		const uint8_t remainder = static_cast<uint8_t>(((hue - region * 60u) * 255u) / 60u);
		const uint8_t p = Scale8(value, static_cast<uint8_t>(255u - saturation));
		const uint8_t q = Scale8(value, static_cast<uint8_t>(255u - Scale8(saturation, remainder)));
		const uint8_t t =
			Scale8(value, static_cast<uint8_t>(255u - Scale8(saturation, static_cast<uint8_t>(255u - remainder))));

		switch (region)
		{
		case 0:
			return {value, t, p};
		case 1:
			return {q, value, p};
		case 2:
			return {p, value, t};
		case 3:
			return {p, q, value};
		case 4:
			return {t, p, value};
		default:
			return {value, p, q};
		}
	}

	void Init()
	{
		if (g_ctx.initialized)
		{
			return;
		}

		g_ctx.initialized = true;
		BackendInit();
		BackendWrite(colors::Off);
	}

	void SetColor(Color color, uint8_t brightness)
	{
		SetPattern(Pattern::Solid(color, brightness));
	}

	void SetPattern(const Pattern& pattern)
	{
		if (!g_ctx.initialized)
		{
			Init();
		}

		StopTimer();

		const Pattern normalized = Normalize(pattern);
		const uint32_t started_ms = NowMs();

		const uint32_t irq = save_and_disable_interrupts();
		g_ctx.pattern = normalized;
		g_ctx.started_ms = started_ms;
		restore_interrupts(irq);

		WriteRendered(normalized, 0);

		if (IsAnimated(normalized.mode))
		{
			g_ctx.timer_active = add_repeating_timer_ms(
				-static_cast<int64_t>(FrameIntervalMs), AnimationTimerCallback, nullptr, &g_ctx.timer);
		}
	}

	void Off()
	{
		SetPattern(Pattern::Off());
	}

}  // namespace led
