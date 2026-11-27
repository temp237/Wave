#pragma once
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

class Visualizer
{
public:
	static constexpr size_t Bands = 64;
	using Frame = std::array<uint8_t, Bands>;

	Visualizer() = delete;

	static void Start(std::function<void(const Frame&)> Sink);
	static void Stop();
	static void SetEnabled(bool bOn);

private:
	static constexpr int FftSize = 16384;
	static constexpr int IntervalMs = 33;
	static constexpr float MinFrequency = 30.f;
	static constexpr float MaxFrequency = 16000.f;
	static constexpr float FloorDb = -64.f;
	static constexpr float RangeDb = 58.f;
	static constexpr float TiltDbPerOctave = 3.f;

	static void Run();
	static bool Analyze(Frame& Out);

	static inline std::function<void(const Frame&)> Output;
	static inline std::thread Worker;
	static inline std::mutex Mutex;
	static inline std::condition_variable Wake;
	static inline std::atomic<bool> bRunning = false;
	static inline std::atomic<bool> bEnabled = false;
};
