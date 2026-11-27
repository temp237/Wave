#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>

class ModelStore
{
public:
	struct Model {
		const wchar_t* File;
		const wchar_t* Url;
		uint64_t Size;
		const char* Sha256;
	};

	static constexpr Model Separation = {
		L"Kim_Vocal_2.onnx",
		L"https://github.com/TRvlvr/model_repo/releases/download/all_public_uvr_models/Kim_Vocal_2.onnx",
		66759214,
		"ce74ef3b6a6024ce44211a07be9cf8bc6d87728cc852a68ab34eb8e58cde9c8b"
	};
	static constexpr Model Whisper = {
		L"ggml-large-v3-turbo-q5_0.bin",
		L"https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-large-v3-turbo-q5_0.bin",
		574041195,
		"394221709cd5ad1f40c46e6031ca61bce88931e6e088c188294c6d5a55ffa7e2"
	};
	static constexpr std::array<Model, 2> Required = { Separation, Whisper };

	ModelStore() = delete;

	static bool Ensure(const std::filesystem::path& Directory, const std::atomic<bool>& Cancel, std::string& Error);

private:
	static constexpr const wchar_t* PartSuffix = L".part";
	static constexpr int ProgressStep = 10;

	static bool IsPresent(const std::filesystem::path& Path, uint64_t Size);
	static bool Download(const Model& Item, const std::filesystem::path& Target, const std::atomic<bool>& Cancel, std::string& Error);
	static std::string Sha256(const std::filesystem::path& Path);
};
