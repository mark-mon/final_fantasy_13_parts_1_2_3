#pragma once
// Kernel object names for an XrBridge instance (see ipc.h).
#include <string>

namespace xrb {

// Returns false if the instance name is empty, too long or has characters other than [A-Za-z0-9_-].
inline bool ValidInstanceName(const char* n) {
    if (!n || !*n) return false;
    int len = 0;
    for (const char* p = n; *p; ++p, ++len) {
        char c = *p;
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok || len >= 31) return false;
    }
    return true;
}

inline std::wstring ObjName(const char* instance, const wchar_t* suffix) {
    std::wstring s = L"Local\\XrBridge_";
    for (const char* p = instance; *p; ++p) s += (wchar_t)*p;
    s += L"_v6";
    s += suffix;
    return s;
}
inline std::wstring ShmName(const char* instance) { return ObjName(instance, L""); }
inline std::wstring FrameEventName(const char* instance) { return ObjName(instance, L"_frame"); }
inline std::wstring HostEventName(const char* instance) { return ObjName(instance, L"_host"); }

} // namespace xrb
