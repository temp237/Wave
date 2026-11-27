#pragma once
#include <nlohmann/json.hpp>
#include <mutex>
#include <string>

class Settings
{
public:
	Settings() = delete;

	static nlohmann::json Get(const std::string& Section);
	static bool Set(const std::string& Section, const nlohmann::json& Value);
	static bool Merge(const std::string& Section, const nlohmann::json& Patch);

	static bool Bool(const nlohmann::json& Object, const char* Key, bool Default);
	static double Number(const nlohmann::json& Object, const char* Key, double Default);
	static std::string String(const nlohmann::json& Object, const char* Key, const std::string& Default);

private:
	static inline nlohmann::json Data;
	static inline bool bLoaded = false;
	static inline std::mutex Mutex;

	static void Load();
	static bool Save();
};
