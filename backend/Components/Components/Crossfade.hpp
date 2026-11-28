#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

class Crossfade
{
public:
	static constexpr double MaxSeconds = 12.0;

	Crossfade() = delete;

	static void Reset(int Writer);
	static void Begin(int Outgoing, int Incoming, double Duration);
	static void OutgoingEnded();
	static void Flush(int Slot);
	static bool IsActive();
	static int Writer();
	static double OffsetSeconds();
	static int TakeFinished();
	static void Process(int Slot, float* Data, size_t Frames, uint32_t Channels, uint32_t SampleRate, int64_t SampleTime);

private:
	enum class Stage : uint8_t {
		Idle,
		Fading,
		Handoff
	};

	static constexpr double CushionSeconds = 0.1;
	static constexpr int64_t Ticks = 10'000'000;

	static float Curve(double Progress);
	static void ApplyFadeIn(float* Data, size_t Frames, uint32_t Channels, uint32_t SampleRate);
	static void Emit(float* Data, size_t Frames, uint32_t Channels, uint32_t SampleRate, int64_t SampleTime);
	static int64_t BacklogHead();
	static void CompleteHandoff(int64_t SampleTime);

	static inline std::mutex Mutex;
	static inline Stage Phase = Stage::Idle;
	static inline std::atomic<int> WriterSlot = 0;
	static inline int OutSlot = -1;
	static inline int InSlot = -1;
	static inline double Seconds = 0.0;
	static inline uint64_t OutFrames = 0;
	static inline uint64_t InFrames = 0;
	static inline bool bFadingIn = false;
	static inline bool bMixing = false;
	static inline std::vector<float> Backlog;
	static inline size_t BacklogRead = 0;
	static inline uint64_t BacklogConsumed = 0;
	static inline uint32_t BacklogChannels = 0;
	static inline uint32_t BacklogRate = 0;
	static inline int64_t BacklogTime = -1;
	static inline int64_t OutEndTime = -1;
	static inline std::atomic<int64_t> Offset = 0;
	static inline std::atomic<int> Finished = -1;
};
