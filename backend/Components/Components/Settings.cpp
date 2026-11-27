#include "Settings.hpp"
#include "AppData.hpp"
#include "Logger.hpp"

void Settings::Load()
{
    if (bLoaded)
        return;
    bLoaded = true;
    Data = nlohmann::json::parse(AppData::ReadFile(AppData::Directory() / "settings.json"), nullptr, false);
    if (!Data.is_object())
        Data = nlohmann::json::object();
}

bool Settings::Save()
{
    if (!AppData::WriteFile(AppData::Directory() / "settings.json", Data.dump(2))) {
        Logger::Error("[Settings] Couldn't save settings.json");
        return false;
    }
    return true;
}

nlohmann::json Settings::Get(const std::string& Section)
{
    std::lock_guard lock(Mutex);
    Load();
    auto it = Data.find(Section);
    return it != Data.end() && it->is_object() ? *it : nlohmann::json::object();
}

bool Settings::Set(const std::string& Section, const nlohmann::json& Value)
{
    std::lock_guard lock(Mutex);
    Load();
    Data[Section] = Value;
    return Save();
}

bool Settings::Merge(const std::string& Section, const nlohmann::json& Patch)
{
    std::lock_guard lock(Mutex);
    Load();
    if (!Data.contains(Section) || !Data[Section].is_object())
        Data[Section] = nlohmann::json::object();
    Data[Section].update(Patch);
    return Save();
}

bool Settings::Bool(const nlohmann::json& Object, const char* Key, bool Default)
{
    auto it = Object.is_object() ? Object.find(Key) : Object.end();
    return Object.is_object() && it != Object.end() && it->is_boolean() ? it->get<bool>() : Default;
}

double Settings::Number(const nlohmann::json& Object, const char* Key, double Default)
{
    auto it = Object.is_object() ? Object.find(Key) : Object.end();
    return Object.is_object() && it != Object.end() && it->is_number() ? it->get<double>() : Default;
}

std::string Settings::String(const nlohmann::json& Object, const char* Key, const std::string& Default)
{
    auto it = Object.is_object() ? Object.find(Key) : Object.end();
    return Object.is_object() && it != Object.end() && it->is_string() ? it->get<std::string>() : Default;
}
