#pragma once
#include <windows.h>
#include <cstdint>

class RelayBuffer
{
public:
	static constexpr const wchar_t* MappingName = L"Global\\WaveMicRelay3";
	static constexpr uint32_t Magic = 0x524D5657;
	static constexpr uint32_t Version = 3;
	static constexpr uint32_t Capacity = 1u << 21;
	static constexpr uint32_t Mask = Capacity - 1;
	static constexpr ULONGLONG StaleAfterMs = 600;

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
		LONG Reserved;
	};

	static constexpr size_t Size = sizeof(Header) + static_cast<size_t>(Capacity) * 2 * sizeof(float);

	static float* Samples(Header* View)
	{
		return reinterpret_cast<float*>(View + 1);
	}

	static const float* Samples(const Header* View)
	{
		return reinterpret_cast<const float*>(View + 1);
	}

	static bool IsValid(const Header* View)
	{
		return View && View->Magic == Magic && View->Version == Version && View->Capacity == Capacity;
	}

	static void WriteAnchor(Header* View, LONG64 Frame, LONG64 Time)
	{
		InterlockedIncrement(&View->AnchorSequence);
		View->AnchorFrame = Frame;
		View->AnchorTime = Time;
		InterlockedIncrement(&View->AnchorSequence);
	}

	static bool ReadAnchor(const Header* View, LONG64& Frame, LONG64& Time)
	{
		for (int attempt = 0; attempt < 4; attempt++) {
			const LONG before = View->AnchorSequence;
			if (before & 1)
				continue;
			MemoryBarrier();
			Frame = View->AnchorFrame;
			Time = View->AnchorTime;
			MemoryBarrier();
			if (View->AnchorSequence == before)
				return before != 0;
		}
		return false;
	}

	static void WriteClock(Header* View, LONG64 Time, LONG64 Qpc, bool bPlaying)
	{
		InterlockedIncrement(&View->ClockSequence);
		View->ClockTime = Time;
		View->ClockQpc = Qpc;
		View->Playing = bPlaying ? 1 : 0;
		InterlockedIncrement(&View->ClockSequence);
	}

	static bool ReadClock(const Header* View, LONG64& Time, LONG64& Qpc, bool& bPlaying)
	{
		for (int attempt = 0; attempt < 4; attempt++) {
			const LONG before = View->ClockSequence;
			if (before & 1)
				continue;
			MemoryBarrier();
			Time = View->ClockTime;
			Qpc = View->ClockQpc;
			bPlaying = View->Playing != 0;
			MemoryBarrier();
			if (View->ClockSequence == before)
				return before != 0;
		}
		return false;
	}
};
