// Utf8.h
// Мелкие помощники для конвертации между UTF-16 (Windows-пути, WinAPI) и
// UTF-8 (JSON-сообщения в WebView2, содержимое .ovpn-конфигов).

#pragma once

#include <windows.h>
#include <string>

namespace tvpn {

inline std::string WideToUtf8(const std::wstring& wide) {
    if (wide.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, wide.data(), (int)wide.size(), nullptr, 0, nullptr, nullptr);
    std::string out(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), (int)wide.size(), out.data(), len, nullptr, nullptr);
    return out;
}

inline std::wstring Utf8ToWide(const std::string& utf8) {
    if (utf8.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), nullptr, 0);
    std::wstring out(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), out.data(), len);
    return out;
}

}  // namespace tvpn
