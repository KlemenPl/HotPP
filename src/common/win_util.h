// Windows helpers shared by the agent and the host: UTF-8 <-> UTF-16 and
// command-line quoting.
#pragma once

#ifdef _WIN32

#include <windows.h>

#include <string>
#include <vector>

namespace hotpp::win {

inline std::string to_utf8(const wchar_t* s, int len = -1) {
    if (!s) return {};
    if (len < 0) len = int(wcslen(s));
    if (len == 0) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s, len, nullptr, 0, nullptr, nullptr);
    std::string out(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s, len, out.data(), n, nullptr, nullptr);
    return out;
}

inline std::wstring to_wide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring out(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), out.data(), n);
    return out;
}

// Quotes one argument so CommandLineToArgvW / the CRT parse it back unchanged.
inline std::wstring quote_arg(const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) return arg;
    std::wstring out = L"\"";
    for (auto it = arg.begin();; ++it) {
        size_t backslashes = 0;
        while (it != arg.end() && *it == L'\\') {
            ++it;
            ++backslashes;
        }
        if (it == arg.end()) {
            out.append(backslashes * 2, L'\\');
            break;
        }
        if (*it == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
        } else {
            out.append(backslashes, L'\\');
        }
        out.push_back(*it);
    }
    out.push_back(L'"');
    return out;
}

inline std::wstring command_line(const std::vector<std::string>& args) {
    std::wstring cmd;
    for (const std::string& a : args) {
        if (!cmd.empty()) cmd.push_back(L' ');
        cmd += quote_arg(to_wide(a));
    }
    return cmd;
}

} // namespace hotpp::win

#endif
