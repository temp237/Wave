#pragma once
#include <filesystem>
#include <string>

class Startup
{
public:
	Startup() = delete;

	static bool IsEnabled();
	static bool SetEnabled(bool bEnabled);
	static void Refresh();

private:
	static constexpr const wchar_t* RunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
	static constexpr const wchar_t* EntryName = L"Wave";

	static std::filesystem::path AppPath();
	static std::wstring Command();
	static std::wstring ReadEntry();
};
