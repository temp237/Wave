#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <iostream>

#include "Logger.hpp"
#include "Utils.hpp"
#include "AppData.hpp"

Logger::Level Logger::LevelFor(TextColor color) {
	switch (color) {
	case TextColor::Cyan:        return { "DEBUG", TextColor::Cyan };
	case TextColor::Blue:        return { "INFO",  TextColor::Blue };
	case TextColor::Green:       return { "OK",    TextColor::Green };
	case TextColor::Yellow:      return { "WARN",  TextColor::Yellow };
	case TextColor::Red:         return { "ERROR", TextColor::Red };
	case TextColor::DarkMagenta: return { "INPUT", TextColor::Magenta };
	default:                     return { "LOG",   TextColor::Gray };
	}
}

void Logger::SplitSource(std::string_view text, std::string_view& source, std::string_view& message) {
	source = {};
	message = text;
	if (text.size() > 2 && text.front() == '[') {
		const size_t close = text.find(']');
		if (close != std::string_view::npos && close > 1) {
			source = text.substr(1, close - 1);
			message = text.substr(close + 1);
			while (!message.empty() && message.front() == ' ')
				message.remove_prefix(1);
		}
	}
}

void Logger::ConsoleWrite(void* console, std::string_view text, TextColor color) {
	SetConsoleTextAttribute(console, static_cast<WORD>(color));
	std::cout << text;
}

void Logger::Log(const std::string& text, TextColor color) {
	if (!s_triedOpen)
		LogsDirectory();
	std::lock_guard lock(s_mutex);

	const Level level = LevelFor(color);
	std::string_view source, message;
	SplitSource(text, source, message);

	const std::string time = Utils::Timestamp(false);
	const std::string levelCol = Utils::Pad(level.name, 6);
	const std::string sourceCol = Utils::Pad(source, 10);

	HANDLE console = GetStdHandle(STD_OUTPUT_HANDLE);
	if (console && console != INVALID_HANDLE_VALUE && GetFileType(console) != FILE_TYPE_UNKNOWN) {
		ConsoleWrite(console, time + "  ", TextColor::DarkGray);
		ConsoleWrite(console, levelCol, level.color);
		ConsoleWrite(console, sourceCol + "  ", TextColor::DarkGray);
		ConsoleWrite(console, message, TextColor::White);
		std::cout << '\n';
		SetConsoleTextAttribute(console, static_cast<WORD>(TextColor::Gray));
	}

	WriteToFile(time + "  " + levelCol + sourceCol + "  " + std::string(message));
}

void Logger::ClearConsole() {
	std::system("cls");
}

void Logger::OpenConsole(const wchar_t* title) {
	if (!GetConsoleWindow() && !AllocConsole())
		return;
	SetConsoleTitleW(title);

	FILE* stream = nullptr;
	freopen_s(&stream, "CONOUT$", "w", stdout);
	freopen_s(&stream, "CONOUT$", "w", stderr);
	freopen_s(&stream, "CONIN$", "r", stdin);
	std::cout.clear();
	std::cerr.clear();
	std::cin.clear();
}

std::filesystem::path Logger::LogsDirectory() {
	std::filesystem::path logsPath = AppData::Directory() / "logs";
	if (std::filesystem::exists(logsPath) == false) {
		std::filesystem::create_directory(logsPath);
	}
	return logsPath;
}

Logger::LogStats Logger::Stats() {
	LogStats stats;
	std::error_code ec;
	for (const auto& entry : std::filesystem::directory_iterator(LogsDirectory(), ec)) {
		if (!entry.is_regular_file(ec))
			continue;
		stats.Files++;
		stats.Bytes += entry.file_size(ec);
	}
	return stats;
}

uint64_t Logger::Clear() {
	if (!s_triedOpen)
		LogsDirectory();
	std::lock_guard lock(s_mutex);
	uint64_t freed = 0;
	std::error_code ec;
	for (const auto& entry : std::filesystem::directory_iterator(LogsDirectory(), ec)) {
		if (!entry.is_regular_file(ec) || entry.path() == s_logPath)
			continue;
		const uint64_t size = entry.file_size(ec);
		if (std::filesystem::remove(entry.path(), ec))
			freed += size;
	}
	if (s_logFile.is_open()) {
		const uint64_t size = std::filesystem::file_size(s_logPath, ec);
		s_logFile.close();
		s_logFile.open(s_logPath, std::ios::trunc);
		freed += ec ? 0 : size;
		if (s_logFile.is_open()) {
			s_logFile << "==== Wave backend  |  logs cleared " << Utils::Timestamp(true)
				<< "  |  pid " << GetCurrentProcessId() << " ====\n\n";
			s_logFile.flush();
		}
	}
	return freed;
}

void Logger::WriteToFile(const std::string& text) {
	// open once per run (truncates the old log) instead of reopening for every line
	if (!s_triedOpen) {
		s_triedOpen = true;
		// next to the exe, not the working directory (dragging a .ptx onto the exe starts it in the .ptx's folder)
		wchar_t exePath[MAX_PATH] = {};
		const DWORD length = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
		const std::filesystem::path logPath = LogsDirectory() / ("log-" + std::to_string(time(0)) + ".log");
		s_logPath = logPath;
		s_logFile.open(logPath, std::ios::trunc);
		if (!s_logFile.is_open()) {
			std::cerr << "Failed to open log.txt for writing." << std::endl;
		}
		else {
			s_logFile << "==== Wave backend  |  started " << Utils::Timestamp(true)
				<< "  |  pid " << GetCurrentProcessId() << " ====\n\n";
		}
	}

	if (s_logFile.is_open()) {
		s_logFile << text << '\n';
		s_logFile.flush();
	}
}
