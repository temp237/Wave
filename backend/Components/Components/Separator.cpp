#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dxgi1_6.h>

extern "C" {
#include <libavutil/tx.h>
}

#include <onnxruntime_cxx_api.h>
#include <dml_provider_factory.h>

#include <algorithm>
#include <cmath>

#include "Separator.hpp"

#pragma comment(lib, "dxgi.lib")

struct Separator::Session {
	Ort::Env Env{ ORT_LOGGING_LEVEL_ERROR, "WaveSeparator" };
	Ort::Session Run{ nullptr };
	std::string Input;
	std::string Output;
	AVTXContext* Forward = nullptr;
	av_tx_fn ForwardFn = nullptr;
	AVTXContext* Inverse = nullptr;
	av_tx_fn InverseFn = nullptr;

	~Session()
	{
		av_tx_uninit(&Forward);
		av_tx_uninit(&Inverse);
	}
};

Separator::Separator() = default;
Separator::~Separator() = default;

int Separator::PreferredAdapter()
{
	IDXGIFactory1* factory = nullptr;
	if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
		return 0;
	int best = 0;
	SIZE_T bestMemory = 0;
	IDXGIAdapter1* adapter = nullptr;
	for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++) {
		DXGI_ADAPTER_DESC1 desc{};
		adapter->GetDesc1(&desc);
		if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && desc.DedicatedVideoMemory > bestMemory) {
			bestMemory = desc.DedicatedVideoMemory;
			best = static_cast<int>(i);
		}
		adapter->Release();
	}
	factory->Release();
	return best;
}

bool Separator::Load(const std::filesystem::path& ModelPath, std::string& Error)
{
	try {
		auto session = std::make_unique<Session>();
		Ort::SessionOptions options;
		options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
		options.DisableMemPattern();
		options.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);

		const OrtDmlApi* dml = nullptr;
		bGpu = false;
		if (!Ort::GetApi().GetExecutionProviderApi("DML", ORT_API_VERSION, reinterpret_cast<const void**>(&dml)) && dml) {
			if (OrtStatus* status = dml->SessionOptionsAppendExecutionProvider_DML(options, PreferredAdapter()))
				Ort::GetApi().ReleaseStatus(status);
			else
				bGpu = true;
		}
		session->Run = Ort::Session(session->Env, ModelPath.c_str(), options);

		Ort::AllocatorWithDefaultOptions allocator;
		session->Input = session->Run.GetInputNameAllocated(0, allocator).get();
		session->Output = session->Run.GetOutputNameAllocated(0, allocator).get();

		const float forwardScale = 1.f;
		const float inverseScale = 1.f / FftSize;
		if (av_tx_init(&session->Forward, &session->ForwardFn, AV_TX_FLOAT_RDFT, 0, FftSize, &forwardScale, 0) < 0 ||
			av_tx_init(&session->Inverse, &session->InverseFn, AV_TX_FLOAT_RDFT, 1, FftSize, &inverseScale, 0) < 0) {
			Error = "couldn't set up the FFT";
			return false;
		}

		Window.resize(FftSize);
		for (int n = 0; n < FftSize; n++)
			Window[n] = static_cast<float>(0.5 - 0.5 * std::cos(2.0 * 3.14159265358979323846 * n / FftSize));
		Model = std::move(session);
		return true;
	}
	catch (const Ort::Exception& e) {
		Error = e.what();
		return false;
	}
}

void Separator::Stft(const float* Signal, float* Spectrum, int Channel, std::vector<float>& Scratch, std::vector<float>& Complex)
{
	const int padded = ChunkSize + FftSize;
	Scratch.resize(padded);
	for (int j = 0; j < Trim; j++)
		Scratch[j] = Signal[Trim - j];
	std::copy(Signal, Signal + ChunkSize, Scratch.begin() + Trim);
	for (int j = 0; j < Trim; j++)
		Scratch[Trim + ChunkSize + j] = Signal[ChunkSize - 2 - j];

	std::vector<float> frame(FftSize);
	Complex.resize(static_cast<size_t>(Bins) * 2);
	for (int t = 0; t < DimT; t++) {
		const float* start = Scratch.data() + static_cast<size_t>(t) * Hop;
		for (int n = 0; n < FftSize; n++)
			frame[n] = start[n] * Window[n];
		Model->ForwardFn(Model->Forward, Complex.data(), frame.data(), sizeof(float));
		for (int f = 0; f < DimF; f++) {
			Spectrum[(static_cast<size_t>(Channel * 2) * DimF + f) * DimT + t] = Complex[f * 2];
			Spectrum[(static_cast<size_t>(Channel * 2 + 1) * DimF + f) * DimT + t] = Complex[f * 2 + 1];
		}
	}
}

void Separator::Istft(const float* Spectrum, int Channel, float* Output, std::vector<float>& Scratch, std::vector<float>& Complex, std::vector<float>& Sum, std::vector<float>& Weight)
{
	const int padded = ChunkSize + FftSize;
	Sum.assign(padded, 0.f);
	Weight.assign(padded, 0.f);
	Scratch.resize(FftSize);
	Complex.resize(static_cast<size_t>(Bins) * 2);
	for (int t = 0; t < DimT; t++) {
		std::fill(Complex.begin(), Complex.end(), 0.f);
		for (int f = 0; f < DimF; f++) {
			Complex[f * 2] = Spectrum[(static_cast<size_t>(Channel * 2) * DimF + f) * DimT + t];
			Complex[f * 2 + 1] = Spectrum[(static_cast<size_t>(Channel * 2 + 1) * DimF + f) * DimT + t];
		}
		Complex[1] = 0.f;
		Model->InverseFn(Model->Inverse, Scratch.data(), Complex.data(), sizeof(float) * 2);
		const size_t start = static_cast<size_t>(t) * Hop;
		for (int n = 0; n < FftSize; n++) {
			Sum[start + n] += Scratch[n] * Window[n];
			Weight[start + n] += Window[n] * Window[n];
		}
	}
	for (int n = 0; n < ChunkSize; n++) {
		const float w = Weight[n + Trim];
		Output[n] = w > 1e-11f ? Sum[n + Trim] / w : 0.f;
	}
}

std::vector<float> Separator::Vocals(const std::vector<float>& Stereo, std::string& Error, const std::atomic<bool>* Cancel)
{
	std::vector<float> vocals;
	if (!Model) {
		Error = "the separation model isn't loaded";
		return vocals;
	}
	const size_t frames = Stereo.size() / 2;
	if (frames == 0)
		return vocals;
	const size_t pad = GenSize - frames % GenSize;
	const size_t total = frames + pad;
	std::vector<float> planar[2];
	for (int c = 0; c < 2; c++) {
		planar[c].assign(Trim + total + Trim, 0.f);
		for (size_t i = 0; i < frames; i++)
			planar[c][Trim + i] = Stereo[i * 2 + c];
	}

	std::vector<float> separated[2] = { std::vector<float>(total), std::vector<float>(total) };
	const size_t segments = total / GenSize;
	const size_t perItem = static_cast<size_t>(4) * DimF * DimT;
	std::vector<float> input;
	std::vector<float> scratch, complex, sum, weight, chunk(ChunkSize);
	Ort::MemoryInfo memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

	try {
		for (size_t first = 0; first < segments; first += Batch) {
			if (Cancel && Cancel->load()) {
				Error = "cancelled";
				return {};
			}
			const size_t count = std::min<size_t>(Batch, segments - first);
			input.assign(perItem * count, 0.f);
			for (size_t b = 0; b < count; b++) {
				const size_t offset = (first + b) * GenSize;
				for (int c = 0; c < 2; c++)
					Stft(planar[c].data() + offset, input.data() + perItem * b, c, scratch, complex);
			}
			const int64_t shape[4] = { static_cast<int64_t>(count), 4, DimF, DimT };
			Ort::Value tensor = Ort::Value::CreateTensor<float>(memory, input.data(), input.size(), shape, 4);
			const char* inputName = Model->Input.c_str();
			const char* outputName = Model->Output.c_str();
			std::vector<Ort::Value> result;
			{
				std::lock_guard lock(Lock);
				result = Model->Run.Run(Ort::RunOptions{ nullptr }, &inputName, &tensor, 1, &outputName, 1);
			}
			const float* output = result[0].GetTensorData<float>();
			for (size_t b = 0; b < count; b++) {
				const size_t offset = (first + b) * GenSize;
				for (int c = 0; c < 2; c++) {
					Istft(output + perItem * b, c, chunk.data(), scratch, complex, sum, weight);
					std::copy(chunk.begin() + Trim, chunk.begin() + Trim + GenSize, separated[c].begin() + offset);
				}
			}
		}
	}
	catch (const Ort::Exception& e) {
		Error = e.what();
		return {};
	}

	vocals.resize(frames * 2);
	for (size_t i = 0; i < frames; i++) {
		vocals[i * 2] = separated[0][i];
		vocals[i * 2 + 1] = separated[1][i];
	}
	return vocals;
}
