#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

struct IMFMediaEngine;
struct IMFMediaEngineClassFactory;

enum class PlayerStatus : uint8_t {
	Stopped,
	Loading,
	Playing,
	Paused,
	Ended,
	Error
};

struct PlayerState {
	PlayerStatus Status;
	std::string FileId;
	double Position;
	double Duration;
};

class Player
{
public:
	Player() = delete;

	static bool Init();
	static void Shutdown();

	static constexpr int EngineCount = 2;

	static bool Play(const std::filesystem::path& Path, const std::string& FileId, double Start = 0, bool bCrossfade = false);
	static bool Cue(const std::filesystem::path& Path, const std::string& FileId, double Position);
	static bool Pause();
	static bool Resume();
	static bool Stop();
	static bool Seek(double Seconds);
	static bool SetVolume(double Volume);
	static void SetEnd(double Seconds);
	static void SetCrossfade(double Seconds);

	static PlayerState GetState();
	static void OnStateChanged(std::function<void(const PlayerState&)> Handler);
	static void OnEnded(std::function<void()> Handler);
	static void OnFadePoint(std::function<void()> Handler);

	static double ProbeDuration(const std::filesystem::path& Path);
	static std::string StatusName(PlayerStatus Status);

private:
	class Notify;

	static bool CreateEngine(IMFMediaEngineClassFactory* Factory, int Slot);
	static IMFMediaEngine* Active();
	static double RelayPosition();
	static void Isolate();
	static void HandleEvent(int Slot, unsigned long Event);
	static void Broadcast();
	static void PublishClock();

	static inline std::array<IMFMediaEngine*, EngineCount> Engines{};
	static inline std::array<Notify*, EngineCount> Callbacks{};
	static inline std::atomic<int> Current = 0;
	static inline std::atomic<double> FadeSeconds = 0;
	static inline std::function<void()> FadeHandler;
	static inline std::atomic<PlayerStatus> Status = PlayerStatus::Stopped;
	static inline std::string CurrentFileId;
	static inline std::mutex FileIdMutex;
	static inline std::function<void(const PlayerState&)> StateHandler;
	static inline std::function<void()> EndedHandler;
	static inline std::atomic<double> PendingSeek = 0;
	static inline std::atomic<double> EndAt = 0;
	static inline std::atomic<bool> bWasPlaying = false;
	static inline std::atomic<bool> bClockRunning = false;
	static inline std::thread ClockThread;
};
