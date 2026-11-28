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
#include "Crossfade.hpp"

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

class Player::Notify : public IMFMediaEngineNotify
{
private:
    std::atomic<ULONG> RefCount = 1;
    int Slot = 0;

public:
    explicit Notify(int Slot) : Slot(Slot) {}

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
        Player::HandleEvent(Slot, Event);
        return S_OK;
    }
};

bool Player::CreateEngine(IMFMediaEngineClassFactory* Factory, int Slot)
{
    IMFAttributes* attributes = nullptr;
    MFCreateAttributes(&attributes, 2);
    Callbacks[Slot] = new Notify(Slot);
    attributes->SetUnknown(MF_MEDIA_ENGINE_CALLBACK, Callbacks[Slot]);
    attributes->SetUINT32(MF_MEDIA_ENGINE_AUDIO_CATEGORY, AudioCategory_Media);

    IMFMediaEngine* engine = nullptr;
    const HRESULT hr = Factory->CreateInstance(MF_MEDIA_ENGINE_AUDIOONLY, attributes, &engine);
    attributes->Release();
    if (FAILED(hr)) {
        Logger::Error("[Player] Couldn't create media engine " + std::to_string(Slot) + " (" + std::to_string(hr) + ")");
        return false;
    }

    IMFMediaEngineEx* engineEx = nullptr;
    if (SUCCEEDED(engine->QueryInterface(IID_PPV_ARGS(&engineEx)))) {
        IUnknown* effect = Equalizer::CreateEffect(Slot);
        const HRESULT effectHr = engineEx->InsertAudioEffect(effect, TRUE);
        effect->Release();
        engineEx->Release();
        if (FAILED(effectHr))
            Logger::Warning("[Player] Couldn't add the equalizer (" + std::to_string(effectHr) + ")");
    }
    Engines[Slot] = engine;
    return true;
}

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

    const bool ok = CreateEngine(factory, 0);
    if (ok && !CreateEngine(factory, 1))
        Logger::Warning("[Player] Crossfade is unavailable without a second media engine");
    factory->Release();
    if (!ok)
        return false;

    Current = 0;
    Crossfade::Reset(0);
    bClockRunning = true;
    ClockThread = std::thread(PublishClock);

    Logger::Success("[Player] Media engine ready");
    return true;
}

IMFMediaEngine* Player::Active()
{
    return Engines[Current];
}

double Player::RelayPosition()
{
    const int writer = Crossfade::Writer();
    IMFMediaEngine* engine = Engines[writer];
    double position = engine ? engine->GetCurrentTime() : 0;
    if (!std::isfinite(position))
        position = 0;
    if (position <= 0 && writer == Current && PendingSeek > 0)
        position = PendingSeek;
    return position + Crossfade::OffsetSeconds();
}

void Player::Isolate()
{
    const int active = Current;
    for (int slot = 0; slot < EngineCount; slot++) {
        if (slot != active && Engines[slot])
            Engines[slot]->Pause();
    }
    Crossfade::Reset(active);
}

void Player::PublishClock()
{
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    double last = -1;
    int lastSlot = -1;
    uint32_t lastTrack = 0;
    bool bFadeFired = false;
    while (bClockRunning) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (const int done = Crossfade::TakeFinished(); done >= 0 && done != Current && Engines[done]) {
            Engines[done]->Pause();
            Logger::Info("[Player] Crossfade finished");
        }
        IMFMediaEngine* engine = Active();
        if (engine && Status == PlayerStatus::Playing) {
            const double position = engine->GetCurrentTime();
            if (std::isfinite(position)) {
                const double relay = RelayPosition();
                AudioRelay::SetClock(relay, true);
                if (MicRelay::IsEnabled())
                    MicRelay::SetClock(relay, true);
                if (Current != lastSlot || TrackChange != lastTrack) {
                    lastSlot = Current;
                    lastTrack = TrackChange;
                    last = -1;
                    bFadeFired = false;
                }
                const auto crossed = [&](double Point) {
                    return Point > 0 && last >= 0 && last < Point && position >= Point && position - last < 1.0;
                };
                const double end = EndAt;
                const double fade = FadeSeconds;
                const double duration = engine->GetDuration();
                const double stop = end > 0 ? end : (std::isfinite(duration) ? duration : 0);
                const double fadeAt = fade > 0 && Engines[1] && stop > 2.0 * fade ? stop - fade : 0;
                if (fadeAt > 0 && position < fadeAt)
                    bFadeFired = false;
                const double remaining = stop - position;
                if (fadeAt > 0 && !bFadeFired && position >= fadeAt && remaining >= MinFadeSeconds && FadeHandler && !Crossfade::IsActive()) {
                    bFadeFired = true;
                    PendingFade = std::min(fade, remaining);
                    FadeHandler();
                }
                else if (crossed(end) && EndedHandler)
                    EndedHandler();
                last = position;
            }
        }
        else {
            last = -1;
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
    for (int slot = 0; slot < EngineCount; slot++) {
        if (Engines[slot]) {
            Engines[slot]->Shutdown();
            Engines[slot]->Release();
            Engines[slot] = nullptr;
        }
        if (Callbacks[slot]) {
            Callbacks[slot]->Release();
            Callbacks[slot] = nullptr;
        }
    }
    MFShutdown();
    CoUninitialize();
}

bool Player::Play(const std::filesystem::path& Path, const std::string& FileId, double Start, bool bCrossfade)
{
    if (!Active())
        return false;

    const int previous = Current;
    const int next = (previous + 1) % EngineCount;
    const double pending = PendingFade.exchange(0);
    const double fadeSeconds = pending > 0 ? pending : FadeSeconds.load();
    const bool bFade = bCrossfade && Engines[next] && fadeSeconds > 0 && Status == PlayerStatus::Playing && !Crossfade::IsActive();
    if (!bFade)
        Isolate();

    {
        std::lock_guard lock(FileIdMutex);
        CurrentFileId = FileId;
    }
    PendingSeek = std::isfinite(Start) ? std::max(0.0, Start) : 0.0;
    EndAt = 0;
    TrackChange++;
    if (bFade) {
        Crossfade::Begin(previous, next, fadeSeconds);
        Current = next;
    }
    else {
        Status = PlayerStatus::Loading;
        AudioRelay::Discontinuity();
    }
    IMFMediaEngine* engine = Active();
    Normalize::SetTrack(Current, FileId, Path);
    Lyrics::Prioritize(FileId, Path);

    BSTR source = SysAllocString(Path.c_str());
    HRESULT hr = engine->SetSource(source);
    SysFreeString(source);
    if (SUCCEEDED(hr))
        hr = engine->Play();

    if (FAILED(hr)) {
        Logger::Error("[Player] Couldn't play " + Utils::ToUtf8(Path.filename()) + " (" + std::to_string(hr) + ")");
        Isolate();
        Status = PlayerStatus::Error;
        Broadcast();
        return false;
    }

    Logger::Info(std::string(bFade ? "[Player] Crossfading into " : "[Player] Playing ") + Utils::ToUtf8(Path.filename()));
    Broadcast();
    return true;
}

bool Player::Cue(const std::filesystem::path& Path, const std::string& FileId, double Position)
{
    IMFMediaEngine* engine = Active();
    if (!engine)
        return false;
    Isolate();

    {
        std::lock_guard lock(FileIdMutex);
        CurrentFileId = FileId;
    }
    PendingSeek = std::max(0.0, Position);
    EndAt = 0;
    TrackChange++;
    Status = PlayerStatus::Paused;
    AudioRelay::Discontinuity();
    Normalize::SetTrack(Current, FileId, Path);
    Lyrics::Prioritize(FileId, Path);

    BSTR source = SysAllocString(Path.c_str());
    HRESULT hr = engine->SetSource(source);
    SysFreeString(source);
    if (SUCCEEDED(hr))
        hr = engine->Load();

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
    IMFMediaEngine* engine = Active();
    if (!engine || Status == PlayerStatus::Stopped)
        return false;
    if (Crossfade::IsActive()) {
        for (int slot = 0; slot < EngineCount; slot++) {
            if (slot != Current && Engines[slot])
                Engines[slot]->Pause();
        }
    }
    return SUCCEEDED(engine->Pause());
}

bool Player::Resume()
{
    IMFMediaEngine* engine = Active();
    if (!engine || Status == PlayerStatus::Stopped || Status == PlayerStatus::Error)
        return false;
    if (Crossfade::IsActive()) {
        for (int slot = 0; slot < EngineCount; slot++) {
            if (slot != Current && Engines[slot])
                Engines[slot]->Play();
        }
    }
    return SUCCEEDED(engine->Play());
}

bool Player::Stop()
{
    IMFMediaEngine* engine = Active();
    if (!engine)
        return false;
    Isolate();
    Status = PlayerStatus::Stopped;
    {
        std::lock_guard lock(FileIdMutex);
        CurrentFileId.clear();
    }
    engine->Pause();
    AudioRelay::Discontinuity();
    engine->SetCurrentTime(0);
    Broadcast();
    return true;
}

bool Player::Seek(double Seconds)
{
    IMFMediaEngine* engine = Active();
    if (!engine || Status == PlayerStatus::Stopped)
        return false;
    if (!std::isfinite(Seconds) || Seconds < 0)
        Seconds = 0;
    Isolate();
    PendingSeek = 0;
    TrackChange++;
    AudioRelay::Discontinuity();
    const bool ok = SUCCEEDED(engine->SetCurrentTime(Seconds));
    if (ok && Status == PlayerStatus::Playing)
        AudioRelay::MarkStart(Seconds);
    return ok;
}

void Player::SetEnd(double Seconds)
{
    EndAt = std::isfinite(Seconds) && Seconds > 0 ? Seconds : 0.0;
}

void Player::SetCrossfade(double Seconds)
{
    FadeSeconds = std::isfinite(Seconds) ? std::clamp(Seconds, 0.0, Crossfade::MaxSeconds) : 0.0;
}

bool Player::SetVolume(double Volume)
{
    if (!Active())
        return false;
    if (!std::isfinite(Volume))
        Volume = 1;
    MicRelay::SetGain(Volume);
    AudioRelay::SetVolume(Volume);
    bool ok = true;
    for (IMFMediaEngine* engine : Engines) {
        if (engine)
            ok = SUCCEEDED(engine->SetVolume(std::clamp(Volume, 0.0, 1.0))) && ok;
    }
    return ok;
}

PlayerState Player::GetState()
{
    PlayerState state{ Status.load(), {}, 0, 0 };
    {
        std::lock_guard lock(FileIdMutex);
        state.FileId = CurrentFileId;
    }
    IMFMediaEngine* engine = Active();
    if (engine && state.Status != PlayerStatus::Stopped) {
        const double position = engine->GetCurrentTime();
        const double duration = engine->GetDuration();
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

void Player::OnFadePoint(std::function<void()> Handler)
{
    FadeHandler = std::move(Handler);
}

void Player::HandleEvent(int Slot, unsigned long Event)
{
    if (Slot != Current) {
        if (Event == MF_MEDIA_ENGINE_EVENT_ENDED || Event == MF_MEDIA_ENGINE_EVENT_ERROR)
            Crossfade::OutgoingEnded();
        return;
    }
    IMFMediaEngine* engine = Engines[Slot];
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
        if (engine && SUCCEEDED(engine->GetError(&error)) && error) {
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
        if (seek > 0 && engine) {
            if (!Crossfade::IsActive())
                AudioRelay::Discontinuity();
            engine->SetCurrentTime(seek);
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
    const double relay = RelayPosition();
    const bool playing = state.Status == PlayerStatus::Playing;
    if (playing && !bWasPlaying.exchange(true) && !Crossfade::IsActive())
        AudioRelay::MarkStart(relay);
    else if (!playing)
        bWasPlaying = false;
    MicRelay::SetClock(relay, playing);
    AudioRelay::SetClock(relay, playing);
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
