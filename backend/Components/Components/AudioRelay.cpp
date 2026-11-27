#include <algorithm>
#include <cmath>
#include <cstring>

#include "AudioRelay.hpp"
#include "Logger.hpp"

static_assert(sizeof(AudioRelay::Header) == 112);

bool AudioRelay::Open()
{
	std::lock_guard lock(Mutex);
	if (View)
		return true;
	const uint64_t size = Size;
	HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, static_cast<DWORD>(size >> 32), static_cast<DWORD>(size), MappingName);
	if (!mapping) {
		Logger::Error("[AudioRelay] Couldn't create the audio buffer (" + std::to_string(GetLastError()) + ")");
		return false;
	}
	auto* view = static_cast<Header*>(MapViewOfFile(mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, Size));
	if (!view) {
		CloseHandle(mapping);
		return false;
	}
	std::memset(view, 0, sizeof(Header));
	view->Capacity = Capacity;
	view->Version = Version;
	view->Volume = 1.0f;
	view->HeardFrame = -1;
	MemoryBarrier();
	view->Magic = Magic;
	Mapping = mapping;
	View = view;
	Logger::Info("[AudioRelay] Audio buffer ready for the frontend");
	return true;
}

void AudioRelay::Close()
{
	std::lock_guard lock(Mutex);
	if (View)
		UnmapViewOfFile(View);
	if (Mapping)
		CloseHandle(Mapping);
	View = nullptr;
	Mapping = nullptr;
}

bool AudioRelay::IsReaderActive()
{
	if (!View)
		return false;
	const LONG64 tick = View->ReaderTick;
	return tick != 0 && static_cast<LONG64>(GetTickCount64()) - tick < static_cast<LONG64>(ReaderTimeoutMs);
}

void AudioRelay::SetVolume(double Value)
{
	if (!std::isfinite(Value))
		Value = 1.0;
	std::lock_guard lock(Mutex);
	if (View)
		View->Volume = static_cast<float>(std::clamp(Value, 0.0, 1.0));
}

void AudioRelay::SetClock(double Seconds, bool bPlaying)
{
	if (!std::isfinite(Seconds))
		return;
	std::lock_guard lock(Mutex);
	if (!View)
		return;
	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);
	InterlockedIncrement(&View->ClockSequence);
	View->ClockTime = static_cast<LONG64>(Seconds * 10'000'000.0);
	View->ClockQpc = now.QuadPart;
	View->Playing = bPlaying ? 1 : 0;
	InterlockedIncrement(&View->ClockSequence);
}

bool AudioRelay::ReadPlaying(size_t Frames, std::vector<float>& Mono, uint32_t& SampleRate)
{
	std::lock_guard lock(Mutex);
	if (!View || !View->Playing || View->SampleRate <= 0)
		return false;
	SampleRate = static_cast<uint32_t>(View->SampleRate);
	const LONG64 written = View->WritePosition;
	LONG64 end = -1;
	if (IsReaderActive())
		end = View->HeardFrame;
	if (end < 0) {
		const LONG64 anchorFrame = View->AnchorFrame;
		const LONG64 anchorTime = View->AnchorTime;
		LARGE_INTEGER now, frequency;
		QueryPerformanceCounter(&now);
		QueryPerformanceFrequency(&frequency);
		const double elapsed = std::clamp(static_cast<double>(now.QuadPart - View->ClockQpc) / frequency.QuadPart, 0.0, 1.0);
		end = anchorFrame + static_cast<LONG64>(((View->ClockTime - anchorTime) / 1e7 + elapsed) * SampleRate);
	}
	end = std::min(end, written);
	const LONG64 start = end - static_cast<LONG64>(Frames);
	if (start < 0 || written - start > static_cast<LONG64>(Capacity))
		return false;
	const float* samples = Samples(View);
	Mono.resize(Frames);
	for (size_t i = 0; i < Frames; i++) {
		const uint32_t index = static_cast<uint32_t>(start + static_cast<LONG64>(i)) & Mask;
		Mono[i] = 0.5f * (samples[index * 2] + samples[index * 2 + 1]);
	}
	return true;
}

void AudioRelay::Discontinuity()
{
	std::lock_guard lock(Mutex);
	Chunks.clear();
	bTrimmed = false;
	PendingStart.reset();
}

void AudioRelay::MarkStart(double Seconds)
{
	if (!std::isfinite(Seconds) || Seconds < 0)
		Seconds = 0;
	std::lock_guard lock(Mutex);
	if (!View)
		return;
	if (!(View->StartCount & 1))
		InterlockedIncrement(&View->StartCount);
	PendingStart = static_cast<LONG64>(Seconds * 10'000'000.0);
	ResolveStart();
}

void AudioRelay::ResolveStart()
{
	if (!View || !PendingStart || Chunks.empty())
		return;
	const LONG64 target = *PendingStart;
	const Chunk& first = Chunks.front();
	if (target < first.Time && bTrimmed) {
		PendingStart.reset();
		InterlockedExchange64(&View->StartFrame, -1);
		InterlockedIncrement(&View->StartCount);
		return;
	}
	for (const Chunk& chunk : Chunks) {
		const LONG64 end = chunk.Time + chunk.Frames * 10'000'000 / chunk.Rate;
		if (target >= end)
			continue;
		const LONG64 offset = target > chunk.Time ? (target - chunk.Time) * chunk.Rate / 10'000'000 : 0;
		InterlockedExchange64(&View->StartFrame, chunk.Frame + offset);
		InterlockedIncrement(&View->StartCount);
		PendingStart.reset();
		return;
	}
}

float* AudioRelay::Samples(Header* Target)
{
	return reinterpret_cast<float*>(Target + 1);
}

void AudioRelay::WriteAnchor(Header* Target, LONG64 Frame, LONG64 Time)
{
	InterlockedIncrement(&Target->AnchorSequence);
	Target->AnchorFrame = Frame;
	Target->AnchorTime = Time;
	InterlockedIncrement(&Target->AnchorSequence);
}

void AudioRelay::Write(float* Data, size_t Frames, uint32_t Channels, uint32_t SampleRate, LONGLONG SampleTime)
{
	if (!Data || Frames == 0 || Channels == 0 || SampleRate == 0)
		return;
	std::lock_guard lock(Mutex);
	if (!View)
		return;

	float* samples = Samples(View);
	if (View->SampleRate != static_cast<LONG>(SampleRate))
		InterlockedExchange(&View->SampleRate, static_cast<LONG>(SampleRate));

	const LONG64 start = View->WritePosition;
	if (SampleTime >= 0)
		WriteAnchor(View, start, SampleTime);
	for (size_t f = 0; f < Frames; f++) {
		const float left = Data[f * Channels];
		const float right = Channels > 1 ? Data[f * Channels + 1] : left;
		const uint32_t index = static_cast<uint32_t>(start + static_cast<LONG64>(f)) & Mask;
		samples[index * 2] = left;
		samples[index * 2 + 1] = right;
	}
	InterlockedExchange64(&View->WritePosition, start + static_cast<LONG64>(Frames));
	InterlockedExchange64(&View->LastWriteTick, static_cast<LONG64>(GetTickCount64()));

	if (SampleTime >= 0) {
		Chunks.push_back({ start, SampleTime, static_cast<LONG64>(Frames), SampleRate });
		if (Chunks.size() > MaxChunks) {
			Chunks.pop_front();
			bTrimmed = true;
		}
		ResolveStart();
	}

	const bool active = IsReaderActive();
	if (active != bReaderWasActive.exchange(active))
		Logger::Info(active ? "[AudioRelay] Frontend is playing the audio" : "[AudioRelay] Frontend stopped, playing audio here");
	if (active)
		std::memset(Data, 0, Frames * Channels * sizeof(float));
}
