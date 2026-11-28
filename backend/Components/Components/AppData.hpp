#pragma once
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <unordered_set>
#include <string>
#include <vector>
#include <cstdint>

#include "Metadata.hpp"

struct LyricLine {
	double Start = 0.0;
	double End = 0.0;
	std::string Text;
};

struct File {
	std::string Name;
	std::filesystem::path Path;
	double Duration = 0;
	std::string Title;
	std::string Artist;
	std::string Album;
	std::string CoverKey;
	bool bLegacyCover = false;
	bool bScanned = false;
	std::string LyricsStatus;
	int LyricsVersion = 0;
	std::vector<LyricLine> Lyrics;
	double VocalStart = -1.0;
	double VocalEnd = -1.0;

	const std::string& Id() const;

	struct DisplayInfo {
		std::string Title;
		std::string Artist;
	};
	DisplayInfo Display() const;

	nlohmann::json ToJson(bool bIncludeLyrics = false) const;
	static std::optional<File> FromJson(const nlohmann::json& j);

	File() : Name(""), Path() {}
	File(std::string Name, std::filesystem::path Path) : Name(std::move(Name)), Path(std::move(Path)) {}

private:
	mutable std::string IdCache;
};

struct Folder {
	std::string Name;
	std::vector<Folder> Folders;
	std::vector<File> Files;

	Folder() : Name(""), Folders({}), Files({}) {};
	Folder(std::string Name, std::vector<Folder> Folders, std::vector<File> Files) : Name(Name), Folders(Folders), Files(Files) {}

	nlohmann::json ToJson() const;
	size_t CountFiles() const;
};

struct CachedQueue {
	std::vector<std::string> Order;
	std::vector<std::string> Base;
	int64_t Index = -1;
	int64_t Queued = 0;
	std::string Source;
	double Position = 0;
};

struct Playlist {
	std::string Id;
	std::string Name;
	// TODO: cover? maybe
	std::vector<std::string> Tracks;
	int64_t CreatedAt = 0;

	Playlist() : Name(""), Tracks({}) {};
	Playlist(std::string Name, std::vector<std::string> Tracks) : Name(Name), Tracks(Tracks) {};

	nlohmann::json ToJson() const;
	static std::optional<Playlist> FromJson(const nlohmann::json& j);
};

class AppData
{
public:
	static inline const std::string LooseFolder = "__loose";

private:
	static inline std::vector<Folder> FileTreeCache;
	static inline bool bFileTreeCached = false;
	static inline bool bPlaylistsCached = false;
	static inline std::recursive_mutex Mutex;
	static inline std::vector<Playlist> PlaylistsCache;
private:
	struct CoverEntry {
		uint64_t Hash;
		uint32_t Color;
		std::string Key;
	};
	static inline std::vector<CoverEntry> CoverIndex;
	static inline bool bCoverIndexLoaded = false;
	static inline std::mutex CoverMutex;

	static std::filesystem::path CoversDirectory();
	static std::filesystem::path CoverPath(const std::string& Key);
	static std::filesystem::path LegacyCoverPath(const std::string& Id);
	static bool IsCoverKey(const std::string& Key);
	static void LoadCoverIndex();
	static std::string StoreCover(const CoverImage& Cover);

	static nlohmann::json ParseJson(const std::string& Body);
	static nlohmann::ordered_json ParseOrderedJson(const std::string& Body);
	static nlohmann::ordered_json TreeToJson(const Folder& Node);
	static Folder TreeFromJson(const std::string& Name, const nlohmann::ordered_json& FolderJson, const nlohmann::json& HashMap);
	static void CollectFiles(const Folder& Node, nlohmann::json& HashMap);
	static void ScanFile(File& Target);
	static Folder ScanFolder(const std::filesystem::path& Path);
	static void CollectFiles(Folder& Node, std::vector<File*>& Out, bool bOnlyUnscanned);
	static void ScanFiles(const std::vector<File*>& Files);

	static void UpdateFileTree(const std::vector<Folder>& Filetree);
	static Folder& FindOrCreateFolder(const std::string& FolderPath);
	static const File* FindFileIn(const Folder& Node, const std::string& Id);
	static size_t RemoveFilesIn(Folder& Node, const std::unordered_set<std::string>& Ids);

	static bool SavePlaylists();
	static Playlist* FindPlaylist(const std::string& Id);
	static std::string NewPlaylistId();
	static std::string CleanPlaylistName(const std::string& Name);
public:
	static std::filesystem::path Directory();
	static bool WriteFile(const std::filesystem::path& Path, const std::string& Body);
	static std::string ReadFile(const std::filesystem::path& Path);

	static bool IsAudioFile(const std::filesystem::path& Path);

	static std::optional<std::string> RegisterFile(const std::filesystem::path& Path, const std::string& FolderPath = LooseFolder);
	static std::optional<size_t> RegisterFolder(const std::filesystem::path& Path);
	static std::optional<Playlist> CreatePlaylist(const std::string& Name, const std::vector<std::string>& Tracks = {});
	static bool RenamePlaylist(const std::string& Id, const std::string& Name);
	static bool DeletePlaylist(const std::string& Id);
	static std::optional<size_t> AddToPlaylist(const std::string& Id, const std::vector<std::string>& Tracks);
	static size_t RemoveFromLibrary(const std::vector<std::string>& Ids);
	static std::optional<size_t> RemoveFromPlaylist(const std::string& Id, const std::vector<std::string>& Tracks);
	static void RefreshMetadata();

	static std::optional<File> FindFile(const std::string& Id);
	static std::vector<std::pair<std::string, std::filesystem::path>> AllFilePaths();
	static std::vector<std::pair<std::string, std::filesystem::path>> FilesNeedingLyrics(int Version);
	static bool SetLyrics(const std::string& Id, std::vector<LyricLine> Lines, const std::string& Status, int Version, double VocalStart = -1.0, double VocalEnd = -1.0);
	static size_t ClearLyrics();
	static nlohmann::json LyricsInfo();
	static void SaveFileTree();
	static std::optional<std::pair<std::string, std::string>> GetCover(const std::string& Key);
	static std::vector<std::vector<std::string>> GetCovers(const std::vector<std::string>& Keys);
	static uint64_t ClearCoverCache();
	static nlohmann::json CacheInfo();
	static std::vector<Folder> GetFileTree(bool bAllowCache = true);
	static std::string GetFileTreeJson(bool bAllowCache = true);
	static std::vector<Playlist> GetPlaylists(bool bAllowCache = true);
	static std::string GetPlaylistsJson();

	static bool SaveQueue(const CachedQueue& State);
	static CachedQueue GetCachedQueue();
	static std::unordered_set<std::string> AllFileIds();
};
