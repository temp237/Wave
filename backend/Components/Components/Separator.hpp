#pragma once
#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct AVTXContext;
struct OrtEnv;

class Separator
{
public:
	static constexpr int SampleRate = 44100;

	Separator();
	~Separator();
	Separator(const Separator&) = delete;
	Separator& operator=(const Separator&) = delete;

	bool Load(const std::filesystem::path& Model, std::string& Error);
	bool IsGpu() const { return bGpu; }
	std::vector<float> Vocals(const std::vector<float>& Stereo, std::string& Error, const std::atomic<bool>* Cancel = nullptr);

private:
	static constexpr int FftSize = 7680;
	static constexpr int Hop = 1024;
	static constexpr int Bins = FftSize / 2 + 1;
	static constexpr int DimF = 3072;
	static constexpr int DimT = 256;
	static constexpr int ChunkSize = Hop * (DimT - 1);
	static constexpr int Trim = FftSize / 2;
	static constexpr int GenSize = ChunkSize - 2 * Trim;
	static constexpr int Batch = 1;

	struct Session;

	static int PreferredAdapter();
	void Stft(const float* Signal, float* Spectrum, int Channel, std::vector<float>& Scratch, std::vector<float>& Complex);
	void Istft(const float* Spectrum, int Channel, float* Output, std::vector<float>& Scratch, std::vector<float>& Complex, std::vector<float>& Sum, std::vector<float>& Weight);

	std::unique_ptr<Session> Model;
	std::vector<float> Window;
	std::mutex Lock;
	bool bGpu = false;
};
