// Inert SFX lifecycle probe: writes only its externally provided marker, then exits.
#include <windows.h>
#include <string>
static std::string utf8(const wchar_t *text) {
    int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, &result[0], size, nullptr, nullptr);
    result.resize(size - 1);
    return result;
}
int main() {
    wchar_t marker[32768], cwd[32768], executable[32768];
    if (!GetEnvironmentVariableW(L"ORANGE_PROBE_MARKER", marker, 32768)) return 10;
    if (!GetCurrentDirectoryW(32768, cwd)) return 11;
    if (!GetModuleFileNameW(nullptr, executable, 32768)) return 12;
    HANDLE file = CreateFileW(marker, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return 13;
    std::string content = utf8(cwd) + "\n" + utf8(executable) + "\n";
    DWORD written;
    bool success = WriteFile(file, content.data(), DWORD(content.size()), &written, nullptr) && written == content.size();
    CloseHandle(file);
    if (!success) return 14;
    Sleep(1800);
    return 0;
}
