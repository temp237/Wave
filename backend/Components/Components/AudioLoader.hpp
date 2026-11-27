#pragma once
#include <atomic>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

class AudioLoader
{
public:
	AudioLoader() = delete;

	static std::optional<std::vector<float>> Load(const std::filesystem::path& Path, const std::string& Filters, std::string& Error, const std::atomic<bool>* Cancel = nullptr);
	static std::vector<float> Resample(const std::vector<float>& Input, int InputRate, int InputChannels, int OutputRate, int OutputChannels);

private:
	static std::string ToUtf8(const std::filesystem::path& Path);
};
