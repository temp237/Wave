#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

struct IMFMediaEngine;

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

	static bool Play(const std::filesystem::path& Path, const std::string& FileId);
	static bool Cue(const std::filesystem::path& Path, const std::string& FileId, double Position);
	static bool Pause();
	static bool Resume();
	static bool Stop();
	static bool Seek(double Seconds);
	static bool SetVolume(double Volume);

	static PlayerState GetState();
	static void OnStateChanged(std::function<void(const PlayerState&)> Handler);
	static void OnEnded(std::function<void()> Handler);

	static double ProbeDuration(const std::filesystem::path& Path);
	static std::string StatusName(PlayerStatus Status);

private:
	class Notify;

	static void HandleEvent(unsigned long Event);
	static void Broadcast();
	static void PublishClock();

	static inline IMFMediaEngine* Engine = nullptr;
	static inline Notify* Callback = nullptr;
	static inline std::atomic<PlayerStatus> Status = PlayerStatus::Stopped;
	static inline std::string CurrentFileId;
	static inline std::mutex FileIdMutex;
	static inline std::function<void(const PlayerState&)> StateHandler;
	static inline std::function<void()> EndedHandler;
	static inline std::atomic<double> PendingSeek = 0;
	static inline std::atomic<bool> bWasPlaying = false;
	static inline std::atomic<bool> bClockRunning = false;
	static inline std::thread ClockThread;
};
