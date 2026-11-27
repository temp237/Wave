#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mftransform.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include "Equalizer.hpp"
#include "DeEsser.hpp"
#include "Normalize.hpp"
#include "Settings.hpp"
#include "Logger.hpp"
#include "MicRelay.hpp"
#include "AudioRelay.hpp"

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")

class Equalizer::Transform : public IMFTransform
{
private:
    struct Coefficients {
        float B0 = 1, B1 = 0, B2 = 0, A1 = 0, A2 = 0;
    };

    std::atomic<ULONG> RefCount = 1;
    std::mutex Lock;
    IMFMediaType* InputType = nullptr;
    IMFMediaType* OutputType = nullptr;
    IMFSample* Pending = nullptr;
    UINT32 Channels = 0;
    UINT32 SampleRate = 0;
    uint32_t SeenVersion = 0;
    bool bActive = false;
    float PreampGain = 1.f;
    std::array<Coefficients, BandCount> Bands{};
    std::array<bool, BandCount> BandOn{};
    std::vector<std::array<std::array<float, 2>, BandCount>> History;
    DeEsser::Processor DeEss;
    Normalize::Processor Level;

    static HRESULT CheckType(IMFMediaType* Type, UINT32& OutChannels, UINT32& OutRate)
    {
        GUID major{}, subtype{};
        UINT32 bits = 0;
        if (FAILED(Type->GetGUID(MF_MT_MAJOR_TYPE, &major)) || major != MFMediaType_Audio)
            return MF_E_INVALIDMEDIATYPE;
        if (FAILED(Type->GetGUID(MF_MT_SUBTYPE, &subtype)) || subtype != MFAudioFormat_Float)
            return MF_E_INVALIDMEDIATYPE;
        if (SUCCEEDED(Type->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bits)) && bits != 32)
            return MF_E_INVALIDMEDIATYPE;
        if (FAILED(Type->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &OutChannels)) || OutChannels == 0)
            return MF_E_INVALIDMEDIATYPE;
        if (FAILED(Type->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &OutRate)) || OutRate == 0)
            return MF_E_INVALIDMEDIATYPE;
        return S_OK;
    }

    void ResetHistory()
    {
        History.assign(Channels, {});
    }

    void UpdateCoefficients()
    {
        const uint32_t version = Equalizer::Version.load();
        if (version == SeenVersion)
            return;
        SeenVersion = version;

        const Settings settings = Equalizer::Get();
        PreampGain = std::pow(10.f, settings.Preamp / 20.f);
        bool anyBand = false;
        for (size_t b = 0; b < BandCount; b++) {
            const float gain = settings.Gains[b];
            const float frequency = Frequencies[b];
            BandOn[b] = settings.bEnabled && std::fabs(gain) > 0.01f && SampleRate > 0 && frequency < SampleRate * 0.45f;
            if (!BandOn[b])
                continue;
            anyBand = true;
            const double A = std::pow(10.0, gain / 40.0);
            const double w0 = 2.0 * 3.14159265358979323846 * frequency / SampleRate;
            const double alpha = std::sin(w0) / (2.0 * 1.41);
            const double a0 = 1.0 + alpha / A;
            Bands[b].B0 = static_cast<float>((1.0 + alpha * A) / a0);
            Bands[b].B1 = static_cast<float>((-2.0 * std::cos(w0)) / a0);
            Bands[b].B2 = static_cast<float>((1.0 - alpha * A) / a0);
            Bands[b].A1 = static_cast<float>((-2.0 * std::cos(w0)) / a0);
            Bands[b].A2 = static_cast<float>((1.0 - alpha / A) / a0);
        }
        bActive = settings.bEnabled && (anyBand || std::fabs(settings.Preamp) > 0.01f);
        if (!bActive)
            ResetHistory();
    }

    void ProcessBuffer(float* Data, size_t Frames)
    {
        UpdateCoefficients();
        if (Channels == 0)
            return;
        Level.Process(Data, Frames, Channels, SampleRate);
        ApplyEqualizer(Data, Frames);
        DeEss.Process(Data, Frames, Channels, SampleRate);
    }

    void ApplyEqualizer(float* Data, size_t Frames)
    {
        if (!bActive)
            return;
        if (History.size() != Channels)
            ResetHistory();

        for (size_t f = 0; f < Frames; f++) {
            for (UINT32 c = 0; c < Channels; c++) {
                float x = Data[f * Channels + c] * PreampGain;
                for (size_t b = 0; b < BandCount; b++) {
                    if (!BandOn[b])
                        continue;
                    const Coefficients& k = Bands[b];
                    auto& z = History[c][b];
                    const float y = k.B0 * x + z[0];
                    z[0] = k.B1 * x - k.A1 * y + z[1];
                    z[1] = k.B2 * x - k.A2 * y;
                    x = y;
                }
                Data[f * Channels + c] = std::clamp(x, -1.f, 1.f);
            }
        }
    }

public:
    ~Transform()
    {
        if (Pending) Pending->Release();
        if (InputType) InputType->Release();
        if (OutputType) OutputType->Release();
    }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv)
            return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFTransform)) {
            *ppv = static_cast<IMFTransform*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() override { return ++RefCount; }

    STDMETHODIMP_(ULONG) Release() override
    {
        const ULONG count = --RefCount;
        if (count == 0)
            delete this;
        return count;
    }

    STDMETHODIMP GetStreamLimits(DWORD* InMin, DWORD* InMax, DWORD* OutMin, DWORD* OutMax) override
    {
        if (!InMin || !InMax || !OutMin || !OutMax)
            return E_POINTER;
        *InMin = *InMax = *OutMin = *OutMax = 1;
        return S_OK;
    }

    STDMETHODIMP GetStreamCount(DWORD* Inputs, DWORD* Outputs) override
    {
        if (!Inputs || !Outputs)
            return E_POINTER;
        *Inputs = *Outputs = 1;
        return S_OK;
    }

    STDMETHODIMP GetStreamIDs(DWORD, DWORD*, DWORD, DWORD*) override { return E_NOTIMPL; }

    STDMETHODIMP GetInputStreamInfo(DWORD Stream, MFT_INPUT_STREAM_INFO* Info) override
    {
        if (!Info)
            return E_POINTER;
        if (Stream != 0)
            return MF_E_INVALIDSTREAMNUMBER;
        *Info = {};
        Info->dwFlags = MFT_INPUT_STREAM_WHOLE_SAMPLES;
        return S_OK;
    }

    STDMETHODIMP GetOutputStreamInfo(DWORD Stream, MFT_OUTPUT_STREAM_INFO* Info) override
    {
        if (!Info)
            return E_POINTER;
        if (Stream != 0)
            return MF_E_INVALIDSTREAMNUMBER;
        *Info = {};
        Info->dwFlags = MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_WHOLE_SAMPLES;
        return S_OK;
    }

    STDMETHODIMP GetAttributes(IMFAttributes**) override { return E_NOTIMPL; }
    STDMETHODIMP GetInputStreamAttributes(DWORD, IMFAttributes**) override { return E_NOTIMPL; }
    STDMETHODIMP GetOutputStreamAttributes(DWORD, IMFAttributes**) override { return E_NOTIMPL; }
    STDMETHODIMP DeleteInputStream(DWORD) override { return E_NOTIMPL; }
    STDMETHODIMP AddInputStreams(DWORD, DWORD*) override { return E_NOTIMPL; }

    STDMETHODIMP GetInputAvailableType(DWORD Stream, DWORD Index, IMFMediaType** Type) override
    {
        if (!Type)
            return E_POINTER;
        if (Stream != 0)
            return MF_E_INVALIDSTREAMNUMBER;
        if (Index > 0)
            return MF_E_NO_MORE_TYPES;
        std::lock_guard lock(Lock);
        if (OutputType) {
            *Type = OutputType;
            OutputType->AddRef();
            return S_OK;
        }
        IMFMediaType* partial = nullptr;
        HRESULT hr = MFCreateMediaType(&partial);
        if (SUCCEEDED(hr)) hr = partial->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        if (SUCCEEDED(hr)) hr = partial->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
        if (FAILED(hr)) {
            if (partial) partial->Release();
            return hr;
        }
        *Type = partial;
        return S_OK;
    }

    STDMETHODIMP GetOutputAvailableType(DWORD Stream, DWORD Index, IMFMediaType** Type) override
    {
        if (!Type)
            return E_POINTER;
        if (Stream != 0)
            return MF_E_INVALIDSTREAMNUMBER;
        if (Index > 0)
            return MF_E_NO_MORE_TYPES;
        std::lock_guard lock(Lock);
        if (!InputType)
            return MF_E_TRANSFORM_TYPE_NOT_SET;
        *Type = InputType;
        InputType->AddRef();
        return S_OK;
    }

    STDMETHODIMP SetInputType(DWORD Stream, IMFMediaType* Type, DWORD Flags) override
    {
        if (Stream != 0)
            return MF_E_INVALIDSTREAMNUMBER;
        std::lock_guard lock(Lock);
        if (Pending)
            return MF_E_TRANSFORM_CANNOT_CHANGE_MEDIATYPE_WHILE_PROCESSING;
        if (!Type) {
            if (!(Flags & MFT_SET_TYPE_TEST_ONLY) && InputType) {
                InputType->Release();
                InputType = nullptr;
            }
            return S_OK;
        }
        UINT32 channels = 0, rate = 0;
        const HRESULT hr = CheckType(Type, channels, rate);
        if (FAILED(hr) || (Flags & MFT_SET_TYPE_TEST_ONLY))
            return hr;
        if (InputType) InputType->Release();
        InputType = Type;
        InputType->AddRef();
        Channels = channels;
        SampleRate = rate;
        SeenVersion = 0;
        ResetHistory();
        Logger::Info("[Equalizer] Audio format: " + std::to_string(rate) + " Hz, " + std::to_string(channels) + " ch");
        return S_OK;
    }

    STDMETHODIMP SetOutputType(DWORD Stream, IMFMediaType* Type, DWORD Flags) override
    {
        if (Stream != 0)
            return MF_E_INVALIDSTREAMNUMBER;
        std::lock_guard lock(Lock);
        if (Pending)
            return MF_E_TRANSFORM_CANNOT_CHANGE_MEDIATYPE_WHILE_PROCESSING;
        if (!Type) {
            if (!(Flags & MFT_SET_TYPE_TEST_ONLY) && OutputType) {
                OutputType->Release();
                OutputType = nullptr;
            }
            return S_OK;
        }
        UINT32 channels = 0, rate = 0;
        HRESULT hr = CheckType(Type, channels, rate);
        if (SUCCEEDED(hr) && InputType && (channels != Channels || rate != SampleRate))
            hr = MF_E_INVALIDMEDIATYPE;
        if (FAILED(hr) || (Flags & MFT_SET_TYPE_TEST_ONLY))
            return hr;
        if (OutputType) OutputType->Release();
        OutputType = Type;
        OutputType->AddRef();
        return S_OK;
    }

    STDMETHODIMP GetInputCurrentType(DWORD Stream, IMFMediaType** Type) override
    {
        if (!Type)
            return E_POINTER;
        if (Stream != 0)
            return MF_E_INVALIDSTREAMNUMBER;
        std::lock_guard lock(Lock);
        if (!InputType)
            return MF_E_TRANSFORM_TYPE_NOT_SET;
        *Type = InputType;
        InputType->AddRef();
        return S_OK;
    }

    STDMETHODIMP GetOutputCurrentType(DWORD Stream, IMFMediaType** Type) override
    {
        if (!Type)
            return E_POINTER;
        if (Stream != 0)
            return MF_E_INVALIDSTREAMNUMBER;
        std::lock_guard lock(Lock);
        if (!OutputType)
            return MF_E_TRANSFORM_TYPE_NOT_SET;
        *Type = OutputType;
        OutputType->AddRef();
        return S_OK;
    }

    STDMETHODIMP GetInputStatus(DWORD Stream, DWORD* Flags) override
    {
        if (!Flags)
            return E_POINTER;
        if (Stream != 0)
            return MF_E_INVALIDSTREAMNUMBER;
        std::lock_guard lock(Lock);
        *Flags = Pending ? 0 : MFT_INPUT_STATUS_ACCEPT_DATA;
        return S_OK;
    }

    STDMETHODIMP GetOutputStatus(DWORD* Flags) override
    {
        if (!Flags)
            return E_POINTER;
        std::lock_guard lock(Lock);
        *Flags = Pending ? MFT_OUTPUT_STATUS_SAMPLE_READY : 0;
        return S_OK;
    }

    STDMETHODIMP SetOutputBounds(LONGLONG, LONGLONG) override { return E_NOTIMPL; }
    STDMETHODIMP ProcessEvent(DWORD, IMFMediaEvent*) override { return E_NOTIMPL; }

    STDMETHODIMP ProcessMessage(MFT_MESSAGE_TYPE Message, ULONG_PTR) override
    {
        std::lock_guard lock(Lock);
        if (Message == MFT_MESSAGE_COMMAND_FLUSH) {
            if (Pending) {
                Pending->Release();
                Pending = nullptr;
            }
            ResetHistory();
        }
        return S_OK;
    }

    STDMETHODIMP ProcessInput(DWORD Stream, IMFSample* Sample, DWORD) override
    {
        if (!Sample)
            return E_POINTER;
        if (Stream != 0)
            return MF_E_INVALIDSTREAMNUMBER;
        std::lock_guard lock(Lock);
        if (!InputType || !OutputType)
            return MF_E_TRANSFORM_TYPE_NOT_SET;
        if (Pending)
            return MF_E_NOTACCEPTING;
        Pending = Sample;
        Pending->AddRef();
        return S_OK;
    }

    STDMETHODIMP ProcessOutput(DWORD, DWORD Count, MFT_OUTPUT_DATA_BUFFER* Samples, DWORD* Status) override
    {
        if (!Samples || !Status || Count != 1)
            return E_INVALIDARG;
        std::lock_guard lock(Lock);
        if (!Pending)
            return MF_E_TRANSFORM_NEED_MORE_INPUT;

        IMFMediaBuffer* buffer = nullptr;
        if (SUCCEEDED(Pending->ConvertToContiguousBuffer(&buffer))) {
            BYTE* data = nullptr;
            DWORD length = 0;
            if (SUCCEEDED(buffer->Lock(&data, nullptr, &length))) {
                if (Channels > 0) {
                    const size_t frames = length / (sizeof(float) * Channels);
                    ProcessBuffer(reinterpret_cast<float*>(data), frames);
                    LONGLONG sampleTime = -1;
                    if (FAILED(Pending->GetSampleTime(&sampleTime)))
                        sampleTime = -1;
                    MicRelay::Write(reinterpret_cast<const float*>(data), frames, Channels, SampleRate, sampleTime);
                    AudioRelay::Write(reinterpret_cast<float*>(data), frames, Channels, SampleRate, sampleTime);
                }
                buffer->Unlock();
            }
            buffer->Release();
        }

        Samples[0].pSample = Pending;
        Samples[0].dwStatus = 0;
        Samples[0].pEvents = nullptr;
        Pending = nullptr;
        *Status = 0;
        return S_OK;
    }
};

IUnknown* Equalizer::CreateEffect()
{
    return static_cast<IMFTransform*>(new Transform());
}

void Equalizer::Set(const Settings& Value)
{
    {
        std::lock_guard lock(Mutex);
        Current = Value;
        Current.Preamp = std::clamp(Current.Preamp, -MaxGain, MaxGain);
        for (auto& gain : Current.Gains)
            gain = std::clamp(gain, -MaxGain, MaxGain);
    }
    Version++;
}

Equalizer::Settings Equalizer::Get()
{
    std::lock_guard lock(Mutex);
    return Current;
}

nlohmann::json Equalizer::ToJson()
{
    const Settings settings = Get();
    nlohmann::json bands = nlohmann::json::array();
    for (size_t i = 0; i < BandCount; i++)
        bands.push_back({ { "frequency", Frequencies[i] }, { "gain", settings.Gains[i] } });
    return { { "enabled", settings.bEnabled }, { "preamp", settings.Preamp }, { "bands", bands }, { "maxGain", MaxGain } };
}

bool Equalizer::FromJson(const nlohmann::json& j)
{
    if (!j.is_object())
        return false;
    Settings settings = Get();
    settings.bEnabled = j.value("enabled", settings.bEnabled);
    settings.Preamp = j.value("preamp", settings.Preamp);
    if (auto it = j.find("bands"); it != j.end() && it->is_array()) {
        for (size_t i = 0; i < BandCount && i < it->size(); i++) {
            const auto& band = (*it)[i];
            if (band.is_number())
                settings.Gains[i] = band.get<float>();
            else if (band.is_object())
                settings.Gains[i] = band.value("gain", settings.Gains[i]);
        }
    }
    Set(settings);
    return true;
}

void Equalizer::Load()
{
    const nlohmann::json saved = ::Settings::Get("equalizer");
    if (!saved.empty())
        FromJson(saved);
}

bool Equalizer::Save()
{
    return ::Settings::Set("equalizer", ToJson());
}
