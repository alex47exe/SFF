// LumaCore — Steam client hook layer for SteaMidra.
// Copyright (c) 2025-2026 Midrag (https://github.com/Midrags).
// Distributed under the GNU General Public License v3 or later.
// See <https://www.gnu.org/licenses/> for the full license text.

#pragma once

#include "entry.h"

#include <string>

namespace AppConfig {

    std::string JsoncPathFor(const std::string& luaFilePath);

    void EnsureJsonc(const std::string& luaFilePath,
                     AppId_t appId,
                     const std::string& gameName);

    void LoadJsonc(const std::string& luaFilePath,
                   AppId_t appId,
                   const std::string& gameName);

    void Unload(AppId_t appId);

    bool IsOnlineFixEnabled(AppId_t appId);
    bool IsAllowUpdateEnabled(AppId_t appId);
}

