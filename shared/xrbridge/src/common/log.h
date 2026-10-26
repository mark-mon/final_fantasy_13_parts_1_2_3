#pragma once
#include <windows.h>
#include <cstdarg>

namespace xrb {

enum LogLevel { LOG_ERROR = 0, LOG_WARN = 1, LOG_INFO = 2, LOG_DEBUG = 3, LOG_TRACE = 4 };

void LogInit(const wchar_t* path, int level);
void LogSetLevel(int level);
int  LogGetLevel();
void LogWrite(int level, const char* fmt, ...);
void LogFlush();

} // namespace xrb

#define LOGE(...) ::xrb::LogWrite(::xrb::LOG_ERROR, __VA_ARGS__)
#define LOGW(...) ::xrb::LogWrite(::xrb::LOG_WARN, __VA_ARGS__)
#define LOGI(...) ::xrb::LogWrite(::xrb::LOG_INFO, __VA_ARGS__)
#define LOGD(...) do { if (::xrb::LogGetLevel() >= ::xrb::LOG_DEBUG) ::xrb::LogWrite(::xrb::LOG_DEBUG, __VA_ARGS__); } while (0)
#define LOGT(...) do { if (::xrb::LogGetLevel() >= ::xrb::LOG_TRACE) ::xrb::LogWrite(::xrb::LOG_TRACE, __VA_ARGS__); } while (0)
