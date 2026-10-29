#include "config/Config.h"
#include "common/StringUtils.h"

#include <windows.h>
#include <shlobj.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace pms {

namespace {

bool FileExistsW(const std::wstring& path) {
    DWORD attr = GetFileAttributesW(path.c_str());
    return (attr != INVALID_FILE_ATTRIBUTES) && ((attr & FILE_ATTRIBUTE_DIRECTORY) == 0);
}

std::wstring GetExeDirectory() {
    wchar_t buf[MAX_PATH * 2] = {};
    DWORD len = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    std::wstring full(buf, len);
    size_t slash = full.find_last_of(L"\\/");
    return (slash != std::wstring::npos) ? full.substr(0, slash) : L".";
}

std::wstring GetExeFullPath() {
    wchar_t buf[MAX_PATH * 2] = {};
    DWORD len = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    return std::wstring(buf, len);
}

std::string EscapeJsonString(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '\"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    char hex[8];
                    std::snprintf(hex, sizeof(hex), "\\u%04x", static_cast<unsigned int>(c));
                    out += hex;
                } else {
                    out.push_back(static_cast<char>(c));
                }
                break;
        }
    }
    return out;
}

// Lightweight recursive-descent JSON reader tailored for AppConfig
class JsonReader {
public:
    explicit JsonReader(const std::string& src) : s_(src) {}

    void SkipWs() {
        while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\r' || s_[pos_] == '\n')) {
            ++pos_;
        }
    }

    bool Consume(char expected) {
        SkipWs();
        if (pos_ < s_.size() && s_[pos_] == expected) {
            ++pos_;
            return true;
        }
        return false;
    }

    char Peek() {
        SkipWs();
        return (pos_ < s_.size()) ? s_[pos_] : '\0';
    }

    bool ParseString(std::string& out) {
        SkipWs();
        if (pos_ >= s_.size() || s_[pos_] != '\"') return false;
        ++pos_;
        out.clear();
        while (pos_ < s_.size()) {
            char c = s_[pos_++];
            if (c == '\"') return true;
            if (c == '\\') {
                if (pos_ >= s_.size()) return false;
                char esc = s_[pos_++];
                switch (esc) {
                    case '\"': out.push_back('\"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/':  out.push_back('/');  break;
                    case 'b':  out.push_back('\b'); break;
                    case 'f':  out.push_back('\f'); break;
                    case 'n':  out.push_back('\n'); break;
                    case 'r':  out.push_back('\r'); break;
                    case 't':  out.push_back('\t'); break;
                    case 'u': {
                        if (pos_ + 4 > s_.size()) return false;
                        unsigned int code = 0;
                        for (int i = 0; i < 4; ++i) {
                            char h = s_[pos_++];
                            code <<= 4;
                            if (h >= '0' && h <= '9') code |= static_cast<unsigned int>(h - '0');
                            else if (h >= 'a' && h <= 'f') code |= static_cast<unsigned int>(h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') code |= static_cast<unsigned int>(h - 'A' + 10);
                            else return false;
                        }
                        if (code <= 0x7F) {
                            out.push_back(static_cast<char>(code));
                        } else if (code <= 0x7FF) {
                            out.push_back(static_cast<char>(0xC0 | ((code >> 6) & 0x1F)));
                            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                        } else {
                            out.push_back(static_cast<char>(0xE0 | ((code >> 12) & 0x0F)));
                            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
                            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                        }
                        break;
                    }
                    default: return false;
                }
            } else {
                out.push_back(c);
            }
        }
        return false;
    }

    bool ParseBool(bool& out) {
        SkipWs();
        if (s_.compare(pos_, 4, "true") == 0) {
            pos_ += 4;
            out = true;
            return true;
        }
        if (s_.compare(pos_, 5, "false") == 0) {
            pos_ += 5;
            out = false;
            return true;
        }
        return false;
    }

    bool ParseInt(int& out) {
        SkipWs();
        if (pos_ >= s_.size()) return false;
        const char* start = s_.c_str() + pos_;
        char* endPtr = nullptr;
        long val = std::strtol(start, &endPtr, 10);
        if (endPtr == start) return false;
        pos_ += static_cast<size_t>(endPtr - start);
        // Skip optional fractional part if someone hand-edited float
        if (pos_ < s_.size() && s_[pos_] == '.') {
            ++pos_;
            while (pos_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[pos_]))) ++pos_;
        }
        out = static_cast<int>(val);
        return true;
    }

    bool SkipValue() {
        char c = Peek();
        if (c == '\"') {
            std::string dummy;
            return ParseString(dummy);
        }
        if (c == '{') {
            Consume('{');
            if (Consume('}')) return true;
            while (true) {
                std::string k;
                if (!ParseString(k)) return false;
                if (!Consume(':')) return false;
                if (!SkipValue()) return false;
                if (Consume('}')) return true;
                if (!Consume(',')) return false;
            }
        }
        if (c == '[') {
            Consume('[');
            if (Consume(']')) return true;
            while (true) {
                if (!SkipValue()) return false;
                if (Consume(']')) return true;
                if (!Consume(',')) return false;
            }
        }
        if (c == 't' || c == 'f') {
            bool b = false;
            return ParseBool(b);
        }
        if (c == 'n' && s_.compare(pos_, 4, "null") == 0) {
            pos_ += 4;
            return true;
        }
        if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) {
            int dummy = 0;
            return ParseInt(dummy);
        }
        return false;
    }

private:
    const std::string& s_;
    size_t pos_ = 0;
};

} // namespace

std::wstring ConfigManager::ResolveConfigPath(bool forcePortable) {
    std::wstring exeDir = GetExeDirectory();
    std::wstring portableCfg = exeDir + L"\\settings.json";
    std::wstring portableMarker = exeDir + L"\\portable.txt";

    if (forcePortable || FileExistsW(portableCfg) || FileExistsW(portableMarker)) {
        return portableCfg;
    }

    PWSTR appDataPath = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appDataPath)) && appDataPath) {
        std::wstring dir = std::wstring(appDataPath) + L"\\PerMonSharp";
        CoTaskMemFree(appDataPath);
        CreateDirectoryW(dir.c_str(), nullptr);
        return dir + L"\\settings.json";
    }

    return portableCfg;
}

std::string ConfigManager::SerializeToJson(const AppConfig& config) {
    std::ostringstream os;
    os << "{\n";
    os << "  \"version\": 1,\n";
    os << "  \"masterEnabled\": " << (config.masterEnabled ? "true" : "false") << ",\n";
    os << "  \"pauseOnFullscreen\": " << (config.pauseOnFullscreen ? "true" : "false") << ",\n";
    os << "  \"minimizeToTrayOnClose\": " << (config.minimizeToTrayOnClose ? "true" : "false") << ",\n";
    os << "  \"startWithWindows\": " << (config.startWithWindows ? "true" : "false") << ",\n";
    os << "  \"enableGlobalHotkey\": " << (config.enableGlobalHotkey ? "true" : "false") << ",\n";
    os << "  \"lastSelectedMonitorId\": \"" << EscapeJsonString(config.lastSelectedMonitorId) << "\",\n";
    os << "  \"monitors\": [";
    for (size_t i = 0; i < config.monitors.size(); ++i) {
        const auto& m = config.monitors[i];
        int clampedSharp = std::clamp(m.sharpness, kMinSharpnessPercent, kMaxSharpnessPercent);
        os << (i == 0 ? "\n" : ",\n");
        os << "    {\n";
        os << "      \"id\": \"" << EscapeJsonString(m.monitorId) << "\",\n";
        os << "      \"name\": \"" << EscapeJsonString(m.lastKnownName) << "\",\n";
        os << "      \"enabled\": " << (m.enabled ? "true" : "false") << ",\n";
        os << "      \"sharpness\": " << clampedSharp << "\n";
        os << "    }";
    }
    if (!config.monitors.empty()) os << "\n  ";
    os << "]\n";
    os << "}\n";
    return os.str();
}

bool ConfigManager::ParseFromJson(const std::string& jsonText, AppConfig& outConfig) {
    JsonReader r(jsonText);
    if (!r.Consume('{')) return false;

    AppConfig parsed{};
    if (r.Consume('}')) {
        outConfig = parsed;
        return true;
    }

    while (true) {
        std::string key;
        if (!r.ParseString(key)) return false;
        if (!r.Consume(':')) return false;

        if (key == "masterEnabled") {
            if (!r.ParseBool(parsed.masterEnabled)) return false;
        } else if (key == "pauseOnFullscreen") {
            if (!r.ParseBool(parsed.pauseOnFullscreen)) return false;
        } else if (key == "minimizeToTrayOnClose") {
            if (!r.ParseBool(parsed.minimizeToTrayOnClose)) return false;
        } else if (key == "startWithWindows") {
            if (!r.ParseBool(parsed.startWithWindows)) return false;
        } else if (key == "enableGlobalHotkey") {
            if (!r.ParseBool(parsed.enableGlobalHotkey)) return false;
        } else if (key == "lastSelectedMonitorId") {
            if (!r.ParseString(parsed.lastSelectedMonitorId)) return false;
        } else if (key == "monitors") {
            if (!r.Consume('[')) return false;
            if (!r.Consume(']')) {
                while (true) {
                    if (!r.Consume('{')) return false;
                    MonitorConfig mc{};
                    if (!r.Consume('}')) {
                        while (true) {
                            std::string mk;
                            if (!r.ParseString(mk)) return false;
                            if (!r.Consume(':')) return false;
                            if (mk == "id") {
                                if (!r.ParseString(mc.monitorId)) return false;
                            } else if (mk == "name") {
                                if (!r.ParseString(mc.lastKnownName)) return false;
                            } else if (mk == "enabled") {
                                if (!r.ParseBool(mc.enabled)) return false;
                            } else if (mk == "sharpness") {
                                if (!r.ParseInt(mc.sharpness)) return false;
                                mc.sharpness = std::clamp(mc.sharpness, kMinSharpnessPercent, kMaxSharpnessPercent);
                            } else {
                                if (!r.SkipValue()) return false;
                            }
                            if (r.Consume('}')) break;
                            if (!r.Consume(',')) return false;
                        }
                    }
                    if (!mc.monitorId.empty()) {
                        parsed.monitors.push_back(mc);
                    }
                    if (r.Consume(']')) break;
                    if (!r.Consume(',')) return false;
                }
            }
        } else {
            if (!r.SkipValue()) return false;
        }

        if (r.Consume('}')) break;
        if (!r.Consume(',')) return false;
    }

    outConfig = std::move(parsed);
    return true;
}

bool ConfigManager::LoadFromFile(const std::wstring& path, AppConfig& outConfig) {
    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(hFile, &sz) || sz.QuadPart <= 0 || sz.QuadPart > (4 * 1024 * 1024)) {
        CloseHandle(hFile);
        return false;
    }

    std::string content(static_cast<size_t>(sz.QuadPart), '\0');
    DWORD bytesRead = 0;
    BOOL ok = ReadFile(hFile, content.data(), static_cast<DWORD>(content.size()), &bytesRead, nullptr);
    CloseHandle(hFile);
    if (!ok || bytesRead != content.size()) return false;

    // Strip UTF-8 BOM if present
    if (content.size() >= 3 &&
        static_cast<unsigned char>(content[0]) == 0xEF &&
        static_cast<unsigned char>(content[1]) == 0xBB &&
        static_cast<unsigned char>(content[2]) == 0xBF) {
        content.erase(0, 3);
    }

    return ParseFromJson(content, outConfig);
}

bool ConfigManager::SaveToFile(const std::wstring& path, const AppConfig& config) {
    size_t slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        std::wstring dir = path.substr(0, slash);
        CreateDirectoryW(dir.c_str(), nullptr);
    }

    std::string json = SerializeToJson(config);
    std::wstring tmpPath = path + L".tmp";

    HANDLE hFile = CreateFileW(tmpPath.c_str(), GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;

    DWORD written = 0;
    BOOL ok = WriteFile(hFile, json.data(), static_cast<DWORD>(json.size()), &written, nullptr);
    FlushFileBuffers(hFile);
    CloseHandle(hFile);

    if (!ok || written != json.size()) {
        DeleteFileW(tmpPath.c_str());
        return false;
    }

    if (!MoveFileExW(tmpPath.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmpPath.c_str());
        return false;
    }
    return true;
}

static constexpr wchar_t kRunRegPath[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

bool ConfigManager::IsStartupRegistryEnabled() {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunRegPath, 0, KEY_READ, &hKey) != ERROR_SUCCESS) {
        return false;
    }
    wchar_t val[MAX_PATH * 2] = {};
    DWORD cb = sizeof(val);
    DWORD type = 0;
    LSTATUS st = RegQueryValueExW(hKey, kAppName, nullptr, &type, reinterpret_cast<LPBYTE>(val), &cb);
    RegCloseKey(hKey);
    return (st == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ) && val[0] != L'\0');
}

bool ConfigManager::SetStartupRegistryEnabled(bool enable) {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunRegPath, 0, KEY_SET_VALUE, &hKey) != ERROR_SUCCESS) {
        return false;
    }
    LSTATUS st = ERROR_SUCCESS;
    if (enable) {
        std::wstring cmd = L"\"" + GetExeFullPath() + L"\" --minimized";
        DWORD cb = static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t));
        st = RegSetValueExW(hKey, kAppName, 0, REG_SZ, reinterpret_cast<const BYTE*>(cmd.c_str()), cb);
    } else {
        st = RegDeleteValueW(hKey, kAppName);
        if (st == ERROR_FILE_NOT_FOUND) st = ERROR_SUCCESS;
    }
    RegCloseKey(hKey);
    return (st == ERROR_SUCCESS);
}

} // namespace pms
