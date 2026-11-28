#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#include "AppData.hpp"
#include "Separator.hpp"
#include "Transcriber.hpp"

class Lyrics
{
public:
	static constexpr int Version = 5;

	Lyrics() = delete;

	static void Start();
	static void Shutdown();
	static void Prioritize(const std::string& Id, const std::filesystem::path& Path);
	static void FollowQueue(std::vector<std::string> Upcoming);
	static size_t ClearAll();
	static bool IsRunning() { return bRunning; }
	static bool NeedsWork(const std::string& Status, int StoredVersion);
	static void OnStored(std::function<void(const std::string&)> Handler);

private:
	using Job = std::pair<std::string, std::filesystem::path>;

	static constexpr const char* NormalizeFilters = "loudnorm=I=-16:TP=-1.5:LRA=11,aresample=44100,aformat=sample_fmts=flt:channel_layouts=stereo";
	static constexpr unsigned WorkerCount = 2;
	static constexpr size_t SaveEvery = 5;
	static constexpr size_t FollowLimit = 50;
	static constexpr double MaxSeconds = 20.0 * 60.0;
	static constexpr auto DownloadRetry = std::chrono::minutes(10);

	static std::filesystem::path ModelsDirectory();
	static void Boot();
	static bool PrepareModels();
	static void QueueMissing();
	static std::optional<Job> NextJob();
	static void Work();
	static void Process(const std::string& Id, const std::filesystem::path& Path);
	static void Store(const std::string& Id, std::vector<LyricLine> Lines, const std::string& Status, double VocalStart = -1.0, double VocalEnd = -1.0);
	static void Save(bool bForce);

	static inline std::unique_ptr<Separator> Separation;
	static inline std::unique_ptr<Transcriber> Transcription;
	static inline std::mutex Mutex;
	static inline std::mutex TranscribeLock;
	static inline std::condition_variable Wake;
	static inline std::deque<Job> Pending;
	static inline std::unordered_set<std::string> Queued;
	static inline std::vector<std::string> Upcoming;
	static inline std::thread Booter;
	static inline std::vector<std::thread> Workers;
	static inline std::atomic<bool> bRunning = false;
	static inline std::atomic<bool> bCancel = false;
	static inline std::atomic<size_t> Unsaved = 0;
	static inline std::function<void(const std::string&)> StoredHandler;
};
