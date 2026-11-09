// Windows helpers shared by the host and the agent: UTF-8 <-> UTF-16 and error text.
// HotPP keeps strings in UTF-8 and converts at the Win32 API boundary.
#pragma once

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <string>
#include <vector>

namespace hotpp::win {

inline std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

inline std::string narrow(const wchar_t* w, size_t len) {
    if (!w || len == 0) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, int(len), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w, int(len), s.data(), n, nullptr, nullptr);
    return s;
}

inline std::string narrow(const std::wstring& w) { return narrow(w.data(), w.size()); }

// "what: <system message> (error N)".
inline std::string error_text(const char* what, DWORD err = GetLastError()) {
    wchar_t* buf = nullptr;
    const DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                   nullptr, err, 0, reinterpret_cast<wchar_t*>(&buf), 0, nullptr);
    std::string msg = n ? narrow(buf, n) : std::string();
    if (buf) LocalFree(buf);
    while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r' || msg.back() == '.' || msg.back() == ' '))
        msg.pop_back();
    return std::string(what) + ": " + (msg.empty() ? "error" : msg) + " (error " + std::to_string(err) + ")";
}

// One argument quoted so CommandLineToArgvW (and the MSVC runtime) read it back
// unchanged.
inline void append_quoted(std::wstring& cmd, const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        cmd += arg;
        return;
    }
    cmd += L'"';
    for (size_t i = 0;; ++i) {
        size_t backslashes = 0;
        while (i < arg.size() && arg[i] == L'\\') ++i, ++backslashes;
        if (i == arg.size()) {
            cmd.append(backslashes * 2, L'\\');
            break;
        }
        if (arg[i] == L'"') {
            cmd.append(backslashes * 2 + 1, L'\\');
            cmd += L'"';
        } else {
            cmd.append(backslashes, L'\\');
            cmd += arg[i];
        }
    }
    cmd += L'"';
}

inline std::wstring command_line(const std::vector<std::string>& argv) {
    std::wstring cmd;
    for (size_t i = 0; i < argv.size(); ++i) {
        if (i) cmd += L' ';
        append_quoted(cmd, widen(argv[i]));
    }
    return cmd;
}

} // namespace hotpp::win

#endif // _WIN32
