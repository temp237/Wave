#pragma once
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

class Utils
{
public:
	Utils() = delete;

	static std::string ToUtf8(const std::filesystem::path& Path);
	static std::filesystem::path FromUtf8(const std::string& Text);
	static std::string WideToUtf8(const wchar_t* Text);
	static std::string Base64(const std::string& Data);

	static std::vector<std::string> SplitPath(const std::string& Path);
	static std::string Pad(std::string_view Text, size_t Width);
	static std::string Timestamp(bool bWithDate);
};
