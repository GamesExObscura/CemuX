#pragma once
#include <whb/log_console.h>
#include <whb/log.h>
#include <cstdarg>
#include <cwchar>
#include <array>

// Use WUT's standard console instead of a custom font/framebuffer renderer.
inline bool WHBLogFreetypeInit() { return WHBLogConsoleInit() != 0; }
inline void WHBLogFreetypeFree() { WHBLogConsoleFree(); }
inline void WHBLogFreetypeSetBackgroundColor(uint32_t color) { WHBLogConsoleSetColor(color); }
inline void WHBLogFreetypeSetFontColor(uint32_t) {}
inline void WHBLogFreetypeStartScreen() { for (unsigned i = 0; i < 16; ++i) WHBLogPrint(""); }
inline void WHBLogFreetypePrint(const wchar_t* input) {
    std::array<char, 53> line{}; size_t n = 0;
    for (size_t i = 0; input[i] && i < 511; ++i) {
        if (input[i] == L'\n' || n == 52) { line[n] = 0; WHBLogPrint(line.data()); n = 0; }
        if (input[i] != L'\n') line[n++] = input[i] >= 32 && input[i] <= 126 ? static_cast<char>(input[i]) : '?';
    }
    line[n] = 0; WHBLogPrint(line.data());
}
inline void WHBLogFreetypePrintf(const wchar_t* format, ...) {
    std::array<wchar_t, 512> text{}; va_list args; va_start(args, format);
    const auto result = vswprintf(text.data(), text.size(), format, args); va_end(args);
    if (result < 0) text.back() = 0;
    WHBLogFreetypePrint(text.data());
}
inline void WHBLogFreetypeDrawScreen() { WHBLogConsoleDraw(); }
