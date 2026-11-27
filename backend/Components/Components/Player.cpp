#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <audiosessiontypes.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfmediaengine.h>
#include <mfreadwrite.h>

#include <algorithm>
#include <chrono>
#include <cmath>

#include "Player.hpp"
#include "MicRelay.hpp"
#include "AudioRelay.hpp"
#include "Normalize.hpp"
#include "Lyrics.hpp"
#include "Logger.hpp"
#include "Utils.hpp"
#include "Equalizer.hpp"

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

class Player::Notify : public IMFMediaEngineNotify
{
private:
    std::atomic<ULONG> RefCount = 1;

public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv)
            return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFMediaEngineNotify)) {
            *ppv = static_cast<IMFMediaEngineNotify*>(this);
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

    STDMETHODIMP EventNotify(DWORD Event, DWORD_PTR, DWORD) override
    {
        Player::HandleEvent(Event);
        return S_OK;
    }
};

bool Player::Init()
{
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        Logger::Error("[Player] COM init failed (" + std::to_string(hr) + ")");
        return false;
    }
    hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) {
        Logger::Error("[Player] Media Foundation startup failed (" + std::to_string(hr) + ")");
        return false;
    }

    IMFMediaEngineClassFactory* factory = nullptr;
    hr = CoCreateInstance(CLSID_MFMediaEngineClassFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        Logger::Error("[Player] Couldn't create the media engine factory (" + std::to_string(hr) + ")");
        return false;
    }

    IMFAttributes* attributes = nullptr;
    MFCreateAttributes(&attributes, 2);
    Callback = new Notify();
    attributes->SetUnknown(MF_MEDIA_ENGINE_CALLBACK, Callback);
    attributes->SetUINT32(MF_MEDIA_ENGINE_AUDIO_CATEGORY, AudioCategory_Media);

    hr = factory->CreateInstance(MF_MEDIA_ENGINE_AUDIOONLY, attributes, &Engine);
    attributes->Release();
    factory->Release();
    if (FAILED(hr)) {
        Logger::Error("[Player] Couldn't create the media engine (" + std::to_string(hr) + ")");
        Engine = nullptr;
        return false;
    }

    IMFMediaEngineEx* engineEx = nullptr;
    if (SUCCEEDED(Engine->QueryInterface(IID_PPV_ARGS(&engineEx)))) {
        IUnknown* effect = Equalizer::CreateEffect();
        const HRESULT effectHr = engineEx->InsertAudioEffect(effect, TRUE);
        effect->Release();
        engineEx->Release();
        if (FAILED(effectHr))
            Logger::Warning("[Player] Couldn't add the equalizer (" + std::to_string(effectHr) + ")");
    }

    bClockRunning = true;
    ClockThread = std::thread(PublishClock);

    Logger::Success("[Player] Media engine ready");
    return true;
}

void Player::PublishClock()
{
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    while (bClockRunning) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (Engine && Status == PlayerStatus::Playing) {
            const double position = Engine->GetCurrentTime();
            if (std::isfinite(position)) {
                AudioRelay::SetClock(position, true);
                if (MicRelay::IsEnabled())
                    MicRelay::SetClock(position, true);
            }
        }
    }
    if (SUCCEEDED(com))
        CoUninitialize();
}

void Player::Shutdown()
{
    bClockRunning = false;
    if (ClockThread.joinable())
        ClockThread.join();
    if (Engine) {
        Engine->Shutdown();
        Engine->Release();
        Engine = nullptr;
    }
    if (Callback) {
        Callback->Release();
        Callback = nullptr;
    }
    MFShutdown();
    CoUninitialize();
}

bool Player::Play(const std::filesystem::path& Path, const std::string& FileId)
{
    if (!Engine)
        return false;

    {
        std::lock_guard lock(FileIdMutex);
        CurrentFileId = FileId;
    }
    PendingSeek = 0;
    Status = PlayerStatus::Loading;
    AudioRelay::Discontinuity();
    Normalize::SetTrack(FileId, Path);
    Lyrics::Prioritize(FileId, Path);

    BSTR source = SysAllocString(Path.c_str());
    HRESULT hr = Engine->SetSource(source);
    SysFreeString(source);
    if (SUCCEEDED(hr))
        hr = Engine->Play();

    if (FAILED(hr)) {
        Logger::Error("[Player] Couldn't play " + Utils::ToUtf8(Path.filename()) + " (" + std::to_string(hr) + ")");
        Status = PlayerStatus::Error;
        Broadcast();
        return false;
    }

    Logger::Info("[Player] Playing " + Utils::ToUtf8(Path.filename()));
    Broadcast();
    return true;
}

bool Player::Cue(const std::filesystem::path& Path, const std::string& FileId, double Position)
{
    if (!Engine)
        return false;

    {
        std::lock_guard lock(FileIdMutex);
        CurrentFileId = FileId;
    }
    PendingSeek = std::max(0.0, Position);
    Status = PlayerStatus::Paused;
    AudioRelay::Discontinuity();
    Normalize::SetTrack(FileId, Path);
    Lyrics::Prioritize(FileId, Path);

    BSTR source = SysAllocString(Path.c_str());
    HRESULT hr = Engine->SetSource(source);
    SysFreeString(source);
    if (SUCCEEDED(hr))
        hr = Engine->Load();

    if (FAILED(hr)) {
        Logger::Error("[Player] Couldn't load " + Utils::ToUtf8(Path.filename()) + " (" + std::to_string(hr) + ")");
        PendingSeek = 0;
        Status = PlayerStatus::Stopped;
        {
            std::lock_guard lock(FileIdMutex);
            CurrentFileId.clear();
        }
        Broadcast();
        return false;
    }

    Logger::Info("[Player] Restored " + Utils::ToUtf8(Path.filename()) + " at " + std::to_string(static_cast<int>(Position)) + "s");
    Broadcast();
    return true;
}

bool Player::Pause()
{
    if (!Engine || Status == PlayerStatus::Stopped)
        return false;
    return SUCCEEDED(Engine->Pause());
}

bool Player::Resume()
{
    if (!Engine || Status == PlayerStatus::Stopped || Status == PlayerStatus::Error)
        return false;
    return SUCCEEDED(Engine->Play());
}

bool Player::Stop()
{
    if (!Engine)
        return false;
    Status = PlayerStatus::Stopped;
    {
        std::lock_guard lock(FileIdMutex);
        CurrentFileId.clear();
    }
    Engine->Pause();
    AudioRelay::Discontinuity();
    Engine->SetCurrentTime(0);
    Broadcast();
    return true;
}

bool Player::Seek(double Seconds)
{
    if (!Engine || Status == PlayerStatus::Stopped)
        return false;
    if (!std::isfinite(Seconds) || Seconds < 0)
        Seconds = 0;
    PendingSeek = 0;
    AudioRelay::Discontinuity();
    const bool ok = SUCCEEDED(Engine->SetCurrentTime(Seconds));
    if (ok && Status == PlayerStatus::Playing)
        AudioRelay::MarkStart(Seconds);
    return ok;
}

bool Player::SetVolume(double Volume)
{
    if (!Engine)
        return false;
    if (!std::isfinite(Volume))
        Volume = 1;
    MicRelay::SetGain(Volume);
    AudioRelay::SetVolume(Volume);
    return SUCCEEDED(Engine->SetVolume(std::clamp(Volume, 0.0, 1.0)));
}

PlayerState Player::GetState()
{
    PlayerState state{ Status.load(), {}, 0, 0 };
    {
        std::lock_guard lock(FileIdMutex);
        state.FileId = CurrentFileId;
    }
    if (Engine && state.Status != PlayerStatus::Stopped) {
        const double position = Engine->GetCurrentTime();
        const double duration = Engine->GetDuration();
        state.Position = std::isfinite(position) ? position : 0;
        if (state.Position <= 0 && PendingSeek > 0)
            state.Position = PendingSeek;
        state.Duration = std::isfinite(duration) ? duration : 0;
    }
    return state;
}

void Player::OnStateChanged(std::function<void(const PlayerState&)> Handler)
{
    StateHandler = std::move(Handler);
}

void Player::OnEnded(std::function<void()> Handler)
{
    EndedHandler = std::move(Handler);
}

void Player::HandleEvent(unsigned long Event)
{
    const PlayerStatus current = Status;
    switch (Event) {
    case MF_MEDIA_ENGINE_EVENT_PLAYING:
        if (current != PlayerStatus::Stopped)
            Status = PlayerStatus::Playing;
        break;
    case MF_MEDIA_ENGINE_EVENT_PAUSE:
        if (current == PlayerStatus::Playing || current == PlayerStatus::Loading)
            Status = PlayerStatus::Paused;
        break;
    case MF_MEDIA_ENGINE_EVENT_ENDED:
        if (current != PlayerStatus::Stopped)
            Status = PlayerStatus::Ended;
        break;
    case MF_MEDIA_ENGINE_EVENT_ERROR: {
        if (current == PlayerStatus::Stopped)
            return;
        Status = PlayerStatus::Error;
        USHORT code = 0;
        IMFMediaError* error = nullptr;
        if (Engine && SUCCEEDED(Engine->GetError(&error)) && error) {
            code = error->GetErrorCode();
            error->Release();
        }
        Logger::Error("[Player] Playback error (code " + std::to_string(code) + ")");
        break;
    }
    case MF_MEDIA_ENGINE_EVENT_TIMEUPDATE:
    case MF_MEDIA_ENGINE_EVENT_DURATIONCHANGE:
        break;
    case MF_MEDIA_ENGINE_EVENT_LOADEDMETADATA: {
        const double seek = PendingSeek.exchange(0);
        if (seek > 0 && Engine) {
            AudioRelay::Discontinuity();
            Engine->SetCurrentTime(seek);
        }
        break;
    }
    case MF_MEDIA_ENGINE_EVENT_SEEKED:
        break;
    default:
        return;
    }
    Broadcast();
    if (Event == MF_MEDIA_ENGINE_EVENT_ENDED && Status == PlayerStatus::Ended && EndedHandler)
        EndedHandler();
}

void Player::Broadcast()
{
    const PlayerState state = GetState();
    const bool playing = state.Status == PlayerStatus::Playing;
    if (playing && !bWasPlaying.exchange(true))
        AudioRelay::MarkStart(state.Position);
    else if (!playing)
        bWasPlaying = false;
    MicRelay::SetClock(state.Position, playing);
    AudioRelay::SetClock(state.Position, playing);
    if (StateHandler)
        StateHandler(state);
}

double Player::ProbeDuration(const std::filesystem::path& Path)
{
    IMFSourceReader* reader = nullptr;
    if (FAILED(MFCreateSourceReaderFromURL(Path.c_str(), nullptr, &reader)))
        return 0;

    double seconds = 0;
    PROPVARIANT value;
    PropVariantInit(&value);
    if (SUCCEEDED(reader->GetPresentationAttribute(static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &value))
        && value.vt == VT_UI8)
        seconds = static_cast<double>(value.uhVal.QuadPart) / 10'000'000.0;
    PropVariantClear(&value);
    reader->Release();
    return seconds;
}

std::string Player::StatusName(PlayerStatus Status)
{
    switch (Status) {
    case PlayerStatus::Loading: return "loading";
    case PlayerStatus::Playing: return "playing";
    case PlayerStatus::Paused:  return "paused";
    case PlayerStatus::Ended:   return "ended";
    case PlayerStatus::Error:   return "error";
    default:                    return "stopped";
    }
}
