#pragma once
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

struct IPropertyStore;
struct _tagpropertykey;
struct IWICImagingFactory;
struct IWICBitmapSource;

struct TrackMetadata {
	std::string Title;
	std::string Artist;
	std::string Album;
	double Duration = 0;
};

struct CoverImage {
	std::string Jpeg;
	uint64_t Hash = 0;
	uint32_t Color = 0;
	uint32_t Accent = 0;
};

class Metadata
{
public:
	Metadata() = delete;

	static TrackMetadata Read(const std::filesystem::path& Path);
	static std::optional<CoverImage> MakeCover(const std::filesystem::path& Path, int Size = 256);
	static std::optional<CoverImage> CoverFromBytes(const std::string& Bytes, int Size = 256);
	static int HashDistance(uint64_t A, uint64_t B);
	static int ColorDistance(uint32_t A, uint32_t B);

	static std::string ImageMime(const std::string& Bytes);
	static size_t ImageStart(const std::string& Bytes);

private:
	static std::string ReadEmbeddedBytes(const std::filesystem::path& Path);
	static std::optional<CoverImage> EncodeCover(IWICImagingFactory* Factory, IWICBitmapSource* Source, int Size);
	static uint64_t PerceptualHash(IWICImagingFactory* Factory, IWICBitmapSource* Source);
	static uint32_t AverageColor(IWICImagingFactory* Factory, IWICBitmapSource* Source);
	static uint32_t AccentColor(IWICImagingFactory* Factory, IWICBitmapSource* Source);
	static std::string ReadString(IPropertyStore* Store, const _tagpropertykey& Key);
};
