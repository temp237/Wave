#pragma once
#ifndef CURL_STATICLIB
#define CURL_STATICLIB
#endif
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include <algorithm>

const std::string CLIENT_ID = "SSdQ80vM8nLPhbDBylHl2JFK6ElhBr9B"; // client id from .apk strings.xml
const std::string BASE_API = "https://api-v2.soundcloud.com";

struct HttpResponse {
	bool bFailed;
	std::string Body;
	int32_t StatusCode;
	std::string Error;

	nlohmann::json GetBodyAsJson();
	bool DidSucceed();
};

struct Transcoding {
	std::string Url;
	std::string Protocol;
	std::string MimeType;
	int Rank;

	std::string Extension() const;
};

struct TrackBlob {
	std::string Id;
	std::string ArtworkURL;
	int32_t Duration;
	int32_t Likes;
	int32_t Playbacks;
	int32_t Reposts;
	std::string Artist;
	std::string Title;
	std::string PermalinkURL;
	std::string HighestTranscodingURL;
	std::vector<Transcoding> Transcodings;
	std::string TrackAuthorization;

	nlohmann::json ToJson() const;
};

class OnlineDownloader
{
public:
	using ProgressCallback = std::function<void(double)>;
	static inline const std::string LibraryFolder = "Downloads";
	static inline const std::string PreviewPrefix = "preview:";

private:
	struct Transfer {
		std::string* Body;
		ProgressCallback OnProgress;
		double LastReported;
	};

	static inline std::mutex Mutex;
	static inline std::unordered_map<std::string, TrackBlob> Known;

	static size_t WriteCallback(char* Ptr, size_t Size, size_t Count, void* User);
	static int ProgressCallbackThunk(void* User, curl_off_t Total, curl_off_t Now, curl_off_t, curl_off_t);
	static HttpResponse HttpGet(std::string url, long TimeoutSeconds = 30, const ProgressCallback& OnProgress = {});
	static std::string UrlEncode(std::string Base);
	static std::string SanitizeFileName(std::string Str, char Replacement = '_');
	static std::string JsonString(const nlohmann::json& Object, const char* Key);
	static int32_t JsonInt(const nlohmann::json& Object, const char* Key);
	static int TranscodingRank(const nlohmann::json& Format, const std::string& Preset);
	static std::optional<TrackBlob> ParseTrack(const nlohmann::json& Track);
	static std::string ResolveUrl(const std::string& Base, const std::string& Relative);
	static std::optional<std::string> FetchHls(const std::string& PlaylistUrl, const ProgressCallback& OnProgress, std::string& Error);
	static std::string SyncSafe(uint32_t Value);
	static std::string Id3Frame(const std::string& Id, const std::string& Body);
	static std::string FetchArtwork(const TrackBlob& Blob);
	static std::string BuildId3(const TrackBlob& Blob, const std::string& Artwork);
	static bool Remux(const std::filesystem::path& Input, const std::filesystem::path& Output, const TrackBlob& Blob, const std::string& Artwork);
	static std::optional<std::string> FetchAudio(const TrackBlob& Blob, const Transcoding& Stream, const ProgressCallback& OnProgress, std::string& Error);
	static std::optional<std::string> FetchBest(const TrackBlob& Blob, const ProgressCallback& OnProgress, std::string& Error, std::string& Extension);
	static void SplitArtist(TrackBlob& Blob);
	static std::filesystem::path PreviewDirectory();
	static std::optional<std::filesystem::path> CachedPreview(const std::string& TrackId);

public:
	static void Init();
	static std::filesystem::path DownloadsDirectory();
	static std::vector<TrackBlob> Search(std::string Query, std::string& Error);
	static std::optional<TrackBlob> Find(const std::string& TrackId);
	static std::string GetMediaLink(TrackBlob Blob, const Transcoding& Stream);
	static std::optional<std::filesystem::path> Preview(const std::string& TrackId, std::string& Error);
	static std::optional<std::filesystem::path> Download(const std::string& TrackId, const std::filesystem::path& Destination, const ProgressCallback& OnProgress, std::string& Error);
};
