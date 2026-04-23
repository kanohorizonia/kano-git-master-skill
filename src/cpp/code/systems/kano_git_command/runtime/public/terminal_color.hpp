#pragma once

#include <cstdlib>
#include <iostream>
#include <string>

#if defined(_WIN32)
#include <io.h>
#include <windows.h>
#define ISATTY(fd) _isatty(fd)
#define STDOUT_FILENO 1
#define STDERR_FILENO 2
#else
#include <unistd.h>
#define ISATTY(fd) isatty(fd)
#ifndef STDOUT_FILENO
#define STDOUT_FILENO 1
#endif
#ifndef STDERR_FILENO
#define STDERR_FILENO 2
#endif
#endif

namespace kano::terminal {

namespace Color {
    inline const char* Reset = "\033[0m";
    inline const char* Bold = "\033[1m";
    inline const char* Dim = "\033[2m";

    inline const char* Red = "\033[31m";
    inline const char* Green = "\033[32m";
    inline const char* Yellow = "\033[33m";
    inline const char* Blue = "\033[34m";
    inline const char* Magenta = "\033[35m";
    inline const char* Cyan = "\033[36m";
    inline const char* White = "\033[37m";

    inline const char* BoldRed = "\033[1;31m";
    inline const char* BoldGreen = "\033[1;32m";
    inline const char* BoldYellow = "\033[1;33m";
    inline const char* BoldBlue = "\033[1;34m";
    inline const char* BoldMagenta = "\033[1;35m";
    inline const char* BoldCyan = "\033[1;36m";
    inline const char* BoldWhite = "\033[1;37m";
} // namespace Color

inline bool IsStdoutInteractive() {
    static bool enabled = []() {
        const char* noColor = std::getenv("NO_COLOR");
        if (noColor != nullptr && *noColor != '\0') return false;
        const char* kogNoColor = std::getenv("KOG_NO_COLOR");
        if (kogNoColor != nullptr && *kogNoColor != '\0') return false;

#if defined(_WIN32)
        if (ISATTY(STDOUT_FILENO) != 0) {
            HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
            if (hOut != INVALID_HANDLE_VALUE) {
                DWORD dwMode = 0;
                if (GetConsoleMode(hOut, &dwMode)) {
                    SetConsoleMode(hOut, dwMode | 0x0004); // ENABLE_VIRTUAL_TERMINAL_PROCESSING
                }
            }
            return true;
        }
        return false;
#else
        return ISATTY(STDOUT_FILENO) != 0;
#endif
    }();
    return enabled;
}

inline bool IsStderrInteractive() {
    static bool enabled = []() {
        const char* noColor = std::getenv("NO_COLOR");
        if (noColor != nullptr && *noColor != '\0') return false;
        const char* kogNoColor = std::getenv("KOG_NO_COLOR");
        if (kogNoColor != nullptr && *kogNoColor != '\0') return false;
#if defined(_WIN32)
        if (ISATTY(STDERR_FILENO) != 0) {
            HANDLE hErr = GetStdHandle(STD_ERROR_HANDLE);
            if (hErr != INVALID_HANDLE_VALUE) {
                DWORD dwMode = 0;
                if (GetConsoleMode(hErr, &dwMode)) {
                    SetConsoleMode(hErr, dwMode | 0x0004); // ENABLE_VIRTUAL_TERMINAL_PROCESSING
                }
            }
            return true;
        }
        return false;
#else
        return ISATTY(STDERR_FILENO) != 0;
#endif
    }();
    return enabled;
}

inline std::string Wrap(const std::string& InText, const char* InColor, bool InStderr = false) {
    if (InStderr ? IsStderrInteractive() : IsStdoutInteractive()) {
        return std::string(InColor) + InText + Color::Reset;
    }
    return InText;
}

inline std::string PlanPrefix() {
    return Wrap("[plan]", Color::BoldCyan);
}

inline std::string AiPrefix() {
    return Wrap("[kog ai]", Color::BoldMagenta);
}

inline std::string LauncherPrefix() {
    return Wrap("[launcher]", Color::BoldGreen);
}

inline std::string StepHeader(const std::string& InLabel) {
    return Wrap("=== " + InLabel + " ===", Color::BoldCyan);
}

inline std::string PreflightHeader(const std::string& InLabel) {
    return Wrap("=== " + InLabel + " ===", Color::BoldWhite);
}

inline std::string PassTag() {
    return Wrap("[PASS]", Color::BoldGreen);
}

inline std::string FailTag() {
    return Wrap("[FAIL]", Color::BoldRed);
}

inline std::string WarnTag() {
    return Wrap("[WARN]", Color::BoldYellow);
}

inline std::string InfoTag() {
    return Wrap("[INFO]", Color::BoldBlue);
}

} // namespace kano::terminal
