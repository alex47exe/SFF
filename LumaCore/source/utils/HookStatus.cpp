// LumaCore - Steam client hook layer for SteaMidra.
// Copyright (c) 2025-2026 Midrag (https://github.com/Midrags).
// Distributed under the GNU General Public License v3 or later.
// See <https://www.gnu.org/licenses/> for the full license text.

#include "HookStatus.h"

#include "Logger.h"
#include "../entry.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <exception>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace HookStatus {

    namespace {

        std::mutex g_mu;

        std::string g_buildId;
        std::string g_steamclientSha;
        std::string g_steamuiSha;
        bool        g_steamclientToml = false;
        bool        g_steamuiToml     = false;
        std::uint64_t            g_installed = 0;
        std::vector<std::string> g_missed;
        std::unordered_set<std::string> g_criticalHooks;
        std::unordered_map<uint32_t, AppInfo> g_apps;
        bool        g_initDone        = false;

        std::string JsonEscape(std::string_view s) {
            std::string out;
            out.reserve(s.size() + 2);
            for (char ch : s) {
                unsigned char c = static_cast<unsigned char>(ch);
                switch (c) {
                    case '"':  out += "\\\""; break;
                    case '\\': out += "\\\\"; break;
                    case '\b': out += "\\b";  break;
                    case '\f': out += "\\f";  break;
                    case '\n': out += "\\n";  break;
                    case '\r': out += "\\r";  break;
                    case '\t': out += "\\t";  break;
                    default:
                        if (c < 0x20 || c > 0x7E) {
                            char buf[8];
                            std::snprintf(buf, sizeof(buf), "\\u%04X", c);
                            out += buf;
                        } else {
                            out += static_cast<char>(c);
                        }
                        break;
                }
            }
            return out;
        }

        std::pair<bool, std::string> ComputeDegradedLocked() {
            std::vector<std::string> hits;
            for (const auto& name : g_missed) {
                if (g_criticalHooks.count(name)) hits.push_back(name);
            }
            if (hits.empty()) return {false, ""};
            std::sort(hits.begin(), hits.end());
            hits.erase(std::unique(hits.begin(), hits.end()), hits.end());

            std::string reason = "critical hooks missed: ";
            for (size_t i = 0; i < hits.size(); ++i) {
                if (i) reason += ", ";
                reason += hits[i];
            }
            return {true, reason};
        }

        std::string SerializeLocked() {
            auto [degraded, reason] = ComputeDegradedLocked();

            std::string out;
            out.reserve(512 + g_missed.size() * 32 + g_apps.size() * 128);
            out += "{\n";
            out += "  \"build_id\": \"";
            out += JsonEscape(g_buildId);
            out += "\",\n";
            out += "  \"toml_found\": {\n";
            out += "    \"steamclient\": ";
            out += g_steamclientToml ? "true" : "false";
            out += ",\n";
            out += "    \"steamui\": ";
            out += g_steamuiToml ? "true" : "false";
            out += "\n  },\n";
            out += "  \"hooks_installed\": ";
            out += std::to_string(g_installed);
            out += ",\n";
            out += "  \"hooks_missed\": [";
            for (size_t i = 0; i < g_missed.size(); ++i) {
                if (i) out += ", ";
                out += "\"";
                out += JsonEscape(g_missed[i]);
                out += "\"";
            }
            out += "],\n";
            out += "  \"steamclient_sha\": \"";
            out += JsonEscape(g_steamclientSha);
            out += "\",\n";
            out += "  \"steamui_sha\": \"";
            out += JsonEscape(g_steamuiSha);
            out += "\",\n";
            out += "  \"degraded_mode\": ";
            out += degraded ? "true" : "false";
            out += ",\n";
            out += "  \"degraded_reason\": \"";
            out += JsonEscape(reason);
            out += "\",\n";
            out += "  \"apps\": [\n";

            std::vector<uint32_t> ids;
            ids.reserve(g_apps.size());
            for (const auto& [id, _] : g_apps) ids.push_back(id);
            std::sort(ids.begin(), ids.end());
            for (size_t i = 0; i < ids.size(); ++i) {
                const AppInfo& app = g_apps[ids[i]];
                out += "    {";
                out += "\"app_id\": " + std::to_string(app.appId);
                out += ", \"game_name\": \"" + JsonEscape(app.gameName) + "\"";
                out += ", \"lua_path\": \"" + JsonEscape(app.luaPath) + "\"";
                out += ", \"jsonc_path\": \"" + JsonEscape(app.jsoncPath) + "\"";
                out += ", \"onlinefix\": " + std::string(app.onlinefix ? "true" : "false");
                out += ", \"allow_update\": " + std::string(app.allowUpdate ? "true" : "false");
                out += ", \"manifest_mode\": \"" + JsonEscape(app.manifestMode) + "\"";
                out += ", \"manifest_gid\": " + std::to_string(app.manifestGid);
                out += "}";
                if (i + 1 != ids.size()) out += ",";
                out += "\n";
            }
            out += "  ]\n";
            out += "}\n";
            return out;
        }

        bool WriteBodyAtomic(const std::string& body) {
            if (!SteamInstallPath[0]) {
                LOG_WARN("HookStatus: SteamInstallPath unset, skipping write");
                return false;
            }
            std::filesystem::path dir = std::filesystem::path(SteamInstallPath) / "lumacore";
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            if (ec) {
                LOG_WARN("HookStatus: create_directories failed: {}", ec.message());
                return false;
            }

            std::filesystem::path target = dir / "status.json";
            std::filesystem::path tmp    = target;
            tmp += ".tmp";

            std::string narrowTmp    = tmp.string();
            std::string narrowTarget = target.string();

            {
                std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
                if (!f) {
                    LOG_WARN("HookStatus: open tmp failed for {}", narrowTarget);
                    DeleteFileA(narrowTmp.c_str());
                    return false;
                }
                f.write(body.data(), static_cast<std::streamsize>(body.size()));
                f.flush();
                if (!f) {
                    LOG_WARN("HookStatus: write tmp failed for {}", narrowTarget);
                    f.close();
                    DeleteFileA(narrowTmp.c_str());
                    return false;
                }
            }

            if (!MoveFileExA(narrowTmp.c_str(), narrowTarget.c_str(),
                             MOVEFILE_REPLACE_EXISTING)) {
                DWORD err = GetLastError();
                LOG_WARN("HookStatus: MoveFileExA failed err={} for {}",
                         err, narrowTarget);
                DeleteFileA(narrowTmp.c_str());
                return false;
            }
            return true;
        }

        void MaybeRepublishLocked() {
            if (!g_initDone) return;
            std::string body = SerializeLocked();
            (void)WriteBodyAtomic(body);
        }

    }  // namespace

    void SetBuildId(std::string buildId) {
        std::lock_guard<std::mutex> lk(g_mu);
        g_buildId = std::move(buildId);
        MaybeRepublishLocked();
    }

    void SetTomlAvailability(std::string_view moduleName, bool found) {
        std::lock_guard<std::mutex> lk(g_mu);
        if (moduleName == "steamclient") {
            g_steamclientToml = found;
        } else if (moduleName == "steamui") {
            g_steamuiToml = found;
        } else {
            LOG_WARN("HookStatus: unknown module '{}' in SetTomlAvailability",
                     std::string(moduleName));
            return;
        }
        MaybeRepublishLocked();
    }

    void SetShas(std::string steamclientSha, std::string steamuiSha) {
        std::lock_guard<std::mutex> lk(g_mu);
        g_steamclientSha = std::move(steamclientSha);
        g_steamuiSha     = std::move(steamuiSha);
        MaybeRepublishLocked();
    }

    void SetCriticalHooks(std::vector<std::string> hooks) {
        std::lock_guard<std::mutex> lk(g_mu);
        g_criticalHooks.clear();
        for (auto& h : hooks) {
            if (!h.empty()) g_criticalHooks.insert(std::move(h));
        }
        MaybeRepublishLocked();
    }

    void RecordInstalled() {
        std::lock_guard<std::mutex> lk(g_mu);
        ++g_installed;
        MaybeRepublishLocked();
    }

    void RecordMissed(std::string hookName) {
        if (hookName.empty()) return;
        std::lock_guard<std::mutex> lk(g_mu);
        if (g_criticalHooks.count(hookName)) {
            LOG_WARN("HookStatus: critical hook '{}' missed", hookName);
        }
        g_missed.push_back(std::move(hookName));
        MaybeRepublishLocked();
    }

    void PublishApp(const AppInfo& app) {
        if (app.appId == 0) return;
        std::lock_guard<std::mutex> lk(g_mu);
        g_apps[app.appId] = app;
        MaybeRepublishLocked();
    }

    void UnpublishApp(uint32_t appId) {
        std::lock_guard<std::mutex> lk(g_mu);
        g_apps.erase(appId);
        MaybeRepublishLocked();
    }

    void WriteToDisk() {
        std::string body;
        {
            std::lock_guard<std::mutex> lk(g_mu);
            body = SerializeLocked();
            g_initDone = true;
        }
        try {
            (void)WriteBodyAtomic(body);
        } catch (const std::exception& e) {
            LOG_WARN("HookStatus: write threw '{}'", e.what());
        } catch (...) {
            LOG_WARN("HookStatus: write threw unknown");
        }
    }

}  // namespace HookStatus
