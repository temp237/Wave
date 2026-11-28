#pragma once
#include <nlohmann/json.hpp>
#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>

struct IUnknown;

class Equalizer
{
public:
	static constexpr size_t BandCount = 10;
	static constexpr std::array<float, BandCount> Frequencies = { 31.f, 62.f, 125.f, 250.f, 500.f, 1000.f, 2000.f, 4000.f, 8000.f, 16000.f };
	static constexpr float MaxGain = 12.f;

	struct Settings {
		bool bEnabled = false;
		float Preamp = 0.f;
		std::array<float, BandCount> Gains{};
	};

	Equalizer() = delete;

	static void Load();
	static bool Save();
	static void Set(const Settings& Value);
	static Settings Get();

	static nlohmann::json ToJson();
	static bool FromJson(const nlohmann::json& j);

	static IUnknown* CreateEffect(int Slot);

private:
	class Transform;

	static inline std::mutex Mutex;
	static inline Settings Current;
	static inline std::atomic<uint32_t> Version = 1;
};
