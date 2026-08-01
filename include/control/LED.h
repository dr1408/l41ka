#pragma once

#include <cstddef>
#include <cstdint>

namespace led {

struct Color {
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;

    constexpr Color() = default;
    constexpr Color(uint8_t red, uint8_t green, uint8_t blue) : r(red), g(green), b(blue) {}

    static constexpr Color RGB(uint8_t red, uint8_t green, uint8_t blue) {
        return {red, green, blue};
    }

    static Color HSV(uint16_t hue, uint8_t saturation = 255, uint8_t value = 255);

    constexpr bool IsOff() const {
        return r == 0 && g == 0 && b == 0;
    }
};

enum class Mode : uint8_t {
    Off,
    Solid,
    Blink,
    Breathe,
    Pulse,
    Heartbeat,
    Strobe,
    Fade,
    Rainbow,
    ColorCycle,
    Sparkle,
    Candle,
};

struct Pattern {
    Mode mode = Mode::Solid;
    Color primary = Color::RGB(255, 255, 255);
    Color secondary = Color::RGB(0, 0, 0);
    uint32_t period_ms = 1000;
    uint8_t brightness = 255;
    const Color *palette = nullptr;
    std::size_t palette_count = 0;

    constexpr Pattern() = default;

    constexpr Pattern(
        Mode pattern_mode,
        Color primary_color,
        Color secondary_color = Color::RGB(0, 0, 0),
        uint32_t pattern_period_ms = 1000,
        uint8_t pattern_brightness = 255,
        const Color *pattern_palette = nullptr,
        std::size_t pattern_palette_count = 0
    ) :
        mode(pattern_mode),
        primary(primary_color),
        secondary(secondary_color),
        period_ms(pattern_period_ms),
        brightness(pattern_brightness),
        palette(pattern_palette),
        palette_count(pattern_palette_count) {}

    static constexpr Pattern Off() {
        return {Mode::Off, Color::RGB(0, 0, 0), Color::RGB(0, 0, 0), 0, 0};
    }

    static constexpr Pattern Solid(Color color, uint8_t brightness = 255) {
        return {Mode::Solid, color, Color::RGB(0, 0, 0), 0, brightness};
    }

    static constexpr Pattern Blink(
        Color color,
        Color off_color = Color::RGB(0, 0, 0),
        uint32_t period_ms = 500,
        uint8_t brightness = 255
    ) {
        return {Mode::Blink, color, off_color, period_ms, brightness};
    }

    static constexpr Pattern Breathe(
        Color color,
        Color base_color = Color::RGB(0, 0, 0),
        uint32_t period_ms = 1800,
        uint8_t brightness = 255
    ) {
        return {Mode::Breathe, color, base_color, period_ms, brightness};
    }

    static constexpr Pattern Pulse(
        Color color,
        Color base_color = Color::RGB(0, 0, 0),
        uint32_t period_ms = 1000,
        uint8_t brightness = 255
    ) {
        return {Mode::Pulse, color, base_color, period_ms, brightness};
    }

    static constexpr Pattern Heartbeat(
        Color color,
        Color base_color = Color::RGB(0, 0, 0),
        uint32_t period_ms = 1200,
        uint8_t brightness = 255
    ) {
        return {Mode::Heartbeat, color, base_color, period_ms, brightness};
    }

    static constexpr Pattern Strobe(
        Color color,
        Color off_color = Color::RGB(0, 0, 0),
        uint32_t period_ms = 120,
        uint8_t brightness = 255
    ) {
        return {Mode::Strobe, color, off_color, period_ms, brightness};
    }

    static constexpr Pattern Fade(
        Color from,
        Color to,
        uint32_t period_ms = 2000,
        uint8_t brightness = 255
    ) {
        return {Mode::Fade, from, to, period_ms, brightness};
    }

    static constexpr Pattern Rainbow(uint32_t period_ms = 3000, uint8_t brightness = 255) {
        return {Mode::Rainbow, Color::RGB(255, 0, 0), Color::RGB(0, 0, 0), period_ms, brightness};
    }

    static constexpr Pattern ColorCycle(
        const Color *colors,
        std::size_t count,
        uint32_t period_ms = 3000,
        uint8_t brightness = 255
    ) {
        return {Mode::ColorCycle, Color::RGB(0, 0, 0), Color::RGB(0, 0, 0), period_ms, brightness, colors, count};
    }

    template <std::size_t N>
    static constexpr Pattern ColorCycle(
        const Color (&colors)[N],
        uint32_t period_ms = 3000,
        uint8_t brightness = 255
    ) {
        return ColorCycle(colors, N, period_ms, brightness);
    }

    static constexpr Pattern Sparkle(
        Color color,
        Color base_color = Color::RGB(0, 0, 0),
        uint32_t period_ms = 900,
        uint8_t brightness = 255
    ) {
        return {Mode::Sparkle, color, base_color, period_ms, brightness};
    }

    static constexpr Pattern Candle(
        Color flame = Color::RGB(255, 122, 24),
        Color ember = Color::RGB(72, 12, 0),
        uint32_t period_ms = 1400,
        uint8_t brightness = 255
    ) {
        return {Mode::Candle, flame, ember, period_ms, brightness};
    }
};

namespace colors {
inline constexpr Color Off = Color::RGB(0, 0, 0);
inline constexpr Color Black = Color::RGB(0, 0, 0);
inline constexpr Color White = Color::RGB(255, 255, 255);
inline constexpr Color Red = Color::RGB(255, 0, 0);
inline constexpr Color Green = Color::RGB(0, 255, 0);
inline constexpr Color Blue = Color::RGB(0, 0, 255);
inline constexpr Color Amber = Color::RGB(255, 90, 0);
inline constexpr Color Orange = Color::RGB(255, 128, 0);
inline constexpr Color Yellow = Color::RGB(255, 255, 0);
inline constexpr Color Lime = Color::RGB(128, 255, 0);
inline constexpr Color Mint = Color::RGB(62, 255, 168);
inline constexpr Color Cyan = Color::RGB(0, 255, 255);
inline constexpr Color Sky = Color::RGB(64, 180, 255);
inline constexpr Color Azure = Color::RGB(0, 128, 255);
inline constexpr Color Indigo = Color::RGB(75, 0, 130);
inline constexpr Color Violet = Color::RGB(148, 0, 211);
inline constexpr Color Purple = Color::RGB(128, 0, 255);
inline constexpr Color Magenta = Color::RGB(255, 0, 255);
inline constexpr Color Pink = Color::RGB(255, 64, 160);
inline constexpr Color Rose = Color::RGB(255, 0, 96);
inline constexpr Color Crimson = Color::RGB(220, 20, 60);
inline constexpr Color Scarlet = Color::RGB(255, 36, 0);
inline constexpr Color Vermilion = Color::RGB(227, 66, 52);
inline constexpr Color Coral = Color::RGB(255, 127, 80);
inline constexpr Color Salmon = Color::RGB(250, 128, 114);
inline constexpr Color Peach = Color::RGB(255, 190, 152);
inline constexpr Color Gold = Color::RGB(255, 215, 0);
inline constexpr Color Lemon = Color::RGB(255, 247, 0);
inline constexpr Color Chartreuse = Color::RGB(127, 255, 0);
inline constexpr Color SpringGreen = Color::RGB(0, 255, 127);
inline constexpr Color SeaGreen = Color::RGB(46, 139, 87);
inline constexpr Color Emerald = Color::RGB(0, 201, 87);
inline constexpr Color Jade = Color::RGB(0, 168, 107);
inline constexpr Color Teal = Color::RGB(0, 128, 128);
inline constexpr Color Turquoise = Color::RGB(64, 224, 208);
inline constexpr Color Aqua = Color::RGB(0, 255, 200);
inline constexpr Color Ice = Color::RGB(180, 240, 255);
inline constexpr Color Navy = Color::RGB(0, 0, 128);
inline constexpr Color Cobalt = Color::RGB(0, 71, 171);
inline constexpr Color Sapphire = Color::RGB(15, 82, 186);
inline constexpr Color Periwinkle = Color::RGB(204, 204, 255);
inline constexpr Color Lavender = Color::RGB(181, 126, 220);
inline constexpr Color Lilac = Color::RGB(200, 162, 200);
inline constexpr Color Plum = Color::RGB(142, 69, 133);
inline constexpr Color Fuchsia = Color::RGB(255, 0, 128);
inline constexpr Color HotPink = Color::RGB(255, 105, 180);
inline constexpr Color DeepPink = Color::RGB(255, 20, 147);
inline constexpr Color Maroon = Color::RGB(128, 0, 0);
inline constexpr Color Brown = Color::RGB(150, 75, 0);
inline constexpr Color Chocolate = Color::RGB(210, 105, 30);
inline constexpr Color Copper = Color::RGB(184, 115, 51);
inline constexpr Color Bronze = Color::RGB(205, 127, 50);
inline constexpr Color Tan = Color::RGB(210, 180, 140);
inline constexpr Color Ivory = Color::RGB(255, 255, 240);
inline constexpr Color WarmWhite = Color::RGB(255, 214, 170);
inline constexpr Color CoolWhite = Color::RGB(210, 235, 255);
inline constexpr Color Silver = Color::RGB(192, 192, 192);
inline constexpr Color Gray = Color::RGB(128, 128, 128);
inline constexpr Color Slate = Color::RGB(112, 128, 144);
inline constexpr Color DimRed = Color::RGB(32, 0, 0);
inline constexpr Color DimGreen = Color::RGB(0, 32, 0);
inline constexpr Color DimBlue = Color::RGB(0, 0, 32);
inline constexpr Color DimAmber = Color::RGB(36, 8, 0);
}

namespace palettes {
inline constexpr Color Rainbow[] = {
    colors::Red,
    colors::Orange,
    colors::Yellow,
    colors::Green,
    colors::Cyan,
    colors::Blue,
    colors::Violet,
    colors::Magenta,
};

inline constexpr Color Warm[] = {
    colors::Red,
    colors::Scarlet,
    colors::Orange,
    colors::Amber,
    colors::Gold,
    colors::WarmWhite,
};

inline constexpr Color Cool[] = {
    colors::Mint,
    colors::Cyan,
    colors::Sky,
    colors::Blue,
    colors::Indigo,
    colors::Violet,
};

inline constexpr Color Status[] = {
    colors::Amber,
    colors::Blue,
    colors::Green,
    colors::Red,
};
}

void Init();
void SetColor(Color color, uint8_t brightness = 255);
void SetPattern(const Pattern &pattern);
void Off();

} // namespace led
