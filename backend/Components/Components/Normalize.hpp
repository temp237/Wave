#pragma once
#include <nlohmann/json.hpp>

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class Normalize
{
public:
	struct Loudness {
		double Integrated = 0.0;
		double Peak = 1.0;
		bool bValid = false;
	};

	static constexpr int SlotCount = 2;

	class Processor
	{
	public:
		explicit Processor(int Slot = 0) : Slot(Slot) {}
		void Process(float* Data, size_t Frames, uint32_t Channels, uint32_t SampleRate);

	private:
		int Slot = 0;
		float Gain = 1.f;
		float Coefficient = 0.f;
		uint32_t Rate = 0;
		uint32_t SeenTrack = 0;
	};

	static constexpr double TargetLufs = -8.0;
	static constexpr double MaxBoostDb = 6.0;
	static constexpr double HeadroomDb = 1.0;

	Normalize() = delete;

	static void Load();
	static void Shutdown();
	static bool IsEnabled();
	static void SetEnabled(bool bOn);
	static nlohmann::json StateJson();
	static void SetTrack(int Slot, const std::string& Id, const std::filesystem::path& Path);
	static std::optional<Loudness> Analyze(const std::filesystem::path& Path);

private:
	static void StartWorkers();
	static void StopWorkers();
	static void QueueMissing();
	static void Work();
	static void Store(const std::string& Id, const Loudness& Value);
	static void ApplyCurrent();
	static double GainDb(const Loudness& Value);
	static std::filesystem::path CachePath();
	static void LoadCache();
	static void SaveCache();

	static inline std::mutex Mutex;
	static inline std::condition_variable Wake;
	static inline std::unordered_map<std::string, Loudness> Cache;
	static inline std::deque<std::pair<std::string, std::filesystem::path>> Pending;
	static inline std::unordered_set<std::string> Queued;
	static inline std::array<std::string, SlotCount> CurrentIds;
	static inline std::vector<std::thread> Workers;
	static inline size_t Unsaved = 0;
	static inline std::atomic<bool> bEnabled = false;
	static inline std::atomic<bool> bRunning = false;
	static inline std::array<std::atomic<float>, SlotCount> Targets{ 1.f, 1.f };
	static inline std::array<std::atomic<uint32_t>, SlotCount> TrackChanges{};
};
