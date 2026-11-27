#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <array>
#include <cwctype>

#include "AppData.hpp"
#include "Logger.hpp"
#include "Encryption.hpp"
#include "Utils.hpp"
#include "Player.hpp"
#include "Metadata.hpp"
#include "TaskQueue.hpp"

#include <cctype>
#include <chrono>
#include <random>
#include <regex>
#include <unordered_map>
#include <unordered_set>

const std::string& File::Id() const
{
    if (IdCache.empty())
        IdCache = Encryption::Sha256(Utils::ToUtf8(Path));
    return IdCache;
}

File::DisplayInfo File::Display() const
{
    auto trim = [](std::string s) {
        const size_t first = s.find_first_not_of(" \t");
        if (first == std::string::npos)
            return std::string();
        return s.substr(first, s.find_last_not_of(" \t") - first + 1);
    };
    auto lower = [](std::string s) {
        for (char& c : s)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };

    std::string base = trim(Title);
    if (base.empty()) {
        base = Path.has_stem() ? Utils::ToUtf8(Path.stem()) : Name;
        std::replace(base.begin(), base.end(), '_', ' ');
        base = std::regex_replace(base, std::regex(R"(\s+)"), " ");
        base = trim(std::regex_replace(trim(base), std::regex(R"(^\d{1,3}\s*[-.)]\s+)"), ""));
    }

    DisplayInfo info{ base.empty() ? Name : base, trim(Artist) };
    const size_t split = base.find(" - ");
    if (split != std::string::npos && split > 0) {
        const std::string prefix = trim(base.substr(0, split));
        const std::string rest = trim(base.substr(split + 3));
        if (!prefix.empty() && !rest.empty()) {
            if (info.Artist.empty()) {
                info.Artist = prefix;
                info.Title = rest;
            }
            else if (lower(info.Artist).rfind(lower(prefix), 0) == 0) {
                info.Title = rest;
            }
        }
    }
    return info;
}

nlohmann::json File::ToJson(bool bIncludeLyrics) const
{
    nlohmann::json j;
    const DisplayInfo display = Display();
    j["displayTitle"] = display.Title;
    j["displayArtist"] = display.Artist;
    j["name"] = Name;
    j["path"] = Utils::ToUtf8(Path);
    j["duration"] = Duration;
    j["title"] = Title;
    j["artist"] = Artist;
    j["album"] = Album;
    j["cover"] = !CoverKey.empty();
    j["coverKey"] = CoverKey;
    j["scanned"] = bScanned;
    j["lyricsStatus"] = LyricsStatus;
    j["lyricsVersion"] = LyricsVersion;
    if (bIncludeLyrics) {
        nlohmann::json lines = nlohmann::json::array();
        for (const LyricLine& line : Lyrics)
            lines.push_back({ { "start", line.Start }, { "end", line.End }, { "text", line.Text } });
        j["lyrics"] = std::move(lines);
    }
    return j;
}

std::optional<File> File::FromJson(const nlohmann::json& j)
{
    if (!j.is_object() || !j.contains("name") || !j.contains("path")
        || !j["name"].is_string() || !j["path"].is_string())
        return std::nullopt;
    File file(j["name"].get<std::string>(), Utils::FromUtf8(j["path"].get<std::string>()));
    file.Duration = j.value("duration", 0.0);
    file.Title = j.value("title", std::string());
    file.Artist = j.value("artist", std::string());
    file.Album = j.value("album", std::string());
    file.CoverKey = j.value("coverKey", std::string());
    file.bLegacyCover = file.CoverKey.empty() && j.value("cover", false);
    file.bScanned = j.value("scanned", false);
    file.LyricsStatus = j.value("lyricsStatus", std::string());
    file.LyricsVersion = j.value("lyricsVersion", file.LyricsStatus.empty() ? 0 : 1);
    if (auto it = j.find("lyrics"); it != j.end() && it->is_array()) {
        for (const auto& line : *it) {
            if (line.is_object())
                file.Lyrics.push_back({ line.value("start", 0.0), line.value("end", 0.0), line.value("text", std::string()) });
        }
    }
    return file;
}

nlohmann::json Folder::ToJson() const
{
    nlohmann::json folders = nlohmann::json::array();
    for (const auto& sub : Folders)
        folders.push_back(sub.ToJson());
    nlohmann::json files = nlohmann::json::array();
    for (const auto& file : Files) {
        nlohmann::json f = file.ToJson();
        f["id"] = file.Id();
        files.push_back(std::move(f));
    }
    return { { "name", Name }, { "folders", folders }, { "files", files } };
}

size_t Folder::CountFiles() const
{
    size_t n = Files.size();
    for (const auto& sub : Folders)
        n += sub.CountFiles();
    return n;
}

nlohmann::json Playlist::ToJson() const
{
    nlohmann::json j;
    j["id"] = Id;
    j["name"] = Name;
    j["tracks"] = nlohmann::json::array();
    for (const auto& track : Tracks) {
        j["tracks"].push_back(track);
    }
    j["createdAt"] = CreatedAt;
    return j;
}

std::optional<Playlist> Playlist::FromJson(const nlohmann::json& j)
{
    if (!j.is_object() || j.contains("name") == false || j.contains("tracks") == false || !j["name"].is_string() || !j["tracks"].is_array()) {
        return std::nullopt;
    }
    std::vector<std::string> tracks;
    for (const auto& track : j["tracks"]) {
        if (track.is_string()) {
            tracks.push_back(track.get<std::string>());
        }
    }
    Playlist file(j["name"].get<std::string>(), tracks);
    file.Id = j.value("id", std::string());
    file.CreatedAt = j.value("createdAt", int64_t(0));
    return file;
}

std::filesystem::path AppData::Directory()
{
    static std::string createError;
    static const std::filesystem::path dir = [] {
        std::filesystem::path base;
        wchar_t buf[MAX_PATH * 2] = {};
        const DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, static_cast<DWORD>(std::size(buf)));
        if (len > 0 && len < std::size(buf))
            base = buf;
        if (base.empty())
            base = std::filesystem::current_path();

        std::filesystem::path wave = base / "Wave";
        std::error_code ec;
        std::filesystem::create_directories(wave, ec);
        if (ec)
            createError = ec.message();
        return wave;
    }();
    static std::atomic<bool> bLogged = false;
    if (!bLogged.exchange(true)) {
        if (!createError.empty())
            Logger::Error("[AppData] Couldn't create " + Utils::ToUtf8(dir) + ": " + createError);
        else
            Logger::Info("[AppData] Data folder: " + Utils::ToUtf8(dir));
    }
    return dir;
}

std::filesystem::path AppData::CoversDirectory()
{
    return Directory() / "covers";
}

std::filesystem::path AppData::CoverPath(const std::string& Key)
{
    return CoversDirectory() / (Key + ".jpg");
}

std::filesystem::path AppData::LegacyCoverPath(const std::string& Id)
{
    return CoversDirectory() / (Id + ".img");
}

bool AppData::IsCoverKey(const std::string& Key)
{
    return Key.size() == 22 && Key.find_first_not_of("0123456789abcdef") == std::string::npos;
}

void AppData::LoadCoverIndex()
{
    if (bCoverIndexLoaded)
        return;
    bCoverIndexLoaded = true;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(CoversDirectory(), ec)) {
        const std::string key = Utils::ToUtf8(entry.path().stem());
        if (entry.path().extension() == ".jpg" && IsCoverKey(key))
            CoverIndex.push_back({ std::stoull(key.substr(0, 16), nullptr, 16), static_cast<uint32_t>(std::stoul(key.substr(16), nullptr, 16)), key });
    }
}

std::string AppData::StoreCover(const CoverImage& Cover)
{
    std::lock_guard lock(CoverMutex);
    LoadCoverIndex();
    for (const auto& entry : CoverIndex) {
        if (Metadata::HashDistance(entry.Hash, Cover.Hash) <= 10 && Metadata::ColorDistance(entry.Color, Cover.Color) <= 24)
            return entry.Key;
    }

    char key[23];
    std::snprintf(key, sizeof(key), "%016llx%06x", static_cast<unsigned long long>(Cover.Hash), Cover.Color & 0xFFFFFF);
    std::error_code ec;
    std::filesystem::create_directories(CoversDirectory(), ec);
    if (!WriteFile(CoverPath(key), Cover.Jpeg))
        return {};
    CoverIndex.push_back({ Cover.Hash, Cover.Color, key });
    return key;
}

bool AppData::WriteFile(const std::filesystem::path& Path, const std::string& Body)
{
    std::filesystem::path tmp = Path;
    tmp += ".tmp";
    {
        std::ofstream file(tmp, std::ios::binary | std::ios::trunc);
        if (!file.is_open()) {
            Logger::Error("[AppData] Failed to open file for writing " + Utils::ToUtf8(Path.filename()));
            return false;
        }
        file.write(Body.data(), static_cast<std::streamsize>(Body.size()));
        if (!file.good()) {
            Logger::Error("[AppData] Failed to write file " + Utils::ToUtf8(Path.filename()));
            return false;
        }
    }
    if (!MoveFileExW(tmp.c_str(), Path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        Logger::Error("[AppData] Failed to replace " + Utils::ToUtf8(Path.filename()) + " (error " + std::to_string(GetLastError()) + ")");
        return false;
    }
    return true;
}

std::string AppData::ReadFile(const std::filesystem::path& Path)
{
    std::error_code ec;
    if (!std::filesystem::exists(Path, ec))
        return {};

    std::ifstream file(Path, std::ios::binary);
    if (!file.is_open()) {
        Logger::Error("[AppData] Failed to open file for reading " + Utils::ToUtf8(Path.filename()));
        return {};
    }
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

nlohmann::json AppData::ParseJson(const std::string& Body)
{
    if (Body.empty())
        return nlohmann::json::object();
    nlohmann::json j = nlohmann::json::parse(Body, nullptr, false);
    return j.is_discarded() ? nlohmann::json() : j;
}

nlohmann::ordered_json AppData::ParseOrderedJson(const std::string& Body)
{
    if (Body.empty())
        return nlohmann::ordered_json::object();
    nlohmann::ordered_json j = nlohmann::ordered_json::parse(Body, nullptr, false);
    return j.is_discarded() ? nlohmann::ordered_json() : j;
}

nlohmann::ordered_json AppData::TreeToJson(const Folder& Node)
{
    nlohmann::ordered_json folders = nlohmann::ordered_json::object();
    for (const auto& sub : Node.Folders)
        folders[sub.Name] = TreeToJson(sub);
    nlohmann::ordered_json files = nlohmann::ordered_json::array();
    for (const auto& file : Node.Files)
        files.push_back(file.Id());
    return { { "folders", folders }, { "files", files } };
}

Folder AppData::TreeFromJson(const std::string& Name, const nlohmann::ordered_json& FolderJson, const nlohmann::json& HashMap)
{
    Folder folder;
    folder.Name = Name;
    if (!FolderJson.is_object())
        return folder;

    if (auto it = FolderJson.find("folders"); it != FolderJson.end() && it->is_object()) {
        for (const auto& [subName, subJson] : it->items())
            folder.Folders.push_back(TreeFromJson(subName, subJson, HashMap));
    }
    if (auto it = FolderJson.find("files"); it != FolderJson.end() && it->is_array()) {
        for (const auto& id : *it) {
            if (!id.is_string())
                continue;
            auto entry = HashMap.find(id.get<std::string>());
            if (entry == HashMap.end()) {
                Logger::Warning("[AppData] File " + id.get<std::string>().substr(0, 12) + "... missing from filehashmap.json, skipped");
                continue;
            }
            if (auto file = File::FromJson(*entry))
                folder.Files.push_back(std::move(*file));
        }
    }
    return folder;
}

void AppData::CollectFiles(const Folder& Node, nlohmann::json& HashMap)
{
    for (const auto& file : Node.Files)
        HashMap[file.Id()] = file.ToJson(true);
    for (const auto& sub : Node.Folders)
        CollectFiles(sub, HashMap);
}

void AppData::ScanFile(File& Target)
{
    const TrackMetadata meta = Metadata::Read(Target.Path);
    Target.Title = meta.Title;
    Target.Artist = meta.Artist;
    Target.Album = meta.Album;
    Target.Duration = meta.Duration > 0 ? meta.Duration : Player::ProbeDuration(Target.Path);
    const std::optional<CoverImage> cover = Metadata::MakeCover(Target.Path);
    Target.CoverKey = cover ? StoreCover(*cover) : std::string();
    Target.bLegacyCover = false;
    Target.bScanned = true;
}

Folder AppData::ScanFolder(const std::filesystem::path& Path)
{
    Folder folder(Utils::ToUtf8(Path.filename()), {}, {});
    std::vector<std::filesystem::directory_entry> entries;
    std::error_code iterEc;
    for (const auto& entry : std::filesystem::directory_iterator(Path, std::filesystem::directory_options::skip_permission_denied, iterEc))
        entries.push_back(entry);
    if (iterEc)
        Logger::Warning("[AppData] Couldn't fully read " + Utils::ToUtf8(Path) + ": " + iterEc.message());

    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return a.path().filename() < b.path().filename(); });
    for (const auto& entry : entries) {
        std::error_code e;
        if (entry.is_directory(e) && !entry.is_symlink(e)) {
            Folder sub = ScanFolder(entry.path());
            if (sub.CountFiles() > 0)
                folder.Folders.push_back(std::move(sub));
        }
        else if (entry.is_regular_file(e) && IsAudioFile(entry.path())) {
            folder.Files.push_back(File(Utils::ToUtf8(entry.path().stem()), entry.path()));
        }
    }
    return folder;
}

void AppData::UpdateFileTree(const std::vector<Folder>& Filetree)
{
    std::lock_guard lock(Mutex);

    nlohmann::json fileHashMap = nlohmann::json::object();
    nlohmann::ordered_json fileTree = nlohmann::ordered_json::object();
    for (const auto& folder : Filetree) {
        CollectFiles(folder, fileHashMap);
        fileTree[folder.Name] = TreeToJson(folder);
    }

    const bool ok = WriteFile(Directory() / "filehashmap.json", fileHashMap.dump(2))
        && WriteFile(Directory() / "filetree.json", fileTree.dump(2));

    FileTreeCache = Filetree;
    bFileTreeCached = true;
    if (ok)
        Logger::Debug("[AppData] Saved file tree (" + std::to_string(fileHashMap.size()) + " files)");
}

std::vector<Folder> AppData::GetFileTree(bool bAllowCache)
{
    std::lock_guard lock(Mutex);
    if (bAllowCache && bFileTreeCached)
        return FileTreeCache;

    const nlohmann::json hashMap = ParseJson(ReadFile(Directory() / "filehashmap.json"));
    const nlohmann::ordered_json tree = ParseOrderedJson(ReadFile(Directory() / "filetree.json"));
    if (!hashMap.is_object() || !tree.is_object())
        Logger::Warning("[AppData] Saved file tree is unreadable, starting empty");

    std::vector<Folder> folders;
    if (hashMap.is_object() && tree.is_object()) {
        for (const auto& [name, folderJson] : tree.items())
            folders.push_back(TreeFromJson(name, folderJson, hashMap));
    }

    FileTreeCache = std::move(folders);
    bFileTreeCached = true;
    return FileTreeCache;
}

void AppData::CollectFiles(Folder& Node, std::vector<File*>& Out, bool bOnlyUnscanned)
{
    for (auto& file : Node.Files) {
        if (!bOnlyUnscanned || !file.bScanned)
            Out.push_back(&file);
    }
    for (auto& sub : Node.Folders)
        CollectFiles(sub, Out, bOnlyUnscanned);
}

void AppData::ScanFiles(const std::vector<File*>& Files)
{
    TaskQueue::ParallelFor(Files.size(), [&](size_t i) { ScanFile(*Files[i]); });
}

void AppData::RefreshMetadata()
{
    std::vector<Folder> tree = GetFileTree();
    std::vector<File*> all;
    for (auto& folder : tree)
        CollectFiles(folder, all, false);

    std::vector<File*> pending;
    std::vector<File*> legacy;
    for (File* file : all) {
        if (!file->bScanned)
            pending.push_back(file);
        else if (file->bLegacyCover)
            legacy.push_back(file);
    }
    if (pending.empty() && legacy.empty())
        return;

    ScanFiles(pending);
    TaskQueue::ParallelFor(legacy.size(), [&](size_t i) {
        File& file = *legacy[i];
        const std::filesystem::path old = LegacyCoverPath(file.Id());
        const std::optional<CoverImage> cover = Metadata::CoverFromBytes(ReadFile(old));
        file.CoverKey = cover ? StoreCover(*cover) : std::string();
        file.bLegacyCover = false;
        std::error_code ec;
        std::filesystem::remove(old, ec);
    });
    UpdateFileTree(tree);
    Logger::Info("[AppData] Read metadata for " + std::to_string(pending.size()) + " files, moved " + std::to_string(legacy.size()) + " old covers to shared covers");
}

std::optional<std::pair<std::string, std::string>> AppData::GetCover(const std::string& Key)
{
    if (!IsCoverKey(Key))
        return std::nullopt;
    const std::string bytes = ReadFile(CoverPath(Key));
    if (bytes.empty())
        return std::nullopt;
    return std::make_pair(Metadata::ImageMime(bytes), Utils::Base64(bytes));
}

std::vector<std::vector<std::string>> AppData::GetCovers(const std::vector<std::string>& Keys)
{
    std::vector<std::optional<std::pair<std::string, std::string>>> found(Keys.size());
    TaskQueue::ParallelFor(Keys.size(), [&](size_t i) { found[i] = GetCover(Keys[i]); });

    std::vector<std::vector<std::string>> out;
    for (size_t i = 0; i < Keys.size(); i++) {
        if (found[i])
            out.push_back({ Keys[i], found[i]->first, std::move(found[i]->second) });
    }
    return out;
}

nlohmann::json AppData::CacheInfo()
{
    uint64_t bytes = 0;
    uint64_t files = 0;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(CoversDirectory(), ec)) {
        std::error_code e;
        if (entry.is_regular_file(e)) {
            bytes += entry.file_size(e);
            files++;
        }
    }
    return { { "bytes", bytes }, { "files", files } };
}

uint64_t AppData::ClearCoverCache()
{
    std::lock_guard lock(Mutex);
    GetFileTree();
    const uint64_t freed = CacheInfo()["bytes"].get<uint64_t>();
    {
        std::lock_guard coverLock(CoverMutex);
        std::error_code ec;
        std::filesystem::remove_all(CoversDirectory(), ec);
        CoverIndex.clear();
        bCoverIndexLoaded = false;
    }

    std::vector<File*> all;
    for (auto& folder : FileTreeCache)
        CollectFiles(folder, all, false);
    for (File* file : all) {
        file->CoverKey.clear();
        file->bLegacyCover = false;
        file->bScanned = false;
    }
    UpdateFileTree(FileTreeCache);
    Logger::Info("[AppData] Cleared cover cache (" + std::to_string(freed / 1024) + " KB)");
    return freed;
}

const File* AppData::FindFileIn(const Folder& Node, const std::string& Id)
{
    for (const auto& file : Node.Files) {
        if (file.Id() == Id)
            return &file;
    }
    for (const auto& sub : Node.Folders) {
        if (const File* found = FindFileIn(sub, Id))
            return found;
    }
    return nullptr;
}

std::optional<File> AppData::FindFile(const std::string& Id)
{
    std::lock_guard lock(Mutex);
    GetFileTree();
    for (const auto& folder : FileTreeCache) {
        if (const File* found = FindFileIn(folder, Id))
            return *found;
    }
    return std::nullopt;
}

std::string AppData::GetFileTreeJson(bool bAllowCache)
{
    nlohmann::json folders = nlohmann::json::array();
    for (const auto& folder : GetFileTree(bAllowCache))
        folders.push_back(folder.ToJson());
    return nlohmann::json{ { "folders", folders } }.dump();
}

std::vector<Playlist> AppData::GetPlaylists(bool bAllowCache)
{
    std::lock_guard lock(Mutex);
    if (bAllowCache && bPlaylistsCached)
        return PlaylistsCache;

    const nlohmann::json playlistsFile = ParseJson(ReadFile(Directory() / "playlists.json"));
    if (!playlistsFile.is_object())
        Logger::Warning("[AppData] Saved playlists file is unreadable, starting empty");

    std::vector<Playlist> playlists;
    if (playlistsFile.is_object()) {
        for (const auto& [name, playlistData] : playlistsFile.items()) {
            std::optional<Playlist> p = Playlist::FromJson(playlistData);
            if (p.has_value()) {
                if (p->Id.empty())
                    p->Id = name;
                playlists.push_back(p.value());
            }
        }
    }
    std::sort(playlists.begin(), playlists.end(), [](const Playlist& a, const Playlist& b) { return a.CreatedAt < b.CreatedAt; });

    PlaylistsCache = std::move(playlists);
    bPlaylistsCached = true;
    return PlaylistsCache;
}

std::string AppData::GetPlaylistsJson()
{
    nlohmann::json list = nlohmann::json::array();
    for (const auto& p : GetPlaylists())
        list.push_back(p.ToJson());
    return list.dump();
}

bool AppData::SaveQueue(const CachedQueue& State)
{
    const bool hasCurrent = State.Index >= 0 && State.Index < static_cast<int64_t>(State.Order.size());
    nlohmann::json j = nlohmann::json::object();
    j["playingTrack"]["hash"] = hasCurrent ? State.Order[State.Index] : std::string();
    j["playingTrack"]["position"] = hasCurrent ? State.Position : 0.0;
    j["queue"] = State.Order;
    j["base"] = State.Base;
    j["index"] = State.Index;
    j["queued"] = State.Queued;
    j["source"] = State.Source;
    return WriteFile(Directory() / "queue.json", j.dump());
}

std::vector<std::pair<std::string, std::filesystem::path>> AppData::AllFilePaths()
{
    std::lock_guard lock(Mutex);
    GetFileTree();
    std::vector<File*> files;
    for (auto& folder : FileTreeCache)
        CollectFiles(folder, files, false);
    std::vector<std::pair<std::string, std::filesystem::path>> out;
    out.reserve(files.size());
    for (const File* file : files)
        out.emplace_back(file->Id(), file->Path);
    return out;
}

std::vector<std::pair<std::string, std::filesystem::path>> AppData::FilesNeedingLyrics(int Version)
{
    std::lock_guard lock(Mutex);
    GetFileTree();
    std::vector<File*> files;
    for (auto& folder : FileTreeCache)
        CollectFiles(folder, files, false);
    std::vector<std::pair<std::string, std::filesystem::path>> missing;
    std::vector<std::pair<std::string, std::filesystem::path>> stale;
    for (const File* file : files) {
        if (file->LyricsStatus.empty())
            missing.emplace_back(file->Id(), file->Path);
        else if (file->LyricsVersion < Version && file->LyricsStatus != "skipped")
            stale.emplace_back(file->Id(), file->Path);
    }
    missing.insert(missing.end(), stale.begin(), stale.end());
    return missing;
}

bool AppData::SetLyrics(const std::string& Id, std::vector<LyricLine> Lines, const std::string& Status, int Version)
{
    std::lock_guard lock(Mutex);
    GetFileTree();
    std::vector<File*> files;
    for (auto& folder : FileTreeCache)
        CollectFiles(folder, files, false);
    for (File* file : files) {
        if (file->Id() == Id) {
            file->Lyrics = std::move(Lines);
            file->LyricsStatus = Status;
            file->LyricsVersion = Version;
            return true;
        }
    }
    return false;
}

size_t AppData::ClearLyrics()
{
    std::lock_guard lock(Mutex);
    GetFileTree();
    std::vector<File*> files;
    for (auto& folder : FileTreeCache)
        CollectFiles(folder, files, false);
    size_t cleared = 0;
    for (File* file : files) {
        if (!file->Lyrics.empty())
            cleared++;
        file->Lyrics.clear();
        file->LyricsStatus.clear();
        file->LyricsVersion = 0;
    }
    UpdateFileTree(FileTreeCache);
    Logger::Info("[AppData] Cleared cached lyrics for " + std::to_string(cleared) + " songs");
    return cleared;
}

nlohmann::json AppData::LyricsInfo()
{
    std::lock_guard lock(Mutex);
    GetFileTree();
    std::vector<File*> files;
    for (auto& folder : FileTreeCache)
        CollectFiles(folder, files, false);
    size_t cached = 0;
    for (const File* file : files) {
        if (!file->Lyrics.empty())
            cached++;
    }
    return { { "cached", cached }, { "total", files.size() } };
}

void AppData::SaveFileTree()
{
    std::lock_guard lock(Mutex);
    GetFileTree();
    UpdateFileTree(FileTreeCache);
}

std::unordered_set<std::string> AppData::AllFileIds()
{
    std::lock_guard lock(Mutex);
    GetFileTree();
    std::vector<File*> files;
    for (auto& folder : FileTreeCache)
        CollectFiles(folder, files, false);
    std::unordered_set<std::string> ids;
    for (const File* file : files)
        ids.insert(file->Id());
    return ids;
}

CachedQueue AppData::GetCachedQueue()
{
    CachedQueue cached;
    const nlohmann::json j = ParseJson(ReadFile(Directory() / "queue.json"));
    if (!j.is_object())
        return cached;

    const std::unordered_set<std::string> library = AllFileIds();
    const auto readIds = [&](const char* key) {
        std::vector<std::string> ids;
        if (auto it = j.find(key); it != j.end() && it->is_array()) {
            for (const auto& id : *it) {
                if (id.is_string() && library.count(id.get<std::string>()))
                    ids.push_back(id.get<std::string>());
            }
        }
        return ids;
    };
    cached.Order = readIds("queue");
    cached.Base = readIds("base");
    if (cached.Base.empty())
        cached.Base = cached.Order;
    if (auto it = j.find("source"); it != j.end() && it->is_string())
        cached.Source = it->get<std::string>();

    std::string playing;
    double position = 0;
    if (auto it = j.find("playingTrack"); it != j.end() && it->is_object()) {
        if (auto hash = it->find("hash"); hash != it->end() && hash->is_string())
            playing = hash->get<std::string>();
        if (auto pos = it->find("position"); pos != it->end() && pos->is_number())
            position = pos->get<double>();
    }
    int64_t index = -1;
    if (auto it = j.find("index"); it != j.end() && it->is_number_integer())
        index = it->get<int64_t>();

    if (!playing.empty()) {
        if (index < 0 || index >= static_cast<int64_t>(cached.Order.size()) || cached.Order[index] != playing) {
            auto found = std::find(cached.Order.begin(), cached.Order.end(), playing);
            index = found == cached.Order.end() ? -1 : found - cached.Order.begin();
        }
    }
    else {
        index = -1;
    }
    cached.Index = index;
    if (auto it = j.find("queued"); index >= 0 && it != j.end() && it->is_number_integer())
        cached.Queued = std::clamp<int64_t>(it->get<int64_t>(), 0, static_cast<int64_t>(cached.Order.size()) - index - 1);
    cached.Position = index >= 0 ? std::max(0.0, position) : 0.0;
    return cached;
}

bool AppData::SavePlaylists()
{
    nlohmann::json j = nlohmann::json::object();
    for (const auto& p : PlaylistsCache)
        j[p.Id] = p.ToJson();
    return WriteFile(Directory() / "playlists.json", j.dump(2));
}

Playlist* AppData::FindPlaylist(const std::string& Id)
{
    auto it = std::find_if(PlaylistsCache.begin(), PlaylistsCache.end(), [&](const Playlist& p) { return p.Id == Id; });
    return it != PlaylistsCache.end() ? &*it : nullptr;
}

std::string AppData::NewPlaylistId()
{
    static std::mt19937_64 rng{ std::random_device{}() };
    static const char hex[] = "0123456789abcdef";
    for (;;) {
        std::string id(16, '0');
        for (auto& c : id)
            c = hex[rng() & 15];
        if (!FindPlaylist(id))
            return id;
    }
}

std::string AppData::CleanPlaylistName(const std::string& Name)
{
    const size_t start = Name.find_first_not_of(" \t\r\n");
    if (start == std::string::npos)
        return {};
    const size_t end = Name.find_last_not_of(" \t\r\n");
    std::string clean = Name.substr(start, end - start + 1);
    if (clean.size() > 200)
        clean.resize(200);
    return clean;
}

std::optional<Playlist> AppData::CreatePlaylist(const std::string& Name, const std::vector<std::string>& Tracks)
{
    const std::string name = CleanPlaylistName(Name);
    if (name.empty())
        return std::nullopt;

    std::lock_guard lock(Mutex);
    GetPlaylists();
    Playlist p(name, {});
    p.Id = NewPlaylistId();
    p.CreatedAt = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    for (const auto& track : Tracks) {
        if (std::find(p.Tracks.begin(), p.Tracks.end(), track) == p.Tracks.end())
            p.Tracks.push_back(track);
    }
    PlaylistsCache.push_back(p);
    if (!SavePlaylists()) {
        PlaylistsCache.pop_back();
        return std::nullopt;
    }
    Logger::Success("[AppData] Created playlist \"" + p.Name + "\"");
    return p;
}

bool AppData::RenamePlaylist(const std::string& Id, const std::string& Name)
{
    const std::string name = CleanPlaylistName(Name);
    if (name.empty())
        return false;

    std::lock_guard lock(Mutex);
    GetPlaylists();
    Playlist* p = FindPlaylist(Id);
    if (!p)
        return false;
    const std::string old = p->Name;
    p->Name = name;
    if (!SavePlaylists()) {
        p->Name = old;
        return false;
    }
    Logger::Info("[AppData] Renamed playlist \"" + old + "\" to \"" + name + "\"");
    return true;
}

bool AppData::DeletePlaylist(const std::string& Id)
{
    std::lock_guard lock(Mutex);
    GetPlaylists();
    auto it = std::find_if(PlaylistsCache.begin(), PlaylistsCache.end(), [&](const Playlist& p) { return p.Id == Id; });
    if (it == PlaylistsCache.end())
        return false;
    const Playlist removed = *it;
    PlaylistsCache.erase(it);
    if (!SavePlaylists()) {
        PlaylistsCache.push_back(removed);
        return false;
    }
    Logger::Info("[AppData] Deleted playlist \"" + removed.Name + "\"");
    return true;
}

std::optional<size_t> AppData::AddToPlaylist(const std::string& Id, const std::vector<std::string>& Tracks)
{
    std::lock_guard lock(Mutex);
    GetPlaylists();
    Playlist* p = FindPlaylist(Id);
    if (!p)
        return std::nullopt;
    const size_t before = p->Tracks.size();
    for (const auto& track : Tracks) {
        if (std::find(p->Tracks.begin(), p->Tracks.end(), track) == p->Tracks.end())
            p->Tracks.push_back(track);
    }
    const size_t added = p->Tracks.size() - before;
    if (added && !SavePlaylists()) {
        p->Tracks.resize(before);
        return std::nullopt;
    }
    return added;
}

size_t AppData::RemoveFilesIn(Folder& Node, const std::unordered_set<std::string>& Ids)
{
    const size_t before = Node.Files.size();
    Node.Files.erase(std::remove_if(Node.Files.begin(), Node.Files.end(), [&](const File& f) { return Ids.count(f.Id()) > 0; }), Node.Files.end());
    size_t removed = before - Node.Files.size();
    for (auto& sub : Node.Folders)
        removed += RemoveFilesIn(sub, Ids);
    Node.Folders.erase(std::remove_if(Node.Folders.begin(), Node.Folders.end(), [](const Folder& f) { return f.CountFiles() == 0; }), Node.Folders.end());
    return removed;
}

size_t AppData::RemoveFromLibrary(const std::vector<std::string>& Ids)
{
    const std::unordered_set<std::string> ids(Ids.begin(), Ids.end());
    if (ids.empty())
        return 0;

    std::lock_guard lock(Mutex);
    GetFileTree();
    size_t removed = 0;
    for (auto& folder : FileTreeCache)
        removed += RemoveFilesIn(folder, ids);
    FileTreeCache.erase(std::remove_if(FileTreeCache.begin(), FileTreeCache.end(), [](const Folder& f) { return f.CountFiles() == 0; }), FileTreeCache.end());
    if (removed)
        UpdateFileTree(FileTreeCache);

    GetPlaylists();
    bool playlistsChanged = false;
    for (auto& playlist : PlaylistsCache) {
        const size_t before = playlist.Tracks.size();
        playlist.Tracks.erase(std::remove_if(playlist.Tracks.begin(), playlist.Tracks.end(), [&](const std::string& t) { return ids.count(t) > 0; }), playlist.Tracks.end());
        playlistsChanged |= playlist.Tracks.size() != before;
    }
    if (playlistsChanged)
        SavePlaylists();

    Logger::Info("[AppData] Removed " + std::to_string(removed) + " track" + (removed == 1 ? "" : "s") + " from the library");
    return removed;
}

std::optional<size_t> AppData::RemoveFromPlaylist(const std::string& Id, const std::vector<std::string>& Tracks)
{
    std::lock_guard lock(Mutex);
    GetPlaylists();
    Playlist* p = FindPlaylist(Id);
    if (!p)
        return std::nullopt;
    const std::unordered_set<std::string> remove(Tracks.begin(), Tracks.end());
    const std::vector<std::string> before = p->Tracks;
    p->Tracks.erase(std::remove_if(p->Tracks.begin(), p->Tracks.end(), [&](const std::string& track) { return remove.count(track) > 0; }), p->Tracks.end());
    const size_t removed = before.size() - p->Tracks.size();
    if (removed == 0)
        return size_t(0);
    if (!SavePlaylists()) {
        p->Tracks = before;
        return std::nullopt;
    }
    Logger::Info("[AppData] Removed " + std::to_string(removed) + " track" + (removed == 1 ? "" : "s") + " from \"" + p->Name + "\"");
    return removed;
}

bool AppData::IsAudioFile(const std::filesystem::path& Path)
{
    static const std::array<std::wstring, 13> extensions = {
        L".mp3", L".wav", L".ogg", L".oga", L".flac", L".m4a",
        L".mp4", L".mov", L".aac", L".opus", L".webm", L".weba", L".wma",
    };
    std::wstring ext = Path.extension().wstring();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return std::find(extensions.begin(), extensions.end(), ext) != extensions.end();
}

Folder& AppData::FindOrCreateFolder(const std::string& FolderPath)
{
    std::vector<std::string> parts = Utils::SplitPath(FolderPath);
    if (parts.empty())
        parts.push_back(LooseFolder);

    std::vector<Folder>* level = &FileTreeCache;
    Folder* folder = nullptr;
    for (const auto& part : parts) {
        auto it = std::find_if(level->begin(), level->end(), [&](const Folder& f) { return f.Name == part; });
        if (it == level->end()) {
            level->push_back(Folder(part, {}, {}));
            it = std::prev(level->end());
        }
        folder = &*it;
        level = &folder->Folders;
    }
    return *folder;
}

std::optional<std::string> AppData::RegisterFile(const std::filesystem::path& Path, const std::string& FolderPath)
{
    std::error_code ec;
    const std::filesystem::path full = std::filesystem::absolute(Path, ec).lexically_normal();
    if (ec || !std::filesystem::is_regular_file(full, ec)) {
        Logger::Error("[AppData] Not a file: " + Utils::ToUtf8(Path));
        return std::nullopt;
    }
    if (!IsAudioFile(full)) {
        Logger::Warning("[AppData] Not an audio file: " + Utils::ToUtf8(full.filename()));
        return std::nullopt;
    }

    File file(Utils::ToUtf8(full.stem()), full);
    const std::string id = file.Id();

    std::unique_lock lock(Mutex);
    GetFileTree();
    Folder* existing = &FindOrCreateFolder(FolderPath);
    if (std::any_of(existing->Files.begin(), existing->Files.end(), [&](const File& f) { return f.Id() == id; })) {
        Logger::Info("[AppData] Already registered: " + file.Name);
        return id;
    }
    lock.unlock();

    ScanFile(file);

    lock.lock();
    Folder& folder = FindOrCreateFolder(FolderPath);

    const bool exists = std::any_of(folder.Files.begin(), folder.Files.end(), [&](const File& f) { return f.Id() == id; });
    if (exists) {
        Logger::Info("[AppData] Already registered: " + file.Name);
        return id;
    }

    folder.Files.push_back(std::move(file));
    UpdateFileTree(FileTreeCache);
    Logger::Success("[AppData] Registered " + Utils::ToUtf8(full.filename()) + " in " + (FolderPath.empty() ? LooseFolder : FolderPath));
    return id;
}

std::optional<size_t> AppData::RegisterFolder(const std::filesystem::path& Path)
{
    std::error_code ec;
    const std::filesystem::path root = std::filesystem::absolute(Path, ec).lexically_normal();
    if (ec || !std::filesystem::is_directory(root, ec)) {
        Logger::Error("[AppData] Not a folder: " + Utils::ToUtf8(Path));
        return std::nullopt;
    }

    const auto started = std::chrono::steady_clock::now();
    Folder scanned = ScanFolder(root);
    if (scanned.Name.empty())
        scanned.Name = Utils::ToUtf8(root);
    const size_t count = scanned.CountFiles();

    std::vector<Folder> current = GetFileTree();
    std::vector<File*> existing;
    for (auto& folder : current)
        CollectFiles(folder, existing, false);
    std::unordered_map<std::string, File> known;
    for (File* file : existing) {
        if (file->bScanned)
            known.emplace(file->Id(), *file);
    }

    std::vector<File*> files;
    CollectFiles(scanned, files, false);
    std::vector<File*> pending;
    for (File* file : files) {
        auto it = known.find(file->Id());
        if (it != known.end())
            *file = it->second;
        else
            pending.push_back(file);
    }
    ScanFiles(pending);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    Logger::Info("[AppData] Scanned " + std::to_string(pending.size()) + " new files (" + std::to_string(count - pending.size()) + " already known) in " + std::to_string(elapsed) + " ms");

    std::lock_guard lock(Mutex);
    GetFileTree();
    auto it = std::find_if(FileTreeCache.begin(), FileTreeCache.end(), [&](const Folder& f) { return f.Name == scanned.Name; });
    if (it != FileTreeCache.end())
        *it = std::move(scanned);
    else
        FileTreeCache.push_back(std::move(scanned));

    UpdateFileTree(FileTreeCache);
    Logger::Success("[AppData] Registered folder " + Utils::ToUtf8(root.filename()) + " (" + std::to_string(count) + " audio files)");
    return count;
}