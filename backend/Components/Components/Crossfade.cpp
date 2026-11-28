#include <algorithm>
#include <cstring>

#include "Crossfade.hpp"
#include "AudioRelay.hpp"
#include "MicRelay.hpp"

float Crossfade::Curve(double Progress)
{
	const double t = std::clamp(Progress, 0.0, 1.0);
	return static_cast<float>(t * t * (3.0 - 2.0 * t));
}

void Crossfade::Reset(int Writer)
{
	std::lock_guard lock(Mutex);
	Phase = Stage::Idle;
	WriterSlot = Writer;
	OutSlot = -1;
	InSlot = -1;
	bFadingIn = false;
	bMixing = false;
	Backlog.clear();
	BacklogRead = 0;
	BacklogConsumed = 0;
	BacklogTime = -1;
	OutEndTime = -1;
	Offset = 0;
	Finished = -1;
}

void Crossfade::Begin(int Outgoing, int Incoming, double Duration)
{
	std::lock_guard lock(Mutex);
	Phase = Stage::Fading;
	WriterSlot = Outgoing;
	OutSlot = Outgoing;
	InSlot = Incoming;
	Seconds = std::clamp(Duration, 0.1, MaxSeconds);
	OutFrames = 0;
	InFrames = 0;
	bFadingIn = true;
	bMixing = false;
	Backlog.clear();
	BacklogRead = 0;
	BacklogConsumed = 0;
	BacklogTime = -1;
	OutEndTime = -1;
	Finished = -1;
}

void Crossfade::OutgoingEnded()
{
	std::lock_guard lock(Mutex);
	if (Phase != Stage::Fading)
		return;
	Phase = Stage::Handoff;
	Finished = OutSlot;
}

void Crossfade::Flush(int Slot)
{
	std::lock_guard lock(Mutex);
	if (Phase != Stage::Fading || Slot != InSlot)
		return;
	Backlog.clear();
	BacklogRead = 0;
	BacklogConsumed = 0;
	BacklogTime = -1;
	InFrames = 0;
	bFadingIn = true;
	bMixing = false;
}

bool Crossfade::IsActive()
{
	std::lock_guard lock(Mutex);
	return Phase != Stage::Idle;
}

int Crossfade::Writer()
{
	return WriterSlot;
}

double Crossfade::OffsetSeconds()
{
	return static_cast<double>(Offset.load()) / Ticks;
}

int Crossfade::TakeFinished()
{
	return Finished.exchange(-1);
}

void Crossfade::ApplyFadeIn(float* Data, size_t Frames, uint32_t Channels, uint32_t SampleRate)
{
	if (!bFadingIn)
		return;
	const double total = Seconds * SampleRate;
	for (size_t f = 0; f < Frames; f++) {
		const float gain = Curve((InFrames + f) / total);
		for (uint32_t c = 0; c < Channels; c++)
			Data[f * Channels + c] *= gain;
	}
	InFrames += Frames;
	if (InFrames >= total)
		bFadingIn = false;
}

void Crossfade::Emit(float* Data, size_t Frames, uint32_t Channels, uint32_t SampleRate, int64_t SampleTime)
{
	const int64_t time = SampleTime >= 0 ? SampleTime + Offset.load() : -1;
	MicRelay::Write(Data, Frames, Channels, SampleRate, time);
	AudioRelay::Write(Data, Frames, Channels, SampleRate, time);
}

int64_t Crossfade::BacklogHead()
{
	if (BacklogTime < 0 || BacklogRate == 0)
		return -1;
	return BacklogTime + static_cast<int64_t>(BacklogConsumed * Ticks / BacklogRate);
}

void Crossfade::CompleteHandoff(int64_t SampleTime)
{
	const size_t pending = BacklogChannels && BacklogRead < Backlog.size() ? (Backlog.size() - BacklogRead) / BacklogChannels : 0;
	const int64_t head = pending ? BacklogHead() : SampleTime;
	if (OutEndTime >= 0 && head >= 0)
		Offset += OutEndTime - head;
	WriterSlot = InSlot;
	if (pending)
		Emit(Backlog.data() + BacklogRead, pending, BacklogChannels, BacklogRate, head);
	Backlog.clear();
	BacklogRead = 0;
	BacklogConsumed = 0;
	BacklogTime = -1;
	Phase = Stage::Idle;
	OutSlot = -1;
	bMixing = false;
}

void Crossfade::Process(int Slot, float* Data, size_t Frames, uint32_t Channels, uint32_t SampleRate, int64_t SampleTime)
{
	if (!Data || Frames == 0 || Channels == 0 || SampleRate == 0)
		return;
	std::lock_guard lock(Mutex);
	if (Phase == Stage::Handoff && Slot == InSlot)
		CompleteHandoff(SampleTime);

	if (Phase == Stage::Idle) {
		if (Slot != WriterSlot) {
			std::memset(Data, 0, Frames * Channels * sizeof(float));
			return;
		}
		ApplyFadeIn(Data, Frames, Channels, SampleRate);
		Emit(Data, Frames, Channels, SampleRate, SampleTime);
		return;
	}

	if (Phase == Stage::Fading && Slot == InSlot) {
		ApplyFadeIn(Data, Frames, Channels, SampleRate);
		if (BacklogRead >= Backlog.size()) {
			Backlog.clear();
			BacklogRead = 0;
			BacklogConsumed = 0;
			BacklogTime = SampleTime;
			BacklogChannels = Channels;
			BacklogRate = SampleRate;
		}
		if (Channels == BacklogChannels && SampleRate == BacklogRate)
			Backlog.insert(Backlog.end(), Data, Data + Frames * Channels);
		std::memset(Data, 0, Frames * Channels * sizeof(float));
		return;
	}

	if (Phase == Stage::Fading && Slot == OutSlot) {
		const double total = Seconds * SampleRate;
		for (size_t f = 0; f < Frames; f++) {
			const float gain = 1.f - Curve((OutFrames + f) / total);
			for (uint32_t c = 0; c < Channels; c++)
				Data[f * Channels + c] *= gain;
		}
		OutFrames += Frames;

		const bool bSameFormat = BacklogChannels == Channels && BacklogRate == SampleRate;
		const size_t available = BacklogChannels && BacklogRead < Backlog.size() ? (Backlog.size() - BacklogRead) / BacklogChannels : 0;
		if (!bMixing && bSameFormat && available >= static_cast<size_t>(CushionSeconds * SampleRate))
			bMixing = true;
		if (bMixing && bSameFormat) {
			const size_t count = std::min(available, Frames);
			const float* source = Backlog.data() + BacklogRead;
			for (size_t i = 0; i < count * Channels; i++)
				Data[i] += source[i];
			BacklogRead += count * Channels;
			BacklogConsumed += count;
		}
		Emit(Data, Frames, Channels, SampleRate, SampleTime);
		if (SampleTime >= 0)
			OutEndTime = SampleTime + static_cast<int64_t>(Frames * Ticks / SampleRate);
		if (OutFrames >= total || (BacklogChannels && !bSameFormat)) {
			Phase = Stage::Handoff;
			Finished = OutSlot;
		}
		return;
	}

	std::memset(Data, 0, Frames * Channels * sizeof(float));
}
