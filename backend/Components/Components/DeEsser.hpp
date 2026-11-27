#pragma once
#include <nlohmann/json.hpp>
#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

class DeEsser
{
public:
	static constexpr float MinGain = -24.f;
	static constexpr float MinFrequency = 2000.f;
	static constexpr float MaxFrequency = 12000.f;
	static constexpr float AutoMaxCut = 10.f;

	struct Settings {
		bool bEnabled = false;
		bool bAuto = false;
		float Gain = 0.f;
		float Frequency = 6500.f;
	};

	class Processor
	{
	public:
		void Process(float* Data, size_t Frames, uint32_t Channels, uint32_t SampleRate);

	private:
		struct Biquad {
			float B0 = 0, B1 = 0, B2 = 0, A1 = 0, A2 = 0;
		};
		static constexpr size_t DetectorCount = 6;
		static constexpr std::array<float, DetectorCount> DetectorFrequencies = { 3500.f, 4500.f, 5600.f, 7000.f, 8500.f, 10500.f };
		static constexpr size_t Block = 32;

		static Biquad BandPass(float Frequency, float Q, uint32_t SampleRate);
		static float Run(const Biquad& k, std::array<float, 2>& z, float x);
		void Reset(uint32_t Channels, uint32_t SampleRate);
		void UpdateBlock();

		uint32_t Rate = 0;
		uint32_t SeenVersion = 0;
		Settings Current;
		bool bOn = false;
		float Frequency = 6500.f;
		float AppliedFrequency = 0.f;
		Biquad Band;
		std::vector<std::array<float, 2>> BandState;
		std::vector<float> Scratch;
		std::array<Biquad, DetectorCount> Detectors{};
		std::array<std::array<float, 2>, DetectorCount> DetectorState{};
		std::array<float, DetectorCount> DetectorEnergy{};
		float MonoEnergy = 0.f;
		float HighBaseline = -20.f;
		float BandEnvelope = 0.f;
		float FullEnvelope = 0.f;
		float AverageRatio = -20.f;
		float TargetGain = 1.f;
		float Gain = 1.f;
		float EnvelopeAttack = 0.f;
		float EnvelopeRelease = 0.f;
		float GainAttack = 0.f;
		float GainRelease = 0.f;
		float DetectorCoefficient = 0.f;
		float AverageCoefficient = 0.f;
		float FrequencyCoefficient = 0.f;
		float BaselineFall = 0.f;
		size_t Counter = 0;
	};

	DeEsser() = delete;

	static void Load();
	static bool Save();
	static void Set(const Settings& Value);
	static Settings Get();
	static uint32_t CurrentVersion();

	static nlohmann::json ToJson();
	static bool FromJson(const nlohmann::json& j);

private:
	static inline std::mutex Mutex;
	static inline Settings Current;
	static inline std::atomic<uint32_t> Version = 1;
};
