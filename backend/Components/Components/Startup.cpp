#include <Windows.h>

#include "Startup.hpp"
#include "Logger.hpp"
#include "Utils.hpp"

#pragma comment(lib, "advapi32.lib")

std::filesystem::path Startup::AppPath()
{
	wchar_t buffer[MAX_PATH * 4];
	const DWORD length = GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
	if (length == 0 || length >= std::size(buffer))
		return {};
	return std::filesystem::path(buffer).parent_path() / L"Wave.exe";
}

std::wstring Startup::Command()
{
	return L"\"" + AppPath().wstring() + L"\"";
}

std::wstring Startup::ReadEntry()
{
	DWORD size = 0;
	if (RegGetValueW(HKEY_CURRENT_USER, RunKey, EntryName, RRF_RT_REG_SZ, nullptr, nullptr, &size) != ERROR_SUCCESS || size == 0)
		return {};
	std::wstring value(size / sizeof(wchar_t), L'\0');
	if (RegGetValueW(HKEY_CURRENT_USER, RunKey, EntryName, RRF_RT_REG_SZ, nullptr, value.data(), &size) != ERROR_SUCCESS)
		return {};
	value.resize(wcsnlen(value.c_str(), value.size()));
	return value;
}

bool Startup::IsEnabled()
{
	return !ReadEntry().empty();
}

bool Startup::SetEnabled(bool bEnabled)
{
	if (!bEnabled) {
		const LSTATUS status = RegDeleteKeyValueW(HKEY_CURRENT_USER, RunKey, EntryName);
		if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND) {
			Logger::Error("[Startup] Couldn't remove the startup entry (" + std::to_string(status) + ")");
			return false;
		}
		Logger::Info("[Startup] Wave won't run on startup");
		return true;
	}

	const std::filesystem::path app = AppPath();
	std::error_code ec;
	if (app.empty() || !std::filesystem::is_regular_file(app, ec)) {
		Logger::Error("[Startup] Couldn't find " + Utils::ToUtf8(app));
		return false;
	}

	const std::wstring command = Command();
	const LSTATUS status = RegSetKeyValueW(HKEY_CURRENT_USER, RunKey, EntryName, REG_SZ, command.c_str(),
		static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
	if (status != ERROR_SUCCESS) {
		Logger::Error("[Startup] Couldn't add the startup entry (" + std::to_string(status) + ")");
		return false;
	}
	Logger::Info("[Startup] Wave will run on startup");
	return true;
}

void Startup::Refresh()
{
	const std::wstring current = ReadEntry();
	if (current.empty() || _wcsicmp(current.c_str(), Command().c_str()) == 0)
		return;
	std::error_code ec;
	if (std::filesystem::is_regular_file(AppPath(), ec))
		SetEnabled(true);
}
