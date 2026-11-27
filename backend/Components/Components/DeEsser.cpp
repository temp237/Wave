#include <algorithm>
#include <cmath>

#include "DeEsser.hpp"
#include "Settings.hpp"

DeEsser::Processor::Biquad DeEsser::Processor::BandPass(float Frequency, float Q, uint32_t SampleRate)
{
	Biquad k;
	if (SampleRate == 0 || Frequency >= SampleRate * 0.45f)
		return k;
	const double w0 = 2.0 * 3.14159265358979323846 * Frequency / SampleRate;
	const double alpha = std::sin(w0) / (2.0 * Q);
	const double a0 = 1.0 + alpha;
	k.B0 = static_cast<float>(alpha / a0);
	k.B1 = 0.f;
	k.B2 = static_cast<float>(-alpha / a0);
	k.A1 = static_cast<float>(-2.0 * std::cos(w0) / a0);
	k.A2 = static_cast<float>((1.0 - alpha) / a0);
	return k;
}

float DeEsser::Processor::Run(const Biquad& k, std::array<float, 2>& z, float x)
{
	const float y = k.B0 * x + z[0];
	z[0] = k.B1 * x - k.A1 * y + z[1];
	z[1] = k.B2 * x - k.A2 * y;
	return y;
}

void DeEsser::Processor::Reset(uint32_t Channels, uint32_t SampleRate)
{
	Rate = SampleRate;
	BandState.assign(Channels, {});
	Scratch.assign(Channels, 0.f);
	for (size_t d = 0; d < DetectorCount; d++)
		Detectors[d] = BandPass(DetectorFrequencies[d], 2.f, SampleRate);
	DetectorState = {};
	DetectorEnergy = {};
	MonoEnergy = 0.f;
	HighBaseline = -20.f;
	BandEnvelope = 0.f;
	FullEnvelope = 0.f;
	AverageRatio = -20.f;
	TargetGain = 1.f;
	Gain = 1.f;
	Counter = 0;
	AppliedFrequency = 0.f;

	const auto coefficient = [&](double Seconds, double Step) {
		return static_cast<float>(1.0 - std::exp(-Step / (SampleRate * Seconds)));
	};
	EnvelopeAttack = coefficient(0.0005, 1.0);
	EnvelopeRelease = coefficient(0.03, 1.0);
	GainAttack = coefficient(0.001, 1.0);
	GainRelease = coefficient(0.06, 1.0);
	DetectorCoefficient = coefficient(0.005, 1.0);
	AverageCoefficient = coefficient(6.0, static_cast<double>(Block));
	BaselineFall = coefficient(0.4, static_cast<double>(Block));
	FrequencyCoefficient = coefficient(0.5, static_cast<double>(Block));
}

void DeEsser::Processor::UpdateBlock()
{
	const float level = 10.f * std::log10(FullEnvelope + 1e-12f);
	const float ratio = 10.f * std::log10((BandEnvelope + 1e-12f) / (FullEnvelope + 1e-12f));
	float cut = 0.f;
	if (level > -60.f) {
		AverageRatio += (ratio < AverageRatio ? BaselineFall : AverageCoefficient) * (ratio - AverageRatio);
		const float threshold = AverageRatio + 3.f;
		const float maxCut = Current.bAuto ? AutoMaxCut : -Current.Gain;
		cut = std::clamp((ratio - threshold) * 2.f, 0.f, maxCut);

		float highEnergy = 0.f;
		for (const float energy : DetectorEnergy)
			highEnergy += energy;
		const float highRatio = 10.f * std::log10((highEnergy + 1e-12f) / (MonoEnergy + 1e-12f));
		if (Current.bAuto)
			HighBaseline += (highRatio < HighBaseline ? BaselineFall : AverageCoefficient) * (highRatio - HighBaseline);
		if (Current.bAuto && highRatio > HighBaseline + 6.f) {
			const size_t best = static_cast<size_t>(std::max_element(DetectorEnergy.begin(), DetectorEnergy.end()) - DetectorEnergy.begin());
			const float current = std::log(Frequency);
			const float target = std::log(DetectorFrequencies[best]);
			Frequency = std::clamp(std::exp(current + FrequencyCoefficient * (target - current)), MinFrequency, MaxFrequency);
		}
	}
	TargetGain = std::pow(10.f, -cut / 20.f);

	if (std::fabs(Frequency - AppliedFrequency) > AppliedFrequency * 0.005f) {
		AppliedFrequency = Frequency;
		Band = BandPass(std::min(Frequency, Rate * 0.45f), 1.4f, Rate);
	}
}

void DeEsser::Processor::Process(float* Data, size_t Frames, uint32_t Channels, uint32_t SampleRate)
{
	if (!Data || Channels == 0 || SampleRate == 0)
		return;
	const uint32_t version = DeEsser::CurrentVersion();
	const bool bFormatChanged = SampleRate != Rate || BandState.size() != Channels;
	if (version != SeenVersion || bFormatChanged) {
		SeenVersion = version;
		Current = DeEsser::Get();
		if (bFormatChanged)
			Reset(Channels, SampleRate);
		if (!Current.bAuto)
			Frequency = Current.Frequency;
		bOn = Current.bEnabled && (Current.bAuto || Current.Gain < -0.05f);
		AppliedFrequency = 0.f;
	}
	if (!bOn) {
		Gain = TargetGain = 1.f;
		return;
	}
	if (AppliedFrequency == 0.f)
		UpdateBlock();

	for (size_t f = 0; f < Frames; f++) {
		float* frame = Data + f * Channels;
		float bandPower = 0.f;
		float fullPower = 0.f;
		float mono = 0.f;
		for (uint32_t c = 0; c < Channels; c++) {
			const float x = frame[c];
			const float y = Run(Band, BandState[c], x);
			Scratch[c] = y;
			bandPower = std::max(bandPower, y * y);
			fullPower = std::max(fullPower, x * x);
			mono += x;
		}
		if (Current.bAuto) {
			mono /= static_cast<float>(Channels);
			MonoEnergy += DetectorCoefficient * (mono * mono - MonoEnergy);
			for (size_t d = 0; d < DetectorCount; d++) {
				const float v = Run(Detectors[d], DetectorState[d], mono);
				DetectorEnergy[d] += DetectorCoefficient * (v * v - DetectorEnergy[d]);
			}
		}
		BandEnvelope += (bandPower > BandEnvelope ? EnvelopeAttack : EnvelopeRelease) * (bandPower - BandEnvelope);
		FullEnvelope += (fullPower > FullEnvelope ? EnvelopeAttack : EnvelopeRelease) * (fullPower - FullEnvelope);
		if (++Counter >= Block) {
			Counter = 0;
			UpdateBlock();
		}
		Gain += (TargetGain < Gain ? GainAttack : GainRelease) * (TargetGain - Gain);
		const float amount = Gain - 1.f;
		for (uint32_t c = 0; c < Channels; c++)
			frame[c] += amount * Scratch[c];
	}
}

void DeEsser::Set(const Settings& Value)
{
	{
		std::lock_guard lock(Mutex);
		Current = Value;
		Current.Gain = std::clamp(Current.Gain, MinGain, 0.f);
		Current.Frequency = std::clamp(Current.Frequency, MinFrequency, MaxFrequency);
	}
	Version++;
}

DeEsser::Settings DeEsser::Get()
{
	std::lock_guard lock(Mutex);
	return Current;
}

uint32_t DeEsser::CurrentVersion()
{
	return Version.load();
}

nlohmann::json DeEsser::ToJson()
{
	const Settings settings = Get();
	return {
		{ "enabled", settings.bEnabled },
		{ "auto", settings.bAuto },
		{ "gain", settings.Gain },
		{ "frequency", settings.Frequency },
		{ "minGain", MinGain },
		{ "minFrequency", MinFrequency },
		{ "maxFrequency", MaxFrequency },
	};
}

bool DeEsser::FromJson(const nlohmann::json& j)
{
	if (!j.is_object())
		return false;
	Settings settings = Get();
	settings.bEnabled = j.value("enabled", settings.bEnabled);
	settings.bAuto = j.value("auto", settings.bAuto);
	settings.Gain = j.value("gain", settings.Gain);
	settings.Frequency = j.value("frequency", settings.Frequency);
	if (!j.contains("enabled") && j.contains("auto"))
		settings.bEnabled = settings.bAuto || settings.Gain < -0.05f;
	Set(settings);
	return true;
}

void DeEsser::Load()
{
	const nlohmann::json saved = ::Settings::Get("deesser");
	if (!saved.empty())
		FromJson(saved);
}

bool DeEsser::Save()
{
	return ::Settings::Set("deesser", ToJson());
}
