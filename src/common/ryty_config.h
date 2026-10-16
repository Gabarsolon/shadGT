// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdio>
#include <optional>
#include <string>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace Common {

/// The launcher's game settings (user\custom_configs\CUSA03220.json next to the exe), read once.
inline const std::string& RytyGameConfigText() {
    static const std::string text = [] {
        std::string result;
#ifdef _WIN32
        char path[MAX_PATH];
        const DWORD length = GetModuleFileNameA(nullptr, path, MAX_PATH);
        if (length == 0 || length >= MAX_PATH) {
            return result;
        }
        std::string config{path, length};
        config = config.substr(0, config.find_last_of("\\/") + 1) +
                 "user\\custom_configs\\CUSA03220.json";
        if (std::FILE* file = std::fopen(config.c_str(), "rb")) {
            char chunk[4096];
            while (const size_t n = std::fread(chunk, 1, sizeof(chunk), file)) {
                result.append(chunk, n);
            }
            std::fclose(file);
        }
#endif
        return result;
    }();
    return text;
}

/// The raw value after "key": in the launcher's settings (true, false, a number), if present.
inline std::optional<std::string> RytyGameConfigValue(const char* key) {
    const std::string& text = RytyGameConfigText();
    const std::string quoted = std::string{"\""} + key + "\"";
    const size_t at = text.find(quoted);
    if (at == std::string::npos) {
        return std::nullopt;
    }
    const size_t begin = text.find_first_not_of(" \t\r\n:", at + quoted.size());
    if (begin == std::string::npos) {
        return std::nullopt;
    }
    const size_t end = text.find_first_of(",}\r\n", begin);
    return text.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
}

} // namespace Common
