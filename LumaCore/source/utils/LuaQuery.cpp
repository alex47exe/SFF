// LumaCore - Steam client hook layer for SteaMidra.
// Copyright (c) 2025-2026 Midrag (https://github.com/Midrags).
// Distributed under the GNU General Public License v3 or later.
// See <https://www.gnu.org/licenses/> for the full license text.
//
// Public LuaLoader query API plus the directory / per-file parser orchestration.
//
// ParseFile uses a stack-allocated ParseSession that records depots through
// the bindings as they fire, then publishes pending additions/removals when
// the session ends. The chunk-by-chunk line accumulator the previous
// implementation used is gone; modern Lua handles multi-line statements
// with a single luaL_loadstring call, and per-line error context is
// available through luaL_loadbuffer's chunk name.

#include "LuaLoaderInternal.h"
#include "AppConfig.h"
#include "HookStatus.h"
#include "Logger.h"

#include <lua.hpp>
#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <future>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace {
    // File-local strict-decimal uint64 parser, re-declared per translation
    // unit so neither LuaBindings.cpp nor LuaQuery.cpp pulls a shared header.
    // Behaviour matches the LuaBindings copy: digit-only, no whitespace, no
    // signs, no 0x/0X prefix; std::stoull is wrapped so neither
    // std::invalid_argument nor std::out_of_range escapes.
    bool TryParseUInt64Decimal(std::string_view text, uint64_t& out) {
        if (text.empty()) return false;
        for (char c : text) {
            if (c < '0' || c > '9') return false;
        }
        try {
            std::string buf(text);
            size_t consumed = 0;
            uint64_t v = std::stoull(buf, &consumed, 10);
            if (consumed != buf.size()) return false;
            out = v;
            return true;
        } catch (const std::invalid_argument&) {
            return false;
        } catch (const std::out_of_range&) {
            return false;
        }
    }
    int64_t ReadMtimeSeconds(const std::string& filePath) {
        int64_t lua_mtime_secs = 0;
        WIN32_FILE_ATTRIBUTE_DATA attr{};
        if (GetFileAttributesExA(filePath.c_str(), GetFileExInfoStandard, &attr)) {
            ULARGE_INTEGER ull{};
            ull.LowPart  = attr.ftLastWriteTime.dwLowDateTime;
            ull.HighPart = attr.ftLastWriteTime.dwHighDateTime;
            constexpr uint64_t kEpochOffset = 116444736000000000ull;
            if (ull.QuadPart >= kEpochOffset) {
                lua_mtime_secs = static_cast<int64_t>((ull.QuadPart - kEpochOffset) / 10000000ull);
            }
        }
        return lua_mtime_secs;
    }

        std::optional<AppId_t> ParseAppIdFromLuaPath(const std::filesystem::path& path) {
            const std::string stem = path.stem().string();
            if (stem.empty()
                || !std::all_of(stem.begin(), stem.end(),
                                [](unsigned char c){ return std::isdigit(c); })) {
                return std::nullopt;
            }
            uint64_t val = 0;
            if (!TryParseUInt64Decimal(stem, val) || val == 0 || val > UINT32_MAX) {
                return std::nullopt;
            }
            return static_cast<AppId_t>(val);
        }

        struct FileSnapshot {
            std::string path;
            std::string body;
            int64_t mtime = 0;
            std::optional<AppId_t> appId;
        };
}

namespace LuaLoader {

    // ── public query surface ──────────────────────────────────────────────
    bool HasDepot(AppId_t depotId) {
        using namespace Internal;
        return DepotKeySet.count(depotId) && !OwnedAppIdSet.count(depotId);
    }

    bool IsOwned(AppId_t appId) {
        using namespace Internal;
        return OwnedAppIdSet.count(appId) > 0;
    }

    int64_t GetLuaMtime(AppId_t appId) {
        using namespace Internal;
        auto it = LuaMtimeMap.find(appId);
        return it == LuaMtimeMap.end() ? 0 : it->second;
    }

    std::string GetLuaFilePath(AppId_t appId) {
        using namespace Internal;
        auto it = LuaFilePathMap.find(appId);
        return it == LuaFilePathMap.end() ? std::string() : it->second;
    }

    void MarkOwned(AppId_t appId) {
        using namespace Internal;
        if (OwnedAppIdSet.insert(appId).second) {
            LOG_PACKAGE_INFO("Marking app {} as owned", appId);
        }
    }

    std::vector<AppId_t> GetAllDepotIds() {
        using namespace Internal;
        std::vector<AppId_t> ids;
        ids.reserve(DepotKeySet.size());
        for (const auto& [id, _] : DepotKeySet) ids.push_back(id);
        return ids;
    }

    std::vector<uint8> GetDecryptionKey(AppId_t depotId) {
        using namespace Internal;
        std::vector<uint8> bytes;
        auto it = DepotKeySet.find(depotId);
        if (it == DepotKeySet.end()) return bytes;

        const std::string& hex = it->second;
        bytes.reserve(hex.size() / 2);
        for (size_t i = 0; i + 1 < hex.size(); i += 2) {
            uint8_t b = 0;
            auto [_, ec] = std::from_chars(hex.data() + i, hex.data() + i + 2, b, 16);
            if (ec == std::errc{}) {
                bytes.push_back(b);
            }
        }
        return bytes;
    }

    uint64_t GetAccessToken(AppId_t appId) {
        using namespace Internal;
        auto it = AccessTokenSet.find(appId);
        return it != AccessTokenSet.end() ? it->second : 0;
    }

    bool pinApp(AppId_t appId) {
        return Internal::PinnedApps.count(appId) > 0;
    }

    // Achievement ringfence: byte-identical semantics with prior version.
    uint64_t GetStatSteamId(AppId_t appId) {
        using namespace Internal;
        auto it = StatSteamIdSet.find(appId);
        return it != StatSteamIdSet.end() ? it->second : kDefaultStatSteamId;
    }

    // Achievement ringfence: hands the wire-level UserStats spoofer either
    // a single configured stat steamid or the full fallback pool.
    const uint64_t* GetStatSteamIdPool(AppId_t appId, size_t& outCount) {
        using namespace Internal;
        auto it = StatSteamIdSet.find(appId);
        if (it != StatSteamIdSet.end()) {
            outCount = 1;
            return &it->second;
        }
        outCount = sizeof(kStatSteamIdPool) / sizeof(kStatSteamIdPool[0]);
        return kStatSteamIdPool;
    }

    const std::unordered_map<uint64_t, ManifestOverride>& GetManifestOverrides() {
        return Internal::ManifestOverrides;
    }

    // ── per-file unload ───────────────────────────────────────────────────
    void UnloadFile(const std::string& filePath) {
        using namespace Internal;
        auto it = g_fileDepots.find(filePath);
        if (it == g_fileDepots.end()) return;
        std::vector<AppId_t> depots(it->second.begin(), it->second.end());
        UnloadFile_nolock(filePath);
        LOG_PACKAGE_INFO("UnloadFile: removed {} depots from {}", depots.size(), filePath);

        if (auto appId = ParseAppIdFromLuaPath(std::filesystem::path(filePath))) {
            AppConfig::Unload(*appId);
        }

        if (Settings::cleanupOrphanJsonc) {
            std::error_code ec;
            std::filesystem::remove(AppConfig::JsoncPathFor(filePath), ec);
            if (ec) {
                LOG_PACKAGE_DEBUG("UnloadFile: jsonc cleanup failed for {} ({})",
                                  filePath, ec.message());
            }
        }
    }

    std::vector<AppId_t> TakePendingRemovals() {
        std::vector<AppId_t> out;
        out.swap(Internal::g_pendingRemovals);
        return out;
    }

    std::vector<AppId_t> TakePendingAdditions() {
        std::vector<AppId_t> out;
        out.swap(Internal::g_pendingAdditions);
        return out;
    }

    // ── single-file parser ───────────────────────────────────────────────
    void ParseFile(const std::string& filePath) {
        using namespace Internal;
        if (!Initialize()) return;

        UnloadFile_nolock(filePath);

        ParseSession session;
        session.currentFile = filePath;
        g_activeSession = &session;
        struct SessionGuard {
            ~SessionGuard() { g_activeSession = nullptr; }
        } guard;

        std::filesystem::path path(filePath);

        int64_t lua_mtime_secs = ReadMtimeSeconds(filePath);
        std::optional<AppId_t> fileAppId = ParseAppIdFromLuaPath(path);

        // Auto-register the appid that the filename stem encodes (e.g. a
        // file named "3764200.lua" registers depot 3764200 even if the
        // .lua body only calls addappid() on auxiliary depots). Also
        // re-clears OwnedAppIdSet for that appid so multi-account swaps
        // don't keep showing "Purchase".
        {
            if (fileAppId) {
                if (OwnedAppIdSet.erase(*fileAppId)) {
                    LOG_PACKAGE_INFO("ParseFile: clearing owned status for appid={} (Lua re-added)", *fileAppId);
                }
                if (!DepotKeySet.count(*fileAppId)) {
                    DepotKeySet[*fileAppId] = "";
                    session.recordDepot(*fileAppId);
                    LOG_DEBUG("ParseFile: auto-registered appid={} from filename {}", *fileAppId, path.stem().string());
                }
                if (lua_mtime_secs > 0) {
                    LuaMtimeMap[*fileAppId] = lua_mtime_secs;
                }
                LuaFilePathMap[*fileAppId] = path.lexically_normal().make_preferred().string();
            }
        }

        // Slurp the file in one shot. The previous chunk-accumulator loop
        // existed only to retry per line on syntax errors; modern Lua
        // handles multi-line statements directly through luaL_loadbuffer.
        std::ifstream file(path);
        if (!file) {
            LOG_WARN("ParseFile: failed to open {}", path.filename().string());
            return;
        }
        std::stringstream buf;
        buf << file.rdbuf();
        std::string body = buf.str();

        const std::string chunkName = path.filename().string();
        lua_settop(g_lua_state, 0);
        int rc = luaL_loadbuffer(g_lua_state, body.data(), body.size(), chunkName.c_str());
        if (rc == LUA_OK) {
            if (lua_pcall(g_lua_state, 0, 0, 0) != LUA_OK) {
                const char* err = lua_tostring(g_lua_state, -1);
                LOG_WARN("{}: {}", chunkName, err ? err : "unknown");
                lua_pop(g_lua_state, 1);
            }
        } else {
            const char* err = lua_tostring(g_lua_state, -1);
            LOG_WARN("{}: {}", chunkName, err ? err : "unknown");
            lua_pop(g_lua_state, 1);
        }

        if (fileAppId) {
            const std::string normPath = path.lexically_normal().make_preferred().string();
            AppConfig::EnsureJsonc(normPath, *fileAppId, "");
            AppConfig::LoadJsonc(normPath, *fileAppId, "");
        }
    }

    // ── directory scanner ────────────────────────────────────────────────
    void ParseDirectory(const std::string& directory) {
        using namespace Internal;
        if (!Initialize()) return;

        std::error_code ec;
        if (!std::filesystem::exists(directory, ec)) {
            std::filesystem::create_directories(directory, ec);
        }
        if (!std::filesystem::exists(directory, ec)
            || !std::filesystem::is_directory(directory, ec)) {
            return;
        }

        std::vector<std::string> luaFiles;
        for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
            if (ec) break;
            if (!entry.is_regular_file()) continue;
            if (entry.path().extension() != ".lua") continue;
            luaFiles.push_back(entry.path().lexically_normal().make_preferred().string());
        }
        if (luaFiles.empty()) return;

        const auto t0 = std::chrono::steady_clock::now();

        // Phase 1: parallel file read + mtime stat
        std::vector<FileSnapshot> snapshots(luaFiles.size());
        const size_t workerCount =
            std::max<size_t>(1, std::min(luaFiles.size(),
                                         static_cast<size_t>(std::max(1u, std::thread::hardware_concurrency()))));
        std::atomic<size_t> nextIx{0};
        std::vector<std::future<void>> jobs;
        jobs.reserve(workerCount);
        for (size_t w = 0; w < workerCount; ++w) {
            jobs.push_back(std::async(std::launch::async, [&]() {
                for (;;) {
                    size_t i = nextIx.fetch_add(1, std::memory_order_relaxed);
                    if (i >= luaFiles.size()) break;
                    const auto& filePath = luaFiles[i];
                    FileSnapshot snap{};
                    snap.path = filePath;
                    snap.mtime = ReadMtimeSeconds(filePath);
                    std::ifstream f(filePath, std::ios::binary);
                    if (f) {
                        snap.body.assign((std::istreambuf_iterator<char>(f)),
                                         std::istreambuf_iterator<char>());
                    } else {
                        LOG_WARN("ParseDirectory phase1: failed to read {}", filePath);
                    }
                    snap.appId = ParseAppIdFromLuaPath(std::filesystem::path(filePath));
                    snapshots[i] = std::move(snap);
                }
            }));
        }
        for (auto& j : jobs) j.get();
        // Keep deterministic ordering across runs after parallel phase-1 reads.
        std::sort(snapshots.begin(), snapshots.end(),
                  [](const FileSnapshot& a, const FileSnapshot& b) { return a.path < b.path; });
        const auto t1 = std::chrono::steady_clock::now();

        // Phase 2: serial luaL_loadbuffer + lua_pcall
        for (const auto& snap : snapshots) {
            UnloadFile_nolock(snap.path);

            ParseSession session;
            session.currentFile = snap.path;
            g_activeSession = &session;
            struct SessionGuard {
                ~SessionGuard() { g_activeSession = nullptr; }
            } guard;

            const std::filesystem::path path(snap.path);
            if (snap.appId) {
                if (OwnedAppIdSet.erase(*snap.appId)) {
                    LOG_PACKAGE_INFO("ParseDirectory: clearing owned status for appid={} (Lua re-added)",
                                     *snap.appId);
                }
                if (!DepotKeySet.count(*snap.appId)) {
                    DepotKeySet[*snap.appId] = "";
                    session.recordDepot(*snap.appId);
                }
                if (snap.mtime > 0) LuaMtimeMap[*snap.appId] = snap.mtime;
                LuaFilePathMap[*snap.appId] = snap.path;
            }

            const std::string chunkName = path.filename().string();
            lua_settop(g_lua_state, 0);
            int rc = luaL_loadbuffer(g_lua_state, snap.body.data(), snap.body.size(), chunkName.c_str());
            if (rc == LUA_OK) {
                if (lua_pcall(g_lua_state, 0, 0, 0) != LUA_OK) {
                    const char* err = lua_tostring(g_lua_state, -1);
                    LOG_WARN("{}: {}", chunkName, err ? err : "unknown");
                    lua_pop(g_lua_state, 1);
                }
            } else {
                const char* err = lua_tostring(g_lua_state, -1);
                LOG_WARN("{}: {}", chunkName, err ? err : "unknown");
                lua_pop(g_lua_state, 1);
            }
        }
        const auto t2 = std::chrono::steady_clock::now();

        // Phase 3: parallel AppConfig ensure/load + status publish
        std::vector<size_t> cfgIdx;
        cfgIdx.reserve(snapshots.size());
        for (size_t i = 0; i < snapshots.size(); ++i) {
            if (snapshots[i].appId) cfgIdx.push_back(i);
        }
        if (!cfgIdx.empty()) {
            const size_t cfgWorkers =
                std::max<size_t>(1, std::min(cfgIdx.size(), workerCount));
            std::atomic<size_t> cfgNext{0};
            std::vector<std::future<void>> cfgJobs;
            cfgJobs.reserve(cfgWorkers);
            for (size_t w = 0; w < cfgWorkers; ++w) {
                cfgJobs.push_back(std::async(std::launch::async, [&]() {
                    for (;;) {
                        size_t j = cfgNext.fetch_add(1, std::memory_order_relaxed);
                        if (j >= cfgIdx.size()) break;
                        const auto& snap = snapshots[cfgIdx[j]];
                        AppConfig::EnsureJsonc(snap.path, *snap.appId, "");
                        AppConfig::LoadJsonc(snap.path, *snap.appId, "");
                    }
                }));
            }
            for (auto& j : cfgJobs) j.get();
        }
        const auto t3 = std::chrono::steady_clock::now();

        LOG_PACKAGE_DEBUG("ParseDirectory phases: p1={}ms p2={}ms p3={}ms files={}",
                          std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count(),
                          std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count(),
                          std::chrono::duration_cast<std::chrono::milliseconds>(t3 - t2).count(),
                          snapshots.size());

        g_pendingAdditions.clear();
    }

    // ── startup injection ────────────────────────────────────────────────
    // Re-queue every loaded depot as a pending addition. RuntimeCapture
    // calls this after MarkLicenseAsChanged fires post-login so package 0
    // can absorb everything in one go via NotifyLicenseChanged.
    void QueueStartupInjection() {
        using namespace Internal;
        g_pendingAdditions.clear();
        g_pendingAdditions.reserve(DepotKeySet.size());
        for (const auto& [id, _] : DepotKeySet) {
            g_pendingAdditions.push_back(id);
        }
        LOG_PACKAGE_INFO("QueueStartupInjection: queued {} depot IDs for injection",
                         g_pendingAdditions.size());
    }
}
