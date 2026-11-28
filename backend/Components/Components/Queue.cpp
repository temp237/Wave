#include <algorithm>
#include <random>

#include <nlohmann/json.hpp>

#include "Queue.hpp"
#include "AppData.hpp"
#include "Logger.hpp"
#include "Lyrics.hpp"
#include "Player.hpp"
#include "Settings.hpp"

void Queue::Init()
{
    {
        std::lock_guard lock(Mutex);
        const nlohmann::json playback = Settings::Get("playback");
        bShuffle = Settings::Bool(playback, "shuffle", false);
        Repeat = ParseRepeat(Settings::String(playback, "repeat", "off"));
        bSkipToVocals = Settings::Bool(playback, "skipToVocals", false);
        Player::SetCrossfade(Settings::Number(playback, "crossfade", 0.0));

        CachedQueue cached = AppData::GetCachedQueue();
        Order = std::move(cached.Order);
        Base = std::move(cached.Base);
        Index = cached.Index;
        Queued = cached.Queued;
        Source = std::move(cached.Source);
        if (Index >= 0) {
            if (std::optional<File> file = AppData::FindFile(Order[Index])) {
                Player::Cue(file->Path, Order[Index], cached.Position);
                Player::SetEnd(FindSkipPoints(*file).End);
            }
            Logger::Info("[Queue] Restored " + std::to_string(Order.size()) + " queued tracks");
        }
    }
    Worker.Start();
    Player::OnEnded([] {
        Worker.Post([] { Next(true); });
    });
    Player::OnFadePoint([] {
        Worker.Post([] { CrossfadeNext(); });
    });
    Lyrics::OnStored([](const std::string& Id) {
        Worker.Post([Id] { ApplySkipPoints(Id); });
    });
}

void Queue::Shutdown()
{
    {
        std::lock_guard lock(Mutex);
        Persist(true);
    }
    Player::OnEnded({});
    Player::OnFadePoint({});
    Lyrics::OnStored({});
    Worker.Stop();
    ChangedHandler = {};
}

std::vector<std::string> Queue::Shuffled(std::vector<std::string> Ids)
{
    static std::mt19937 rng{ std::random_device{}() };
    std::shuffle(Ids.begin(), Ids.end(), rng);
    return Ids;
}

void Queue::ClampQueued()
{
    const int64_t room = Index >= 0 ? static_cast<int64_t>(Order.size()) - Index - 1 : 0;
    Queued = std::clamp<int64_t>(Queued, 0, std::max<int64_t>(room, 0));
}

bool Queue::PlayCurrent(bool bCrossfade)
{
    if (Index < 0 || Index >= static_cast<int64_t>(Order.size()))
        return false;
    const std::string& id = Order[Index];
    std::optional<File> file = AppData::FindFile(id);
    if (!file) {
        Logger::Error("[Queue] Track " + id.substr(0, 12) + "... isn't in the library");
        return false;
    }
    const SkipPoints points = FindSkipPoints(*file);
    if (bSkipToVocals)
        Logger::Info(points.Source
            ? "[Queue] Skip to vocals (" + std::string(points.Source) + "): start " + std::to_string(static_cast<int>(points.Start)) + "s, end " + std::to_string(static_cast<int>(points.End)) + "s"
            : std::string("[Queue] Skip to vocals: no vocal timing or cached lyrics yet for this song"));
    const bool ok = Player::Play(file->Path, id, points.Start, bCrossfade);
    if (ok)
        Player::SetEnd(points.End);
    return ok;
}

Queue::SkipPoints Queue::FindSkipPoints(const File& Track)
{
    SkipPoints points;
    if (!bSkipToVocals)
        return points;
    double vocalStart = -1.0;
    double vocalEnd = -1.0;
    if (Track.LyricsVersion >= Lyrics::Version && Track.VocalStart >= 0) {
        vocalStart = Track.VocalStart;
        vocalEnd = Track.VocalEnd;
        points.Source = "vocals";
    }
    else if (!Track.Lyrics.empty()) {
        vocalStart = Track.Lyrics.front().Start;
        vocalEnd = Track.Lyrics.back().End;
        points.Source = "cached lyrics";
    }
    if (vocalStart < 0 || vocalEnd <= vocalStart) {
        points.Source = nullptr;
        return points;
    }
    points.Start = std::max(0.0, vocalStart - VocalMargin);
    const double end = vocalEnd + VocalMargin;
    if (Track.Duration <= 0 || end < Track.Duration - 1.0)
        points.End = end;
    return points;
}

void Queue::ApplySkipPoints(const std::string& Id)
{
    std::lock_guard lock(Mutex);
    if (!bSkipToVocals || Index < 0 || Index >= static_cast<int64_t>(Order.size()) || Order[Index] != Id)
        return;
    const PlayerState state = Player::GetState();
    if (state.FileId != Id)
        return;
    const std::optional<File> file = AppData::FindFile(Id);
    if (!file)
        return;
    const SkipPoints points = FindSkipPoints(*file);
    Player::SetEnd(points.End);
    if (points.Start > 0 && state.Status == PlayerStatus::Playing && state.Position < points.Start - 1.0) {
        Logger::Info("[Queue] Vocals found, skipping intro to " + std::to_string(static_cast<int>(points.Start)) + "s");
        Player::Seek(points.Start);
    }
}

void Queue::SetSkipToVocals(bool bOn)
{
    std::lock_guard lock(Mutex);
    bSkipToVocals = bOn;
    std::optional<File> file;
    if (Index >= 0 && Index < static_cast<int64_t>(Order.size()))
        file = AppData::FindFile(Order[Index]);
    Player::SetEnd(file ? FindSkipPoints(*file).End : 0.0);
}

bool Queue::Set(const std::vector<std::string>& Ids, size_t Start, const std::string& Source)
{
    if (Ids.empty())
        return false;
    std::lock_guard lock(Mutex);
    Start = std::min(Start, Ids.size() - 1);
    Base = Ids;
    Queued = 0;
    Queue::Source = Source;
    if (bShuffle) {
        std::vector<std::string> rest;
        for (size_t i = 0; i < Ids.size(); i++) {
            if (i != Start)
                rest.push_back(Ids[i]);
        }
        Order = { Ids[Start] };
        for (auto& id : Shuffled(std::move(rest)))
            Order.push_back(std::move(id));
        Index = 0;
    }
    else {
        Order = Ids;
        Index = static_cast<int64_t>(Start);
    }
    const bool ok = PlayCurrent();
    Broadcast();
    return ok;
}

void Queue::CrossfadeNext()
{
    std::lock_guard lock(Mutex);
    if (Index < 0 || Index >= static_cast<int64_t>(Order.size()) || Player::GetState().FileId != Order[Index])
        return;
    if (Repeat == RepeatMode::One || (Index + 1 >= static_cast<int64_t>(Order.size()) && Repeat != RepeatMode::All))
        return;
    Next(true, true);
}

bool Queue::Next(bool bAuto, bool bCrossfade)
{
    std::lock_guard lock(Mutex);
    if (Order.empty())
        return false;

    if (bAuto && (Index < 0 || Index >= static_cast<int64_t>(Order.size()) || Player::GetState().FileId != Order[Index]))
        return false;

    if (bAuto && Repeat == RepeatMode::One) {
        std::optional<File> file = AppData::FindFile(Order[Index]);
        Player::Seek(file ? FindSkipPoints(*file).Start : 0.0);
        return Player::Resume();
    }

    if (Index >= 0 && Index < static_cast<int64_t>(Order.size()) && std::find(Base.begin(), Base.end(), Order[Index]) == Base.end()) {
        Order.erase(Order.begin() + Index);
        Index--;
        if (Order.empty()) {
            Player::Stop();
            Index = -1;
            Queued = 0;
            Broadcast();
            return false;
        }
    }

    int64_t next = Index + 1;
    if (next >= static_cast<int64_t>(Order.size())) {
        if (Repeat != RepeatMode::All && bAuto) {
            if (Player::GetState().Status == PlayerStatus::Playing)
                Player::Pause();
            Broadcast();
            return false;
        }
        next = 0;
        Queued = 0;
        if (bShuffle && Order.size() > 2) {
            const std::string last = Order.back();
            Order = Shuffled(Order);
            if (Order.front() == last)
                std::rotate(Order.begin(), Order.begin() + 1, Order.end());
        }
    }
    if (next == Index + 1 && Queued > 0)
        Queued--;
    Index = next;
    const bool ok = PlayCurrent(bCrossfade);
    Broadcast();
    return ok;
}

bool Queue::Previous()
{
    std::lock_guard lock(Mutex);
    if (Order.empty())
        return false;
    if (Player::GetState().Position > 3)
        return Player::Seek(0);

    int64_t prev = Index - 1;
    if (prev < 0)
        prev = Repeat == RepeatMode::All ? static_cast<int64_t>(Order.size()) - 1 : 0;
    Index = prev;
    Queued = 0;
    const bool ok = PlayCurrent();
    Broadcast();
    return ok;
}

bool Queue::Jump(size_t Index)
{
    std::lock_guard lock(Mutex);
    if (Index >= Order.size())
        return false;
    const int64_t target = static_cast<int64_t>(Index);
    const int64_t queuedEnd = Queue::Index + Queued;
    Queued = target > Queue::Index && target <= queuedEnd ? queuedEnd - target : 0;
    Queue::Index = target;
    const bool ok = PlayCurrent();
    Broadcast();
    return ok;
}

bool Queue::Remove(size_t Index)
{
    std::lock_guard lock(Mutex);
    if (Index >= Order.size() || static_cast<int64_t>(Index) == Queue::Index)
        return false;
    const std::string id = Order[Index];
    if (static_cast<int64_t>(Index) > Queue::Index && static_cast<int64_t>(Index) <= Queue::Index + Queued)
        Queued--;
    Order.erase(Order.begin() + Index);
    if (static_cast<int64_t>(Index) < Queue::Index)
        Queue::Index--;
    if (auto it = std::find(Base.begin(), Base.end(), id); it != Base.end())
        Base.erase(it);
    Broadcast();
    return true;
}

void Queue::RemoveTracks(const std::vector<std::string>& Ids)
{
    std::lock_guard lock(Mutex);
    if (Order.empty())
        return;
    auto removed = [&](const std::string& id) { return std::find(Ids.begin(), Ids.end(), id) != Ids.end(); };
    const bool hasCurrent = Index >= 0 && Index < static_cast<int64_t>(Order.size());

    int64_t survivors = 0;
    for (int64_t i = Index + 1; hasCurrent && i <= Index + Queued && i < static_cast<int64_t>(Order.size()); i++) {
        if (!removed(Order[i]))
            survivors++;
    }
    Queued = survivors;

    std::vector<std::string> kept;
    int64_t newIndex = -1;
    for (int64_t i = 0; i < static_cast<int64_t>(Order.size()); i++) {
        const bool isCurrent = hasCurrent && i == Index;
        if (isCurrent)
            newIndex = static_cast<int64_t>(kept.size());
        if (isCurrent || !removed(Order[i]))
            kept.push_back(Order[i]);
    }
    Order = std::move(kept);
    Index = newIndex;
    Base.erase(std::remove_if(Base.begin(), Base.end(), removed), Base.end());
    ClampQueued();
    Broadcast();
}

std::string Queue::CurrentSource()
{
    std::lock_guard lock(Mutex);
    return Source;
}

bool Queue::PlayNext(const std::string& Id)
{
    std::lock_guard lock(Mutex);
    if (Order.empty() || Index < 0)
        return Set({ Id }, 0, "");

    const std::string current = Order[Index];
    if (Id == current)
        return true;
    for (int64_t i = Index + 1; i <= Index + Queued && i < static_cast<int64_t>(Order.size()); i++) {
        if (Order[i] == Id)
            Queued--;
    }
    auto strip = [&](std::vector<std::string>& list) {
        list.erase(std::remove(list.begin(), list.end(), Id), list.end());
    };
    strip(Order);
    strip(Base);
    Index = std::find(Order.begin(), Order.end(), current) - Order.begin();
    Order.insert(Order.begin() + Index + 1, Id);
    Queued++;
    ClampQueued();
    auto inBase = std::find(Base.begin(), Base.end(), current);
    Base.insert(inBase == Base.end() ? Base.end() : inBase + 1, Id);
    Broadcast();
    return true;
}

bool Queue::Add(const std::vector<std::string>& Ids)
{
    if (Ids.empty())
        return false;
    std::lock_guard lock(Mutex);
    if (Order.empty() || Index < 0)
        return Set(Ids, 0, "");
    ClampQueued();
    Order.insert(Order.begin() + Index + 1 + Queued, Ids.begin(), Ids.end());
    auto inBase = std::find(Base.begin(), Base.end(), Order[Index]);
    const size_t basePos = inBase == Base.end() ? Base.size() : std::min<size_t>(Base.size(), (inBase - Base.begin()) + 1 + static_cast<size_t>(Queued));
    Base.insert(Base.begin() + basePos, Ids.begin(), Ids.end());
    Queued += static_cast<int64_t>(Ids.size());
    Logger::Info("[Queue] Added " + std::to_string(Ids.size()) + " track(s) after the current one");
    Broadcast();
    return true;
}

void Queue::Clear()
{
    std::lock_guard lock(Mutex);
    if (Index >= 0 && Index < static_cast<int64_t>(Order.size())) {
        const std::string current = Order[Index];
        Order = { current };
        Base = { current };
        Index = 0;
    }
    else {
        Order.clear();
        Base.clear();
        Index = -1;
    }
    Queued = 0;
    Broadcast();
}

void Queue::Stop()
{
    std::lock_guard lock(Mutex);
    Player::Stop();
    Order.clear();
    Base.clear();
    Index = -1;
    Queued = 0;
    Source.clear();
    Broadcast();
}

void Queue::SetShuffle(bool bOn)
{
    std::lock_guard lock(Mutex);
    bShuffle = bOn;
    if (Index >= 0 && Index < static_cast<int64_t>(Order.size())) {
        const std::string current = Order[Index];
        ClampQueued();
        const std::vector<std::string> queued(Order.begin() + Index + 1, Order.begin() + Index + 1 + Queued);
        if (bShuffle) {
            std::vector<std::string> rest = Base;
            for (const std::string& id : queued) {
                if (auto it = std::find(rest.begin(), rest.end(), id); it != rest.end())
                    rest.erase(it);
            }
            if (auto it = std::find(rest.begin(), rest.end(), current); it != rest.end())
                rest.erase(it);
            Order = { current };
            Order.insert(Order.end(), queued.begin(), queued.end());
            for (auto& id : Shuffled(std::move(rest)))
                Order.push_back(std::move(id));
            Index = 0;
        }
        else {
            std::vector<std::string> rest = Base;
            for (const std::string& id : queued) {
                if (auto it = std::find(rest.begin(), rest.end(), id); it != rest.end())
                    rest.erase(it);
            }
            auto it = std::find(rest.begin(), rest.end(), current);
            const size_t at = it == rest.end() ? 0 : static_cast<size_t>(it - rest.begin());
            if (it == rest.end())
                rest.insert(rest.begin(), current);
            rest.insert(rest.begin() + at + 1, queued.begin(), queued.end());
            Order = rest;
            Base = rest;
            Index = static_cast<int64_t>(at);
        }
    }
    Broadcast();
}

void Queue::SetRepeat(RepeatMode Mode)
{
    std::lock_guard lock(Mutex);
    Repeat = Mode;
    Broadcast();
}

std::string Queue::StateJson()
{
    std::lock_guard lock(Mutex);
    return nlohmann::json{
        { "order", Order },
        { "index", Index },
        { "queued", Queued },
        { "shuffle", bShuffle },
        { "repeat", RepeatName(Repeat) },
        { "source", Source },
    }.dump();
}

void Queue::OnChanged(std::function<void(const std::string&)> Handler)
{
    ChangedHandler = std::move(Handler);
}

void Queue::Persist(bool bForce)
{
    std::unique_lock lock(Mutex, std::try_to_lock);
    if (!lock.owns_lock())
        return;

    const auto now = std::chrono::steady_clock::now();
    if (!bForce && now - LastPersist < std::chrono::seconds(5))
        return;
    LastPersist = now;

    CachedQueue state;
    state.Order = Order;
    state.Base = Base;
    state.Index = Index;
    state.Queued = Queued;
    state.Source = Source;
    if (Index >= 0 && Index < static_cast<int64_t>(Order.size())) {
        const PlayerState player = Player::GetState();
        if (player.FileId == Order[Index] && player.Status != PlayerStatus::Ended)
            state.Position = player.Position;
    }
    AppData::SaveQueue(state);
}

void Queue::Broadcast()
{
    Persist(true);
    std::vector<std::string> upcoming;
    if (Index >= 0 && Index < static_cast<int64_t>(Order.size())) {
        upcoming.assign(Order.begin() + Index, Order.end());
        if (Repeat == RepeatMode::All)
            upcoming.insert(upcoming.end(), Order.begin(), Order.begin() + Index);
    }
    Lyrics::FollowQueue(std::move(upcoming));
    if (ChangedHandler)
        ChangedHandler(StateJson());
}

RepeatMode Queue::ParseRepeat(const std::string& Name)
{
    if (Name == "all")
        return RepeatMode::All;
    if (Name == "one")
        return RepeatMode::One;
    return RepeatMode::Off;
}

std::string Queue::RepeatName(RepeatMode Mode)
{
    switch (Mode) {
    case RepeatMode::All: return "all";
    case RepeatMode::One: return "one";
    default:              return "off";
    }
}
