#ifndef ORANGE_SENSITIVESTRINGS_H
#define ORANGE_SENSITIVESTRINGS_H
#include "encodedstring.h"

// Audited inventory: each entry is scanned as UTF-8 and UTF-16LE in the final EXE.
// Only private verification/diagnostic text; no UI, protocol or Qt metadata.
namespace OrangeSecrets {
ORANGE_SENSITIVE_STRING(defaultPassword, "123456...")
ORANGE_SENSITIVE_STRING(passwordAccepted, "密码验证成功，启动主界面")
ORANGE_SENSITIVE_STRING(passwordRejected, "用户取消或密码验证失败，程序退出")
ORANGE_SENSITIVE_STRING(integrityStarted, "开始程序完整性验证...")
ORANGE_SENSITIVE_STRING(integrityAccepted, "程序完整性验证通过")
ORANGE_SENSITIVE_STRING(debuggerWarning, "警告：检测到调试器")
}
#undef ORANGE_SENSITIVE_STRING
#endif
