#pragma once

#include "plugin_interface.h"

// Forward declaration to access the global plugin self pointer
IPluginSelf* GetSelf();

// Convenience wrappers used by implementation files
inline IPluginHooks*   GetHooks()   { auto* s = GetSelf(); return s ? s->hooks   : nullptr; }
inline IPluginConfig*  GetConfig()  { auto* s = GetSelf(); return s ? s->config  : nullptr; }
inline IPluginScanner* GetScanner() { auto* s = GetSelf(); return s ? s->scanner : nullptr; }

// Convenience macros for logging.
//
// The do/while (0) wrapper is what makes these safe as the body of an
// unbraced if/else. Without it the macro's own `if` swallows a following
// `else`, so
//
//     if (!hook)
//         LOG_WARN("failed to install");
//     else
//         LOG_INFO("installed");
//
// binds the else to `if (auto s = GetSelf())` and prints the success line
// only when there is no logger to print it with -- i.e. never. The failure
// branch still works, which is what makes it easy to miss.
#define LOG_TRACE(format, ...) do { if (auto s = GetSelf()) s->logger->Trace(s, format, ##__VA_ARGS__); } while (0)
#define LOG_DEBUG(format, ...) do { if (auto s = GetSelf()) s->logger->Debug(s, format, ##__VA_ARGS__); } while (0)
#define LOG_INFO(format, ...)  do { if (auto s = GetSelf()) s->logger->Info (s, format, ##__VA_ARGS__); } while (0)
#define LOG_WARN(format, ...)  do { if (auto s = GetSelf()) s->logger->Warn (s, format, ##__VA_ARGS__); } while (0)
#define LOG_ERROR(format, ...) do { if (auto s = GetSelf()) s->logger->Error(s, format, ##__VA_ARGS__); } while (0)
