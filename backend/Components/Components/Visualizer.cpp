extern "C" {
#include <libavutil/tx.h>
}

#include <algorithm>
#include <chrono>
#include <cmath>

#include "Visualizer.hpp"
#include "AudioRelay.hpp"

void Visualizer::Start(std::function<void(const Frame&)> Sink)
{
	if (bRunning.exchange(true))
		return;
	Output = std::move(Sink);
	Worker = std::thread(Run);
}

void Visualizer::Stop()
{
	bRunning = false;
	Wake.notify_all();
	if (Worker.joinable())
		Worker.join();
	Output = {};
}

void Visualizer::SetEnabled(bool bOn)
{
	bEnabled = bOn;
	Wake.notify_all();
}

bool Visualizer::Analyze(Frame& Out)
{
	static thread_local AVTXContext* context = nullptr;
	static thread_local av_tx_fn transform = nullptr;
	static thread_local std::vector<float> window;
	static thread_local std::vector<float> samples;
	static thread_local std::vector<float> spectrum;
	if (!context) {
		const float scale = 1.f;
		if (av_tx_init(&context, &transform, AV_TX_FLOAT_RDFT, 0, FftSize, &scale, 0) < 0)
			return false;
		window.resize(FftSize);
		for (int n = 0; n < FftSize; n++)
			window[n] = static_cast<float>(0.5 - 0.5 * std::cos(2.0 * 3.14159265358979323846 * n / FftSize));
		spectrum.resize(FftSize + 2);
	}

	uint32_t rate = 0;
	if (!AudioRelay::ReadPlaying(FftSize, samples, rate) || rate == 0)
		return false;
	for (int n = 0; n < FftSize; n++)
		samples[n] *= window[n];
	transform(context, spectrum.data(), samples.data(), sizeof(float));

	const float binHz = static_cast<float>(rate) / FftSize;
	const float top = std::min(MaxFrequency, rate * 0.45f);
	const float ratio = std::pow(top / MinFrequency, 1.f / Bands);
	const float norm = 4.f / FftSize;
	float low = MinFrequency;
	for (size_t b = 0; b < Bands; b++) {
		const float high = low * ratio;
		const float center = std::sqrt(low * high);
		const auto magnitude = [&](int k) {
			const float re = spectrum[k * 2];
			const float im = spectrum[k * 2 + 1];
			return std::sqrt(re * re + im * im);
		};
		float peak = 0.f;
		if (high - low < 2.f * binHz) {
			const float position = std::clamp(center / binHz, 1.f, FftSize / 2.f - 1.f);
			const int k = static_cast<int>(position);
			const float frac = position - k;
			peak = magnitude(k) * (1.f - frac) + magnitude(k + 1) * frac;
		}
		else {
			const int first = std::clamp(static_cast<int>(std::floor(low / binHz)), 1, FftSize / 2);
			const int last = std::clamp(static_cast<int>(std::ceil(high / binHz)), first + 1, FftSize / 2 + 1);
			for (int k = first; k < last; k++)
				peak = std::max(peak, magnitude(k));
		}
		const float db = 20.f * std::log10(peak * norm + 1e-9f) + TiltDbPerOctave * std::log2(center / 1000.f);
		const float level = std::clamp((db - FloorDb) / RangeDb, 0.f, 1.f);
		Out[b] = static_cast<uint8_t>(std::lround(level * 255.f));
		low = high;
	}
	return true;
}

void Visualizer::Run()
{
	Frame frame{};
	bool bSentSilence = true;
	while (bRunning) {
		{
			std::unique_lock lock(Mutex);
			Wake.wait(lock, [] { return !bRunning || bEnabled; });
		}
		if (!bRunning)
			break;
		const auto started = std::chrono::steady_clock::now();
		if (Analyze(frame)) {
			bSentSilence = false;
			if (Output)
				Output(frame);
		}
		else if (!bSentSilence) {
			frame.fill(0);
			bSentSilence = true;
			if (Output)
				Output(frame);
		}
		std::unique_lock lock(Mutex);
		Wake.wait_until(lock, started + std::chrono::milliseconds(IntervalMs), [] { return !bRunning || !bEnabled; });
	}
}
