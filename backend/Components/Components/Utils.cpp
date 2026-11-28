#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdint>
#include <cstdio>

#include "Utils.hpp"

std::string Utils::WideToUtf8(const wchar_t* Text)
{
    if (!Text || !*Text)
        return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, Text, -1, nullptr, 0, nullptr, nullptr);
    std::string out(size > 0 ? size - 1 : 0, '\0');
    if (size > 1)
        WideCharToMultiByte(CP_UTF8, 0, Text, -1, out.data(), size, nullptr, nullptr);
    return out;
}

std::string Utils::Base64(const std::string& Data)
{
    static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((Data.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < Data.size(); i += 3) {
        const uint32_t n = (uint32_t(uint8_t(Data[i])) << 16) | (uint32_t(uint8_t(Data[i + 1])) << 8) | uint8_t(Data[i + 2]);
        out += table[(n >> 18) & 63];
        out += table[(n >> 12) & 63];
        out += table[(n >> 6) & 63];
        out += table[n & 63];
    }
    if (i < Data.size()) {
        uint32_t n = uint32_t(uint8_t(Data[i])) << 16;
        if (i + 1 < Data.size())
            n |= uint32_t(uint8_t(Data[i + 1])) << 8;
        out += table[(n >> 18) & 63];
        out += table[(n >> 12) & 63];
        out += i + 1 < Data.size() ? table[(n >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

std::string Utils::ToUtf8(const std::filesystem::path& Path)
{
    const std::u8string s = Path.u8string();
    return std::string(s.begin(), s.end());
}

std::filesystem::path Utils::FromUtf8(const std::string& Text)
{
    return std::filesystem::path(std::u8string(Text.begin(), Text.end()));
}

std::vector<std::string> Utils::SplitPath(const std::string& Path)
{
    std::vector<std::string> parts;
    std::string current;
    for (char c : Path) {
        if (c == '/' || c == '\\') {
            if (!current.empty()) parts.push_back(current);
            current.clear();
        }
        else {
            current += c;
        }
    }
    if (!current.empty()) parts.push_back(current);
    return parts;
}

std::string Utils::Pad(std::string_view Text, size_t Width)
{
    std::string out(Text);
    if (out.size() < Width)
        out.append(Width - out.size(), ' ');
    return out;
}

std::string Utils::Timestamp(bool bWithDate)
{
    SYSTEMTIME t;
    GetLocalTime(&t);
    char buf[32];
    if (bWithDate)
        std::snprintf(buf, sizeof(buf), "%04u-%02u-%02u %02u:%02u:%02u",
            t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    else
        std::snprintf(buf, sizeof(buf), "%02u:%02u:%02u.%03u",
            t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    return buf;
}
