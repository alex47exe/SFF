// LumaCore — Steam client hook layer for SteaMidra.
// Copyright (c) 2025-2026 Midrag (https://github.com/Midrags).
// Distributed under the GNU General Public License v3 or later.
// See <https://www.gnu.org/licenses/> for the full license text.

#include "AppConfig.h"

#include "HookStatus.h"
#include "Logger.h"

#include <filesystem>
#include <fstream>
#include <cctype>
#include <mutex>
#include <regex>
#include <string>
#include <unordered_map>

namespace AppConfig {
    namespace {
        struct AppCfg {
            bool onlinefix = false;
            bool allowUpdate = false;
            std::string luaPath;
            std::string jsoncPath;
            std::string gameName;
        };

        std::mutex g_mu;
        std::unordered_map<AppId_t, AppCfg> g_cfg;

        std::string ReadAll(const std::string& path) {
            std::ifstream f(path, std::ios::binary);
            if (!f) return {};
            std::string s((std::istreambuf_iterator<char>(f)),
                          std::istreambuf_iterator<char>());
            return s;
        }

        std::string StripJsoncComments(std::string in) {
            static const std::regex kLineComment(R"(//[^\r\n]*)");
            static const std::regex kBlockComment(R"(/\*[\s\S]*?\*/)");
            in = std::regex_replace(in, kLineComment, "");
            in = std::regex_replace(in, kBlockComment, "");
            return in;
        }

        bool ReadBoolKey(const std::string& json, const char* key, bool fallback) {
            const std::string needle = std::string("\"") + key + "\"";
            size_t pos = json.find(needle);
            if (pos == std::string::npos) return fallback;
            pos = json.find(':', pos + needle.size());
            if (pos == std::string::npos) return fallback;
            ++pos;
            while (pos < json.size()
                   && (json[pos] == ' ' || json[pos] == '\t'
                       || json[pos] == '\r' || json[pos] == '\n')) {
                ++pos;
            }
            if (pos + 4 <= json.size()) {
                std::string token = json.substr(pos, 5);
                for (char& c : token) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
                if (token.rfind("true", 0) == 0) return true;
                if (token.rfind("false", 0) == 0) return false;
            }
            return fallback;
        }

        std::string JsonEscape(const std::string& s) {
            std::string out;
            out.reserve(s.size() + 8);
            for (char ch : s) {
                switch (ch) {
                    case '\\': out += "\\\\"; break;
                    case '"':  out += "\\\""; break;
                    case '\n': out += "\\n"; break;
                    case '\r': out += "\\r"; break;
                    case '\t': out += "\\t"; break;
                    default:   out += ch; break;
                }
            }
            return out;
        }
    }

    std::string JsoncPathFor(const std::string& luaFilePath) {
        namespace fs = std::filesystem;
        fs::path p(luaFilePath);
        return (p.parent_path() / (p.stem().string() + ".jsonc")).string();
    }

    void EnsureJsonc(const std::string& luaFilePath, AppId_t appId, const std::string& gameName) {
        namespace fs = std::filesystem;
        const std::string jsoncPath = JsoncPathFor(luaFilePath);
        std::error_code ec;
        if (fs::exists(jsoncPath, ec)) return;

        std::ofstream f(jsoncPath, std::ios::binary | std::ios::trunc);
        if (!f) {
            LOG_WARN("AppConfig: failed to create {}", jsoncPath);
            return;
        }
        f << "{\n";
        f << "  \"appid\": " << appId << ",\n";
        f << "  \"game_name\": \"" << JsonEscape(gameName) << "\",\n";
        f << "  \"onlinefix\": false,\n";
        f << "  \"allow_update\": false\n";
        f << "}\n";
    }

    void LoadJsonc(const std::string& luaFilePath, AppId_t appId, const std::string& gameName) {
        const std::string jsoncPath = JsoncPathFor(luaFilePath);
        std::string body = StripJsoncComments(ReadAll(jsoncPath));

        AppCfg cfg{};
        cfg.luaPath = luaFilePath;
        cfg.jsoncPath = jsoncPath;
        cfg.gameName = gameName;
        if (!body.empty()) {
            cfg.onlinefix = ReadBoolKey(body, "onlinefix", false);
            cfg.allowUpdate = ReadBoolKey(body, "allow_update", false);
        }

        {
            std::lock_guard<std::mutex> lk(g_mu);
            g_cfg[appId] = cfg;
        }

        HookStatus::AppInfo info{};
        info.appId = appId;
        info.gameName = gameName;
        info.luaPath = cfg.luaPath;
        info.jsoncPath = cfg.jsoncPath;
        info.onlinefix = cfg.onlinefix;
        info.allowUpdate = cfg.allowUpdate;
        info.manifestMode = cfg.allowUpdate ? "latest" : "pinned";
        HookStatus::PublishApp(info);
    }

    void Unload(AppId_t appId) {
        {
            std::lock_guard<std::mutex> lk(g_mu);
            g_cfg.erase(appId);
        }
        HookStatus::UnpublishApp(appId);
    }

    bool IsOnlineFixEnabled(AppId_t appId) {
        std::lock_guard<std::mutex> lk(g_mu);
        auto it = g_cfg.find(appId);
        return it != g_cfg.end() && it->second.onlinefix;
    }

    bool IsAllowUpdateEnabled(AppId_t appId) {
        std::lock_guard<std::mutex> lk(g_mu);
        auto it = g_cfg.find(appId);
        return it != g_cfg.end() && it->second.allowUpdate;
    }
}
