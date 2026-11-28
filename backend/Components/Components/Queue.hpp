#pragma once
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "TaskQueue.hpp"

struct File;

enum class RepeatMode : uint8_t {
	Off,
	All,
	One
};

class Queue
{
public:
	Queue() = delete;

	static void Init();
	static void Shutdown();

	static bool Set(const std::vector<std::string>& Ids, size_t Start, const std::string& Source);
	static bool Next(bool bAuto = false, bool bCrossfade = false);
	static bool Previous();
	static bool Jump(size_t Index);
	static bool Remove(size_t Index);
	static void RemoveTracks(const std::vector<std::string>& Ids);
	static std::string CurrentSource();
	static bool PlayNext(const std::string& Id);
	static bool Add(const std::vector<std::string>& Ids);
	static void Clear();
	static void Stop();
	static void SetShuffle(bool bOn);
	static void SetRepeat(RepeatMode Mode);
	static void SetSkipToVocals(bool bOn);

	static void Persist(bool bForce);
	static std::string StateJson();
	static void OnChanged(std::function<void(const std::string&)> Handler);

	static RepeatMode ParseRepeat(const std::string& Name);
	static std::string RepeatName(RepeatMode Mode);

private:
	struct SkipPoints {
		double Start = 0;
		double End = 0;
	};

	static constexpr double VocalMargin = 5.0;

	static bool PlayCurrent(bool bCrossfade = false);
	static void CrossfadeNext();
	static SkipPoints FindSkipPoints(const File& Track);
	static void Broadcast();
	static std::vector<std::string> Shuffled(std::vector<std::string> Ids);
	static void ClampQueued();

	static inline std::recursive_mutex Mutex;
	static inline std::vector<std::string> Base;
	static inline std::vector<std::string> Order;
	static inline int64_t Index = -1;
	static inline int64_t Queued = 0;
	static inline bool bShuffle = false;
	static inline bool bSkipToVocals = false;
	static inline RepeatMode Repeat = RepeatMode::Off;
	static inline std::string Source;
	static inline std::function<void(const std::string&)> ChangedHandler;
	static inline TaskQueue Worker;
	static inline std::chrono::steady_clock::time_point LastPersist{};
};
