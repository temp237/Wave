#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#include "../Components/Components/Player.hpp"

const std::string APPLICATION_ID = "1574183360270438420";
const std::string MAIN_ICON_ASSET = "wave-icon-512";
const uint32_t DISCORD_RPC_UPDATE_INTERVAL_SECONDS = 5;

class DiscordPresence
{
private:
	static inline std::recursive_mutex Mutex;
	static inline std::thread PeriodicThread;
	static inline std::condition_variable_any PeriodicWake;
	static inline bool bPeriodicThreadRunning = false;
	static inline bool bInitialized = false;
	static inline bool bEnabled = true;
	static inline bool bDirty = true;
	static inline bool bShown = false;

	static inline PlayerState Current{ PlayerStatus::Stopped, {}, 0, 0 };
	static inline std::string SentKey;
	static inline int64_t SentStart = 0;
	static inline std::chrono::steady_clock::time_point LastSent{};
	static inline std::chrono::steady_clock::time_point LastConnectAttempt{};

	struct Sent {
		std::string Key;
		int64_t Start;
		bool bShown;
	};

private:
	static void InitializePeriodic();
	static Sent Publish(const PlayerState& State, bool bShow, bool bWasShown);
	static bool EnsureConnected();
	static int64_t NowMs();
	static std::string StateKey(const PlayerState& State);
public:
	static void Initialize();
	static void Shutdown();
	static void SetEnabled(bool bEnabled);
	static bool IsEnabled();
	static void OnPlayerState(const PlayerState& State);

	static void Load();
	static bool Save();
};
