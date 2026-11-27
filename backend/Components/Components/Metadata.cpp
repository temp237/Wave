#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shobjidl_core.h>
#include <propkey.h>
#include <propvarutil.h>
#include <wincodec.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <fstream>
#include <vector>

#include "Metadata.hpp"
#include "Utils.hpp"

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "propsys.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

std::string Metadata::ReadString(IPropertyStore* Store, const PROPERTYKEY& Key)
{
    PROPVARIANT value;
    PropVariantInit(&value);
    std::string out;
    if (SUCCEEDED(Store->GetValue(Key, &value))) {
        if (value.vt == VT_LPWSTR) {
            out = Utils::WideToUtf8(value.pwszVal);
        }
        else if (value.vt == (VT_VECTOR | VT_LPWSTR)) {
            for (ULONG i = 0; i < value.calpwstr.cElems; i++) {
                const std::string part = Utils::WideToUtf8(value.calpwstr.pElems[i]);
                if (part.empty())
                    continue;
                if (!out.empty())
                    out += ", ";
                out += part;
            }
        }
    }
    PropVariantClear(&value);
    return out;
}

std::string Metadata::ImageMime(const std::string& Bytes)
{
    if (Bytes.size() >= 4 && static_cast<unsigned char>(Bytes[0]) == 0x89 && Bytes.compare(1, 3, "PNG") == 0)
        return "image/png";
    if (Bytes.size() >= 3 && static_cast<unsigned char>(Bytes[0]) == 0xFF && static_cast<unsigned char>(Bytes[1]) == 0xD8)
        return "image/jpeg";
    if (Bytes.size() >= 6 && (Bytes.compare(0, 6, "GIF87a") == 0 || Bytes.compare(0, 6, "GIF89a") == 0))
        return "image/gif";
    if (Bytes.size() >= 12 && Bytes.compare(0, 4, "RIFF") == 0 && Bytes.compare(8, 4, "WEBP") == 0)
        return "image/webp";
    if (Bytes.size() >= 2 && Bytes.compare(0, 2, "BM") == 0)
        return "image/bmp";
    return "image/png";
}

size_t Metadata::ImageStart(const std::string& Bytes)
{
    const size_t limit = std::min<size_t>(Bytes.size(), 1024);
    for (size_t i = 0; i < limit && i + 12 <= Bytes.size(); i++) {
        const unsigned char* b = reinterpret_cast<const unsigned char*>(Bytes.data() + i);
        if (b[0] == 0x89 && std::memcmp(b + 1, "PNG", 3) == 0)
            return i;
        if (b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF)
            return i;
        if (std::memcmp(b, "GIF8", 4) == 0)
            return i;
        if (std::memcmp(b, "RIFF", 4) == 0 && std::memcmp(b + 8, "WEBP", 4) == 0)
            return i;
    }
    return std::string::npos;
}

TrackMetadata Metadata::Read(const std::filesystem::path& Path)
{
    TrackMetadata meta;
    IPropertyStore* store = nullptr;
    if (FAILED(SHGetPropertyStoreFromParsingName(Path.c_str(), nullptr, GPS_DEFAULT, IID_PPV_ARGS(&store))))
        return meta;

    meta.Title = ReadString(store, PKEY_Title);
    meta.Artist = ReadString(store, PKEY_Music_Artist);
    if (meta.Artist.empty())
        meta.Artist = ReadString(store, PKEY_Music_AlbumArtist);
    meta.Album = ReadString(store, PKEY_Music_AlbumTitle);

    PROPVARIANT value;
    PropVariantInit(&value);
    if (SUCCEEDED(store->GetValue(PKEY_Media_Duration, &value)) && value.vt == VT_UI8)
        meta.Duration = static_cast<double>(value.uhVal.QuadPart) / 10'000'000.0;
    PropVariantClear(&value);

    store->Release();
    return meta;
}

std::string Metadata::ReadEmbeddedBytes(const std::filesystem::path& Path)
{
    IPropertyStore* store = nullptr;
    if (FAILED(SHGetPropertyStoreFromParsingName(Path.c_str(), nullptr, GPS_DEFAULT, IID_PPV_ARGS(&store))))
        return {};

    std::string bytes;
    PROPVARIANT value;
    PropVariantInit(&value);
    if (SUCCEEDED(store->GetValue(PKEY_ThumbnailStream, &value)) && value.vt == VT_STREAM && value.pStream) {
        LARGE_INTEGER start{};
        value.pStream->Seek(start, STREAM_SEEK_SET, nullptr);
        char buffer[16384];
        ULONG read = 0;
        while (SUCCEEDED(value.pStream->Read(buffer, sizeof(buffer), &read)) && read > 0)
            bytes.append(buffer, read);
    }
    PropVariantClear(&value);
    store->Release();

    const size_t start = ImageStart(bytes);
    if (start == std::string::npos)
        return {};
    bytes.erase(0, start);
    return bytes;
}

uint64_t Metadata::PerceptualHash(IWICImagingFactory* Factory, IWICBitmapSource* Source)
{
    constexpr UINT N = 32;
    IWICBitmapScaler* scaler = nullptr;
    IWICFormatConverter* gray = nullptr;
    std::vector<BYTE> pixels(N * N);
    const bool ok = SUCCEEDED(Factory->CreateBitmapScaler(&scaler))
        && SUCCEEDED(scaler->Initialize(Source, N, N, WICBitmapInterpolationModeFant))
        && SUCCEEDED(Factory->CreateFormatConverter(&gray))
        && SUCCEEDED(gray->Initialize(scaler, GUID_WICPixelFormat8bppGray, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))
        && SUCCEEDED(gray->CopyPixels(nullptr, N, N * N, pixels.data()));
    if (gray) gray->Release();
    if (scaler) scaler->Release();
    if (!ok)
        return 0;

    static const auto cosines = [] {
        std::array<std::array<double, N>, 8> table{};
        for (int u = 0; u < 8; u++)
            for (UINT x = 0; x < N; x++)
                table[u][x] = std::cos((2.0 * x + 1.0) * u * 3.14159265358979323846 / (2.0 * N));
        return table;
    }();

    std::array<double, 64> dct{};
    for (int v = 0; v < 8; v++) {
        for (int u = 0; u < 8; u++) {
            double sum = 0;
            for (UINT y = 0; y < N; y++)
                for (UINT x = 0; x < N; x++)
                    sum += pixels[y * N + x] * cosines[u][x] * cosines[v][y];
            dct[v * 8 + u] = sum;
        }
    }

    std::array<double, 63> ac{};
    std::copy(dct.begin() + 1, dct.end(), ac.begin());
    std::nth_element(ac.begin(), ac.begin() + ac.size() / 2, ac.end());
    const double median = ac[ac.size() / 2];

    uint64_t hash = 0;
    for (int i = 0; i < 64; i++) {
        if (dct[i] > median)
            hash |= uint64_t(1) << i;
    }
    return hash;
}

int Metadata::HashDistance(uint64_t A, uint64_t B)
{
    return static_cast<int>(std::popcount(A ^ B));
}

int Metadata::ColorDistance(uint32_t A, uint32_t B)
{
    int worst = 0;
    for (int shift = 0; shift <= 16; shift += 8)
        worst = std::max(worst, std::abs(static_cast<int>((A >> shift) & 0xFF) - static_cast<int>((B >> shift) & 0xFF)));
    return worst;
}

uint32_t Metadata::AverageColor(IWICImagingFactory* Factory, IWICBitmapSource* Source)
{
    constexpr UINT N = 8;
    IWICBitmapScaler* scaler = nullptr;
    IWICFormatConverter* rgb = nullptr;
    std::vector<BYTE> pixels(N * N * 3);
    const bool ok = SUCCEEDED(Factory->CreateBitmapScaler(&scaler))
        && SUCCEEDED(scaler->Initialize(Source, N, N, WICBitmapInterpolationModeFant))
        && SUCCEEDED(Factory->CreateFormatConverter(&rgb))
        && SUCCEEDED(rgb->Initialize(scaler, GUID_WICPixelFormat24bppRGB, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))
        && SUCCEEDED(rgb->CopyPixels(nullptr, N * 3, static_cast<UINT>(pixels.size()), pixels.data()));
    if (rgb) rgb->Release();
    if (scaler) scaler->Release();
    if (!ok)
        return 0;

    uint32_t sum[3] = {};
    for (size_t i = 0; i < pixels.size(); i += 3)
        for (int c = 0; c < 3; c++)
            sum[c] += pixels[i + c];
    const uint32_t count = N * N;
    return ((sum[0] / count) << 16) | ((sum[1] / count) << 8) | (sum[2] / count);
}

uint32_t Metadata::AccentColor(IWICImagingFactory* Factory, IWICBitmapSource* Source)
{
    constexpr UINT N = 32;
    constexpr int Bins = 24;
    constexpr double MinValue = 0.18;
    constexpr double MinChroma = 0.08;
    constexpr double NeutralDominance = 1.5;
    IWICBitmapScaler* scaler = nullptr;
    IWICFormatConverter* rgb = nullptr;
    std::vector<BYTE> pixels(N * N * 3);
    const bool ok = SUCCEEDED(Factory->CreateBitmapScaler(&scaler))
        && SUCCEEDED(scaler->Initialize(Source, N, N, WICBitmapInterpolationModeFant))
        && SUCCEEDED(Factory->CreateFormatConverter(&rgb))
        && SUCCEEDED(rgb->Initialize(scaler, GUID_WICPixelFormat24bppRGB, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))
        && SUCCEEDED(rgb->CopyPixels(nullptr, N * 3, static_cast<UINT>(pixels.size()), pixels.data()));
    if (rgb) rgb->Release();
    if (scaler) scaler->Release();
    if (!ok)
        return 0;

    struct Sample { double R, G, B, Weight; int Bin; };
    std::vector<Sample> colorful;
    double weights[Bins] = {};
    double lit[3] = {};
    double litCount = 0.0;
    double neutral[3] = {};
    double neutralCount = 0.0;
    double all[3] = {};
    for (size_t i = 0; i < pixels.size(); i += 3) {
        const double r = pixels[i] / 255.0, g = pixels[i + 1] / 255.0, b = pixels[i + 2] / 255.0;
        all[0] += r; all[1] += g; all[2] += b;
        const double max = std::max({ r, g, b });
        const double min = std::min({ r, g, b });
        if (max < MinValue)
            continue;
        lit[0] += r; lit[1] += g; lit[2] += b;
        litCount += 1.0;
        const double chroma = max - min;
        if (chroma < MinChroma) {
            neutral[0] += r; neutral[1] += g; neutral[2] += b;
            neutralCount += 1.0;
            continue;
        }
        double hue = max == r ? std::fmod((g - b) / chroma, 6.0) : max == g ? (b - r) / chroma + 2.0 : (r - g) / chroma + 4.0;
        if (hue < 0.0)
            hue += 6.0;
        const int bin = std::min(Bins - 1, static_cast<int>(hue / 6.0 * Bins));
        const double weight = chroma * max;
        weights[bin] += weight;
        colorful.push_back({ r, g, b, weight, bin });
    }

    const auto pack = [](double r, double g, double b) {
        const auto channel = [](double v) { return static_cast<uint32_t>(std::lround(std::clamp(v, 0.0, 1.0) * 255.0)); };
        return (channel(r) << 16) | (channel(g) << 8) | channel(b);
    };
    if (!colorful.empty()) {
        int best = 0;
        double bestWeight = -1.0;
        for (int b = 0; b < Bins; b++) {
            const double around = weights[b] + 0.5 * (weights[(b + 1) % Bins] + weights[(b + Bins - 1) % Bins]);
            if (around > bestWeight) {
                bestWeight = around;
                best = b;
            }
        }
        double sum[3] = {};
        double total = 0.0;
        double area = 0.0;
        for (const Sample& s : colorful) {
            const int distance = std::min((s.Bin - best + Bins) % Bins, (best - s.Bin + Bins) % Bins);
            if (distance > 1)
                continue;
            sum[0] += s.R * s.Weight; sum[1] += s.G * s.Weight; sum[2] += s.B * s.Weight;
            total += s.Weight;
            area += 1.0;
        }
        if (total > 0.0 && neutralCount < area * NeutralDominance)
            return pack(sum[0] / total, sum[1] / total, sum[2] / total);
    }
    if (neutralCount > 0.0)
        return pack(neutral[0] / neutralCount, neutral[1] / neutralCount, neutral[2] / neutralCount);
    if (litCount > 0.0)
        return pack(lit[0] / litCount, lit[1] / litCount, lit[2] / litCount);
    const double count = static_cast<double>(N * N);
    return pack(all[0] / count, all[1] / count, all[2] / count);
}

std::optional<CoverImage> Metadata::EncodeCover(IWICImagingFactory* Factory, IWICBitmapSource* Source, int Size)
{
    UINT width = 0, height = 0;
    if (FAILED(Source->GetSize(&width, &height)) || width == 0 || height == 0)
        return std::nullopt;

    const double scale = std::min(1.0, static_cast<double>(Size) / static_cast<double>(std::max(width, height)));
    const UINT outW = std::max<UINT>(1, static_cast<UINT>(width * scale));
    const UINT outH = std::max<UINT>(1, static_cast<UINT>(height * scale));

    bool ok = false;
    IWICBitmapScaler* scaler = nullptr;
    IWICFormatConverter* converter = nullptr;
    IStream* memory = nullptr;
    IWICBitmapEncoder* encoder = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    IPropertyBag2* options = nullptr;
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;

    if (SUCCEEDED(Factory->CreateBitmapScaler(&scaler))
        && SUCCEEDED(scaler->Initialize(Source, outW, outH, WICBitmapInterpolationModeFant))
        && SUCCEEDED(Factory->CreateFormatConverter(&converter))
        && SUCCEEDED(converter->Initialize(scaler, GUID_WICPixelFormat24bppBGR, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))
        && SUCCEEDED(CreateStreamOnHGlobal(nullptr, TRUE, &memory))
        && SUCCEEDED(Factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder))
        && SUCCEEDED(encoder->Initialize(memory, WICBitmapEncoderNoCache))
        && SUCCEEDED(encoder->CreateNewFrame(&frame, &options))) {
        PROPBAG2 option{};
        option.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
        VARIANT quality;
        VariantInit(&quality);
        quality.vt = VT_R4;
        quality.fltVal = 0.85f;
        options->Write(1, &option, &quality);
        ok = SUCCEEDED(frame->Initialize(options))
            && SUCCEEDED(frame->SetSize(outW, outH))
            && SUCCEEDED(frame->SetPixelFormat(&format))
            && SUCCEEDED(frame->WriteSource(converter, nullptr))
            && SUCCEEDED(frame->Commit())
            && SUCCEEDED(encoder->Commit());
    }

    CoverImage cover;
    if (ok) {
        STATSTG stat{};
        HGLOBAL global = nullptr;
        ok = SUCCEEDED(memory->Stat(&stat, STATFLAG_NONAME)) && SUCCEEDED(GetHGlobalFromStream(memory, &global));
        if (ok) {
            const void* data = GlobalLock(global);
            cover.Jpeg.assign(static_cast<const char*>(data), static_cast<size_t>(stat.cbSize.QuadPart));
            GlobalUnlock(global);
            cover.Hash = PerceptualHash(Factory, converter);
            cover.Color = AverageColor(Factory, converter);
            cover.Accent = AccentColor(Factory, converter);
        }
    }

    if (options) options->Release();
    if (frame) frame->Release();
    if (encoder) encoder->Release();
    if (memory) memory->Release();
    if (converter) converter->Release();
    if (scaler) scaler->Release();

    if (!ok || cover.Jpeg.empty())
        return std::nullopt;
    return cover;
}

std::optional<CoverImage> Metadata::CoverFromBytes(const std::string& Bytes, int Size)
{
    const size_t start = ImageStart(Bytes);
    if (start == std::string::npos)
        return std::nullopt;

    std::optional<CoverImage> cover;
    IWICImagingFactory* factory = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;

    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))
        && SUCCEEDED(factory->CreateStream(&stream))
        && SUCCEEDED(stream->InitializeFromMemory(reinterpret_cast<BYTE*>(const_cast<char*>(Bytes.data() + start)), static_cast<DWORD>(Bytes.size() - start)))
        && SUCCEEDED(factory->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnDemand, &decoder))
        && SUCCEEDED(decoder->GetFrame(0, &frame)))
        cover = EncodeCover(factory, frame, Size);

    if (frame) frame->Release();
    if (decoder) decoder->Release();
    if (stream) stream->Release();
    if (factory) factory->Release();
    return cover;
}

std::optional<CoverImage> Metadata::MakeCover(const std::filesystem::path& Path, int Size)
{
    if (auto cover = CoverFromBytes(ReadEmbeddedBytes(Path), Size))
        return cover;

    IShellItemImageFactory* shellFactory = nullptr;
    if (FAILED(SHCreateItemFromParsingName(Path.c_str(), nullptr, IID_PPV_ARGS(&shellFactory))))
        return std::nullopt;

    HBITMAP bitmap = nullptr;
    const HRESULT hr = shellFactory->GetImage({ Size, Size }, SIIGBF_THUMBNAILONLY | SIIGBF_BIGGERSIZEOK, &bitmap);
    shellFactory->Release();
    if (FAILED(hr) || !bitmap)
        return std::nullopt;

    std::optional<CoverImage> cover;
    IWICImagingFactory* factory = nullptr;
    IWICBitmap* source = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))
        && SUCCEEDED(factory->CreateBitmapFromHBITMAP(bitmap, nullptr, WICBitmapIgnoreAlpha, &source)))
        cover = EncodeCover(factory, source, Size);

    if (source) source->Release();
    if (factory) factory->Release();
    DeleteObject(bitmap);
    return cover;
}
