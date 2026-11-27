#pragma once
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>

enum class TextColor : uint8_t {
	Black = 0,
	DarkBlue = 1,
	DarkGreen = 2,
	DarkCyan = 3,
	DarkRed = 4,
	DarkMagenta = 5,
	DarkYellow = 6,
	Gray = 7,
	DarkGray = 8,
	Blue = 9,
	Green = 10,
	Cyan = 11,
	Red = 12,
	Magenta = 13,
	Yellow = 14,
	White = 15
};

class Logger {
public:
	Logger() = delete;

	static void Log(const std::string& text, TextColor color = TextColor::White);

	static void Debug(const std::string& text) { Log(text, TextColor::Cyan); }
	static void Info(const std::string& text) { Log(text, TextColor::Blue); }
	static void Success(const std::string& text) { Log(text, TextColor::Green); }
	static void Warning(const std::string& text) { Log(text, TextColor::Yellow); }
	static void Error(const std::string& text) { Log(text, TextColor::Red); }
	static void Prompt(const std::string& text) { Log(text, TextColor::DarkMagenta); }

	static void ClearConsole();
	static void OpenConsole(const wchar_t* title = L"Wave Backend");

	static std::filesystem::path LogsDirectory();

	struct LogStats {
		uint64_t Bytes = 0;
		uint64_t Files = 0;
	};
	static LogStats Stats();
	static uint64_t Clear();

private:
	struct Level {
		const char* name;
		TextColor color;
	};

	static Level LevelFor(TextColor color);
	static void SplitSource(std::string_view text, std::string_view& source, std::string_view& message);
	static void ConsoleWrite(void* console, std::string_view text, TextColor color);
	static void WriteToFile(const std::string& text);

	static inline std::ofstream s_logFile;
	static inline std::filesystem::path s_logPath;
	static inline bool s_triedOpen = false;
	static inline std::mutex s_mutex;
};
