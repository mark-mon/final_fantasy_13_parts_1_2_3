#include "config.h"
#include "log.h"
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <windows.h>

namespace xrb {

static std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

static std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)tolower(c); });
    return s;
}

bool Config::Load(const wchar_t* path) {
    FILE* f = _wfopen(path, L"r");
    if (!f) return false;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        std::string l = line;
        size_t c = l.find_first_of("#;");
        if (c != std::string::npos) l = l.substr(0, c);
        size_t eq = l.find('=');
        if (eq == std::string::npos) continue;
        std::string k = Lower(Trim(l.substr(0, eq)));
        std::string v = Trim(l.substr(eq + 1));
        if (!k.empty()) m_[k] = v;
    }
    fclose(f);
    return true;
}

std::string Config::GetStr(const char* key, const char* def) const {
    auto it = m_.find(Lower(key));
    return it == m_.end() ? std::string(def) : it->second;
}
int Config::GetInt(const char* key, int def) const {
    auto it = m_.find(Lower(key));
    return it == m_.end() ? def : (int)strtol(it->second.c_str(), nullptr, 0);
}
float Config::GetFloat(const char* key, float def) const {
    auto it = m_.find(Lower(key));
    return it == m_.end() ? def : (float)atof(it->second.c_str());
}
bool Config::GetBool(const char* key, bool def) const {
    auto it = m_.find(Lower(key));
    if (it == m_.end()) return def;
    std::string v = Lower(it->second);
    return v == "1" || v == "true" || v == "yes" || v == "on";
}
void Config::Dump() const {
    for (auto& kv : m_) LOGI("config: %s = %s", kv.first.c_str(), kv.second.c_str());
}

void WriteDefaultConfig(const wchar_t* path, const char* contents) {
    if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) return;
    FILE* f = _wfopen(path, L"w");
    if (!f) return;
    fputs(contents, f);
    fclose(f);
}

} // namespace xrb
