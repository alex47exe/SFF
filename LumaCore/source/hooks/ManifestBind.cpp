// LumaCore - Steam client hook layer for SteaMidra.
// Copyright (c) 2025-2026 Midrag (https://github.com/Midrags).
// Distributed under the GNU General Public License v3 or later.
// See <https://www.gnu.org/licenses/> for the full license text.

#include "ManifestBind.h"
#include "Macros.h"
#include "entry.h"
#include "utils/AppConfig.h"
#include "utils/HookStatus.h"
#include <format>
#include <filesystem>
#include <fstream>
#include <atomic>
#include <exception>
#include <mutex>
#include <regex>
#include <string>
#include <thread>
#include <vector>

// ▌▌ LumaCore ▌ MANIFEST ▌ Manifest override hook
//  BuildDepotDependency patches depot entries' gid/size directly in the
//  output vector after Steam builds the depot list.
//
//  DLC observation hooks (BIsDlcEnabled / IsAppDlcInstalled /
//  IsCloudEnabledForApp) were intentionally NOT hooked. Steam already
//  returns the right answer for Lua-tracked appids through the existing
//  CheckAppOwnership patch, so adding those would be redundant and would
//  re-introduce wrong-target risk on builds where their TOML rva drifts.
// ▌▌
namespace {

    struct PendingRewrite {
        AppId_t appId = 0;
        uint64_t depotId = 0;
        uint64_t newGid = 0;
    };

    std::mutex g_pendingMu;
    std::vector<PendingRewrite> g_pending;
    std::atomic<bool> g_flushRunning{false};

    static bool RewriteLuaManifestGid(const std::string& luaPath, uint64_t depotId, uint64_t gid) {
        std::ifstream in(luaPath, std::ios::binary);
        if (!in) {
            LOG_MANIFESTCH_WARN("RewriteLuaManifestGid: failed to open {}", luaPath);
            return false;
        }
        std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        in.close();
        if (body.empty()) return false;

        const std::string pat =
            "setManifestid\\s*\\(\\s*" + std::to_string(depotId) + "\\s*,\\s*\"[0-9]+\"";
        std::regex re(pat);
        std::string replacement =
            "setManifestid(" + std::to_string(depotId) + ", \"" + std::to_string(gid) + "\"";
        std::string updated = std::regex_replace(body, re, replacement,
                                                 std::regex_constants::format_first_only);
        if (updated == body) return false;

        std::ofstream out(luaPath, std::ios::binary | std::ios::trunc);
        if (!out) {
            LOG_MANIFESTCH_WARN("RewriteLuaManifestGid: failed to open for write {}", luaPath);
            return false;
        }
        out.write(updated.data(), static_cast<std::streamsize>(updated.size()));
        if (!out) {
            LOG_MANIFESTCH_WARN("RewriteLuaManifestGid: write failed {}", luaPath);
            return false;
        }
        return true;
    }

    // ▌ MANIFEST ▌ helper

    std::string DepotStr(const DepotEntry& e) {
        return std::format("[DepotId={} | AppId={} | Gid={} | Size={} | Dlc={} | Lcs={} | Carry={} | Shared={}]",
            e.DepotId, e.AppId, e.ManifestGid, e.ManifestSize, e.DlcAppId,
            (int)e.LcsRequired, (int)e.bNotNewTarget, (int)e.SharedInstall);
    }

    // ▌ MANIFEST ▌ BuildDepotDependency hook
    // After Steam builds the depot list for an app, patch ManifestGid
    // and ManifestSize for any depots we have overrides for.

    LC_HOOK_DEF(BuildDepotDependency, bool, void* pUserAppMgr, AppId_t AppId,
              void* pUserConfig, CUtlVector<DepotEntry>* pDepotInfo,
              CUtlVector<DepotEntry>* pSharedDepotInfo, void* pSteamApp,
              uint32* pBuildId, bool* pbBetaFallback)
    {
        bool outcome = oBuildDepotDependency(pUserAppMgr, AppId, pUserConfig,
            pDepotInfo, pSharedDepotInfo, pSteamApp, pBuildId, pbBetaFallback);

        LOG_MANIFESTCH_TRACE("BuildDepotDependency: AppId={} pUserConfig=0x{:X} result={} pSteamApp=0x{:X} pBuildId={} pbBetaFallback={}",
            AppId, (uintptr_t)pUserConfig, outcome, (uintptr_t)pSteamApp,
            pBuildId ? *pBuildId : 0, pbBetaFallback ? *pbBetaFallback : false);
        if (pDepotInfo) {
            LOG_MANIFESTCH_TRACE("pDepotInfo->nCount={}", pDepotInfo->m_Size);
            const DepotEntry* dBase = pDepotInfo->m_Memory.m_pMemory;
            for (uint32 n = 0; n < pDepotInfo->m_Size; ++n)
                LOG_MANIFESTCH_TRACE("  [{}] {}", n, DepotStr(dBase[n]));
        }
        if (pSharedDepotInfo) {
            LOG_MANIFESTCH_TRACE("pSharedDepotInfo->nCount={}", pSharedDepotInfo->m_Size);
            const DepotEntry* sBase = pSharedDepotInfo->m_Memory.m_pMemory;
            for (uint32 n = 0; n < pSharedDepotInfo->m_Size; ++n)
                LOG_MANIFESTCH_TRACE("  shared[{}] {}", n, DepotStr(sBase[n]));
        }

        if (!outcome) return outcome;

        const auto& overrides = LuaLoader::GetManifestOverrides();
        if (overrides.empty()) return outcome;

        if (pDepotInfo && pDepotInfo->m_Size) {
            DepotEntry* pBegin = pDepotInfo->m_Memory.m_pMemory;
            DepotEntry* pEnd   = pBegin + pDepotInfo->m_Size;
            for (DepotEntry* ep = pBegin; ep != pEnd; ++ep) {
                auto it = overrides.find(ep->DepotId);
                if (it != overrides.end()) {
                    if (AppConfig::IsAllowUpdateEnabled(ep->AppId)) {
                        if (ep->ManifestGid != it->second.gid) {
                            std::lock_guard<std::mutex> lk(g_pendingMu);
                            g_pending.push_back(PendingRewrite{
                                ep->AppId, ep->DepotId, ep->ManifestGid
                            });
                            LOG_MANIFESTCH_INFO("BuildDepotDependency: queued latest gid update app={} depot={} {}->{}",
                                                ep->AppId, ep->DepotId, it->second.gid, ep->ManifestGid);
                        }
                        continue;
                    }
                    // if size=0 in the override, keep the original size(affects download display but not the actual download)
                    uint64_t newSize = it->second.size ? it->second.size : ep->ManifestSize;
                    LOG_MANIFESTCH_INFO("BuildDepotDependency: patching depot {} gid={}->{} size={}->{}",
                        ep->DepotId, ep->ManifestGid, it->second.gid,
                        ep->ManifestSize, newSize);
                    ep->ManifestGid  = it->second.gid;
                    ep->ManifestSize = newSize;
                }
            }
        }
        return outcome;
    }

} // anonymous namespace

namespace ManifestBind {

    void Install() {
        LC_TX_OPEN();
        LC_ATTACH_D(BuildDepotDependency);
        LC_TX_COMMIT();
    }

    void Uninstall() {
        LC_TX_OPEN();
        LC_DETACH(BuildDepotDependency);
        LC_TX_COMMIT();
    }

    void FlushPending() {
        std::vector<PendingRewrite> items;
        {
            std::lock_guard<std::mutex> lk(g_pendingMu);
            if (g_pending.empty()) return;
            items.swap(g_pending);
        }

        bool expected = false;
        if (!g_flushRunning.compare_exchange_strong(expected, true)) {
            std::lock_guard<std::mutex> lk(g_pendingMu);
            g_pending.insert(g_pending.end(), items.begin(), items.end());
            return;
        }

        std::thread([items = std::move(items)]() mutable {
            try {
                for (const auto& it : items) {
                    std::string luaPath = LuaLoader::GetLuaFilePath(it.appId);
                    if (luaPath.empty()) continue;
                    if (RewriteLuaManifestGid(luaPath, it.depotId, it.newGid)) {
                        LOG_MANIFESTCH_INFO("FlushPending: updated lua gid app={} depot={} gid={}",
                                            it.appId, it.depotId, it.newGid);
                        AppConfig::LoadJsonc(luaPath, it.appId, "");
                        HookStatus::AppInfo app{};
                        app.appId = it.appId;
                        app.luaPath = luaPath;
                        app.jsoncPath = AppConfig::JsoncPathFor(luaPath);
                        app.allowUpdate = AppConfig::IsAllowUpdateEnabled(it.appId);
                        app.onlinefix = AppConfig::IsOnlineFixEnabled(it.appId);
                        app.manifestMode = app.allowUpdate ? "latest" : "pinned";
                        app.manifestGid = it.newGid;
                        HookStatus::PublishApp(app);
                    }
                }
            } catch (const std::exception& e) {
                LOG_MANIFESTCH_WARN("FlushPending worker exception: {}", e.what());
            } catch (...) {
                LOG_MANIFESTCH_WARN("FlushPending worker exception: unknown");
            }
            g_flushRunning.store(false, std::memory_order_release);
        }).detach();
    }
}
