#include "DiscordPresence.hpp"
#include "DiscordIPC.hpp"
#include "../Components/Components/AppData.hpp"
#include "../Components/Components/Logger.hpp"
#include "../Components/Components/Settings.hpp"

#include <cmath>

int64_t DiscordPresence::NowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string DiscordPresence::StateKey(const PlayerState& State)
{
    const bool playing = State.Status == PlayerStatus::Playing || State.Status == PlayerStatus::Loading;
    const bool active = playing || State.Status == PlayerStatus::Paused;
    return active ? State.FileId + (playing ? "|playing" : "|paused") : std::string("idle");
}

void DiscordPresence::Initialize()
{
    std::lock_guard lock(Mutex);
    if (bInitialized)
        return;
    bInitialized = true;
    InitializePeriodic();
}

void DiscordPresence::Shutdown()
{
    {
        std::lock_guard lock(Mutex);
        bPeriodicThreadRunning = false;
        bInitialized = false;
    }
    PeriodicWake.notify_all();
    if (PeriodicThread.joinable())
        PeriodicThread.join();
    DiscordIPC::Shutdown();
}

void DiscordPresence::SetEnabled(bool bValue)
{
    {
        std::lock_guard lock(Mutex);
        if (bEnabled == bValue)
            return;
        bEnabled = bValue;
        bDirty = true;
    }
    Logger::Info(std::string("[DiscordPresence] Discord rich presence ") + (bValue ? "enabled" : "disabled"));
    PeriodicWake.notify_all();
}

bool DiscordPresence::IsEnabled()
{
    std::lock_guard lock(Mutex);
    return bEnabled;
}

void DiscordPresence::OnPlayerState(const PlayerState& State)
{
    std::lock_guard lock(Mutex);
    Current = State;
    const bool playing = State.Status == PlayerStatus::Playing;
    const int64_t start = NowMs() - static_cast<int64_t>(State.Position * 1000.0);
    const bool seeked = playing && SentStart != 0 && std::llabs(start - SentStart) > 3000;
    if (StateKey(State) != SentKey || seeked) {
        bDirty = true;
        PeriodicWake.notify_all();
    }
}

bool DiscordPresence::EnsureConnected()
{
    if (DiscordIPC::IsConnected())
        return true;
    const auto now = std::chrono::steady_clock::now();
    if (now - LastConnectAttempt < std::chrono::seconds(15))
        return false;
    LastConnectAttempt = now;
    return DiscordIPC::Initialize(APPLICATION_ID);
}

DiscordPresence::Sent DiscordPresence::Publish(const PlayerState& State, bool bShow, bool bWasShown)
{
    if (!bShow) {
        if (bWasShown)
            DiscordIPC::ClearRichPresence();
        return { {}, 0, false };
    }

    RichPresence presence;
    presence.name = "Wave";
    presence.type = RichPresenceActivityType::Listening;
    presence.assets.large_image = MAIN_ICON_ASSET;
    presence.assets.large_text = "Wave";

    const std::string key = StateKey(State);
    int64_t start = 0;
    if (key == "idle") {
        presence.details = "Browsing the library";
    }
    else {
        const std::optional<File> file = AppData::FindFile(State.FileId);
        const File::DisplayInfo display = file ? file->Display() : File::DisplayInfo{ "Unknown track", "" };
        const std::string& title = display.Title;
        const std::string& artist = display.Artist;
        presence.details = title;
        if (artist.empty() == false || State.Status == PlayerStatus::Paused) {
            presence.state = State.Status == PlayerStatus::Paused ? "Paused" + (artist.empty() ? "" : " \xC2\xB7 " + artist) : artist;
        }
        presence.status_display_type = 2;
        if (file && !file->Album.empty())
            presence.assets.large_text = file->Album;

        if (State.Status != PlayerStatus::Paused && State.Duration > 0) {
            start = NowMs() - static_cast<int64_t>(State.Position * 1000.0);
            presence.timestamps.start = static_cast<uint64_t>(start);
            presence.timestamps.end = static_cast<uint64_t>(start + static_cast<int64_t>(State.Duration * 1000.0));
        }
    }

    DiscordIPC::SendRichPresence(presence);
    return { key, start, true };
}

void DiscordPresence::InitializePeriodic()
{
    std::lock_guard lock(Mutex);
    if (PeriodicThread.joinable())
        return;
    bPeriodicThreadRunning = true;
    PeriodicThread = std::thread([] {
        std::unique_lock lock(Mutex);
        while (bPeriodicThreadRunning) {
            PeriodicWake.wait_for(lock, std::chrono::seconds(DISCORD_RPC_UPDATE_INTERVAL_SECONDS), [] { return !bPeriodicThreadRunning || bDirty; });
            if (!bPeriodicThreadRunning)
                break;
            if (!bDirty)
                continue;
            if (!bEnabled && !bShown) {
                bDirty = false;
                continue;
            }

            lock.unlock();
            const bool wasConnected = DiscordIPC::IsConnected();
            const bool connected = EnsureConnected();
            lock.lock();
            if (!connected) {
                PeriodicWake.wait_for(lock, std::chrono::seconds(DISCORD_RPC_UPDATE_INTERVAL_SECONDS), [] { return !bPeriodicThreadRunning; });
                continue;
            }
            if (!wasConnected)
                bShown = false;

            const auto earliest = LastSent + std::chrono::seconds(DISCORD_RPC_UPDATE_INTERVAL_SECONDS) - std::chrono::seconds(1);
            if (PeriodicWake.wait_until(lock, earliest, [] { return !bPeriodicThreadRunning; }))
                break;

            const PlayerState state = Current;
            const bool show = bEnabled;
            const bool wasShown = bShown;
            bDirty = false;
            lock.unlock();
            Sent sent{ SentKey, SentStart, bShown };
            try {
                sent = Publish(state, show, wasShown);
            }
            catch (const std::exception& e) {
                Logger::Error(std::string("[DiscordPresence] Update failed: ") + e.what());
            }
            lock.lock();

            SentKey = sent.Key;
            SentStart = sent.Start;
            bShown = sent.bShown;
            LastSent = std::chrono::steady_clock::now();
        }
    });
}

void DiscordPresence::Load()
{
    bEnabled = Settings::Bool(Settings::Get("discord"), "enabled", true);
}

bool DiscordPresence::Save()
{
    return Settings::Merge("discord", { { "enabled", IsEnabled() } });
}
