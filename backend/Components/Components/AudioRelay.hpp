#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <vector>

class AudioRelay
{
public:
	AudioRelay() = delete;

	struct Header {
		uint32_t Magic;
		uint32_t Version;
		uint32_t Capacity;
		volatile LONG SampleRate;
		volatile LONG64 WritePosition;
		volatile LONG64 LastWriteTick;
		volatile LONG AnchorSequence;
		volatile LONG ClockSequence;
		volatile LONG64 AnchorFrame;
		volatile LONG64 AnchorTime;
		volatile LONG64 ClockTime;
		volatile LONG64 ClockQpc;
		volatile LONG Playing;
		volatile float Volume;
		volatile LONG64 ReaderTick;
		volatile LONG64 StartFrame;
		volatile LONG StartCount;
		LONG Reserved;
		volatile LONG64 HeardFrame;
	};

	static constexpr const wchar_t* MappingName = L"Local\\WaveAudioRelay4";
	static constexpr uint32_t Magic = 0x52415657;
	static constexpr uint32_t Version = 4;
	static constexpr uint32_t Capacity = 1u << 21;
	static constexpr uint32_t Mask = Capacity - 1;
	static constexpr size_t Size = sizeof(Header) + static_cast<size_t>(Capacity) * 2 * sizeof(float);
	static constexpr ULONGLONG ReaderTimeoutMs = 400;

	static bool Open();
	static void Close();
	static bool IsReaderActive();
	static void SetVolume(double Volume);
	static void SetClock(double Seconds, bool bPlaying);
	static void Write(float* Data, size_t Frames, uint32_t Channels, uint32_t SampleRate, LONGLONG SampleTime);
	static void Discontinuity();
	static void MarkStart(double Seconds);
	static bool ReadPlaying(size_t Frames, std::vector<float>& Mono, uint32_t& SampleRate);

private:
	struct Chunk {
		LONG64 Frame;
		LONG64 Time;
		LONG64 Frames;
		uint32_t Rate;
	};
	static constexpr size_t MaxChunks = 1024;

	static float* Samples(Header* View);
	static void WriteAnchor(Header* View, LONG64 Frame, LONG64 Time);
	static void ResolveStart();

	static inline std::deque<Chunk> Chunks;
	static inline bool bTrimmed = false;
	static inline std::optional<LONG64> PendingStart;

	static inline HANDLE Mapping = nullptr;
	static inline Header* View = nullptr;
	static inline std::mutex Mutex;
	static inline std::atomic<bool> bReaderWasActive = false;
};
