#include <cstring>
#include <format>
#include <sstream>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#include "OnlineDownloader.hpp"
#include "AppData.hpp"
#include "Logger.hpp"
#include "Utils.hpp"
#include "TaskQueue.hpp"

nlohmann::json HttpResponse::GetBodyAsJson()
{
    return nlohmann::json::parse(Body, nullptr, false);
}

bool HttpResponse::DidSucceed()
{
    return bFailed == false && StatusCode == 200;
}

std::string Transcoding::Extension() const
{
    return MimeType.rfind("audio/mp4", 0) == 0 ? ".m4a" : ".mp3";
}

nlohmann::json TrackBlob::ToJson() const
{
    return {
        { "id", Id },
        { "title", Title },
        { "artist", Artist },
        { "thumbnailUrl", ArtworkURL },
        { "likeCount", Likes },
        { "repostCount", Reposts },
        { "playbackCount", Playbacks },
        { "durationMs", Duration },
        { "pageUrl", PermalinkURL },
        { "streamUrl", HighestTranscodingURL },
        { "format", Transcodings.empty() ? std::string() : Transcodings.front().Extension().substr(1) },
    };
}

void OnlineDownloader::Init()
{
    curl_global_init(CURL_GLOBAL_DEFAULT);
    std::error_code ec;
    std::filesystem::remove_all(PreviewDirectory(), ec);
}

std::filesystem::path OnlineDownloader::PreviewDirectory()
{
    std::error_code ec;
    const std::filesystem::path temp = std::filesystem::temp_directory_path(ec);
    return (ec ? AppData::Directory() : temp) / L"WavePreview";
}

size_t OnlineDownloader::WriteCallback(char* Ptr, size_t Size, size_t Count, void* User)
{
    auto* out = static_cast<std::string*>(User);
    out->append(Ptr, Size * Count);
    return Size * Count;
}

int OnlineDownloader::ProgressCallbackThunk(void* User, curl_off_t Total, curl_off_t Now, curl_off_t, curl_off_t)
{
    auto* transfer = static_cast<Transfer*>(User);
    if (!transfer->OnProgress || Total <= 0)
        return 0;
    const double progress = static_cast<double>(Now) / static_cast<double>(Total);
    if (progress - transfer->LastReported >= 0.01 || (progress >= 1.0 && transfer->LastReported < 1.0)) {
        transfer->LastReported = progress;
        transfer->OnProgress(progress);
    }
    return 0;
}

HttpResponse OnlineDownloader::HttpGet(std::string url, long TimeoutSeconds, const ProgressCallback& OnProgress)
{
    HttpResponse res = {};

    CURL* curl = curl_easy_init();
    if (!curl) {
        res.bFailed = true;
        res.Error = "couldn't start a web request";
        return res;
    }

    Transfer transfer = { &res.Body, OnProgress, -1.0 };

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);   // follow redirects
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, TimeoutSeconds);         // total timeout, seconds
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &res.Body);

    if (OnProgress) {
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, ProgressCallbackThunk);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &transfer);
    }

    // Needed for many HTTPS servers / to avoid being blocked
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla/5.0 (Linux; Android 7.1.1; ONEPLUS A3010) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/86.0.4240.185 Mobile Safari/537.36");

    CURLcode code = curl_easy_perform(curl);
    if (code != CURLE_OK) {
        res.bFailed = true;
        res.Error = curl_easy_strerror(code);
    }
    else {
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        res.StatusCode = static_cast<int32_t>(status);
    }

    curl_easy_cleanup(curl);
    return res;
}

std::string OnlineDownloader::UrlEncode(std::string Base) {
    CURL* curl = curl_easy_init();
    if (!curl) return Base;   // fallback: return unencoded

    char* encoded = curl_easy_escape(curl, Base.c_str(),
        static_cast<int>(Base.length()));
    std::string result = encoded ? encoded : Base;

    if (encoded) curl_free(encoded);
    curl_easy_cleanup(curl);
    return result;
}

std::string OnlineDownloader::SanitizeFileName(std::string Str, char Replacement)
{
    static const std::string invalid = "<>:\"/\\|?*";

    for (char& c : Str) {
        if (invalid.find(c) != std::string::npos ||
            static_cast<unsigned char>(c) < 32) {
            c = Replacement;
        }
    }

    while (!Str.empty() && (Str.back() == ' ' || Str.back() == '.')) {
        Str.pop_back();
    }

    size_t start = Str.find_first_not_of(' ');
    Str = (start == std::string::npos) ? "" : Str.substr(start);

    static const std::string reserved[] = {
        "CON", "PRN", "AUX", "NUL",
        "COM1", "COM2", "COM3", "COM4", "COM5",
        "COM6", "COM7", "COM8", "COM9",
        "LPT1", "LPT2", "LPT3", "LPT4", "LPT5",
        "LPT6", "LPT7", "LPT8", "LPT9"
    };

    std::string stem = Str.substr(0, Str.find('.'));
    std::string upper = stem;
    std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);

    for (const auto& r : reserved) {
        if (upper == r) {
            Str = "_" + Str;
            break;
        }
    }

    if (Str.empty()) Str = "unnamed";

    return Str;
}

std::string OnlineDownloader::JsonString(const nlohmann::json& Object, const char* Key)
{
    if (!Object.is_object())
        return {};
    const auto it = Object.find(Key);
    return it != Object.end() && it->is_string() ? it->get<std::string>() : std::string();
}

int32_t OnlineDownloader::JsonInt(const nlohmann::json& Object, const char* Key)
{
    if (!Object.is_object())
        return 0;
    const auto it = Object.find(Key);
    return it != Object.end() && it->is_number() ? static_cast<int32_t>(it->get<double>()) : 0;
}

int OnlineDownloader::TranscodingRank(const nlohmann::json& Format, const std::string& Preset)
{
    const std::string protocol = JsonString(Format, "protocol");
    const std::string mime = JsonString(Format, "mime_type");
    const bool mp3 = mime.rfind("audio/mpeg", 0) == 0;
    const bool aac = mime.rfind("audio/mp4", 0) == 0;
    if (protocol == "hls" && aac && Preset.find("160") != std::string::npos)
        return 5;
    if (protocol == "progressive" && mp3)
        return 4;
    if (protocol == "hls" && mp3)
        return 3;
    if (protocol == "hls" && aac)
        return 2;
    return 0;
}

std::optional<TrackBlob> OnlineDownloader::ParseTrack(const nlohmann::json& Track)
{
    if (!Track.is_object() || JsonString(Track, "kind") != "track")
        return std::nullopt;
    const auto id = Track.find("id");
    if (id == Track.end() || !id->is_number_integer())
        return std::nullopt;

    const auto media = Track.find("media");
    if (media == Track.end() || !media->is_object())
        return std::nullopt;
    const auto transcodings = media->find("transcodings");
    if (transcodings == media->end() || !transcodings->is_array())
        return std::nullopt;

    TrackBlob blob = {};
    for (const auto& item : *transcodings) {
        if (!item.is_object())
            continue;
        const std::string url = JsonString(item, "url");
        const auto snipped = item.find("snipped");
        if (url.empty() || (snipped != item.end() && snipped->is_boolean() && snipped->get<bool>()))
            continue;
        const auto format = item.find("format");
        if (format == item.end())
            continue;
        const int rank = TranscodingRank(*format, JsonString(item, "preset"));
        if (rank > 0)
            blob.Transcodings.push_back({ url, JsonString(*format, "protocol"), JsonString(*format, "mime_type"), rank });
    }
    if (blob.Transcodings.empty())
        return std::nullopt;
    std::stable_sort(blob.Transcodings.begin(), blob.Transcodings.end(), [](const Transcoding& A, const Transcoding& B) { return A.Rank > B.Rank; });

    const nlohmann::json empty = nlohmann::json::object();
    const auto user = Track.find("user");
    const nlohmann::json& userJson = user != Track.end() ? *user : empty;
    const auto publisher = Track.find("publisher_metadata");
    const nlohmann::json& publisherJson = publisher != Track.end() ? *publisher : empty;

    blob.Id = std::to_string(id->get<int64_t>());
    blob.Title = JsonString(Track, "title");
    blob.ArtworkURL = JsonString(Track, "artwork_url");
    if (blob.ArtworkURL.empty())
        blob.ArtworkURL = JsonString(userJson, "avatar_url");
    blob.Artist = JsonString(publisherJson, "artist");
    if (blob.Artist.empty())
        blob.Artist = JsonString(userJson, "username");
    blob.Likes = JsonInt(Track, "likes_count");
    blob.Playbacks = JsonInt(Track, "playback_count");
    blob.Reposts = JsonInt(Track, "reposts_count");
    blob.Duration = JsonInt(Track, "full_duration");
    if (blob.Duration <= 0)
        blob.Duration = JsonInt(Track, "duration");
    blob.PermalinkURL = JsonString(Track, "permalink_url");
    blob.TrackAuthorization = JsonString(Track, "track_authorization");
    blob.HighestTranscodingURL = blob.Transcodings.front().Url;
    SplitArtist(blob);
    return blob;
}

void OnlineDownloader::SplitArtist(TrackBlob& Blob)
{
    auto trim = [](const std::string& s) {
        const size_t first = s.find_first_not_of(" \t");
        if (first == std::string::npos)
            return std::string();
        return s.substr(first, s.find_last_not_of(" \t") - first + 1);
    };

    const size_t split = Blob.Title.find(" - ");
    if (split == std::string::npos || split == 0)
        return;
    const std::string artist = trim(Blob.Title.substr(0, split));
    const std::string title = trim(Blob.Title.substr(split + 3));
    if (artist.empty() || title.empty())
        return;
    Blob.Artist = artist;
    Blob.Title = title;
}

std::vector<TrackBlob> OnlineDownloader::Search(std::string Query, std::string& Error)
{
    std::vector<TrackBlob> ret;

    const size_t start = Query.find_first_not_of(" \t");
    if (start == std::string::npos)
        return ret;
    Query = Query.substr(start);

    HttpResponse res = HttpGet(std::format("{}/search/tracks?q={}&client_id={}&limit=30", BASE_API, UrlEncode(Query), CLIENT_ID));
    if (!res.DidSucceed()) {
        Error = res.bFailed ? "Couldn't reach SoundCloud: " + res.Error : "SoundCloud refused the search (status " + std::to_string(res.StatusCode) + ")";
        Logger::Error("[OnlineDownloader] Got invalid response from SoundCloud api /search. Response code: " + std::to_string(res.StatusCode) + " " + res.Error);
        return ret;
    }

    nlohmann::json body = res.GetBodyAsJson();
    if (!body.is_object() || !body.contains("collection") || !body["collection"].is_array()) {
        Error = "SoundCloud sent an unexpected reply";
        Logger::Error("[OnlineDownloader] /search reply has no collection");
        return ret;
    }

    std::lock_guard lock(Mutex);
    for (const auto& value : body["collection"]) {
        std::optional<TrackBlob> blob = ParseTrack(value);
        if (!blob)
            continue;
        Known[blob->Id] = *blob;
        ret.push_back(std::move(*blob));
    }
    Logger::Info("[OnlineDownloader] Search \"" + Query + "\" found " + std::to_string(ret.size()) + " downloadable tracks");
    return ret;
}

std::optional<TrackBlob> OnlineDownloader::Find(const std::string& TrackId)
{
    std::lock_guard lock(Mutex);
    const auto it = Known.find(TrackId);
    if (it == Known.end())
        return std::nullopt;
    return it->second;
}

std::string OnlineDownloader::GetMediaLink(TrackBlob Blob, const Transcoding& Stream)
{
    const char separator = Stream.Url.find('?') == std::string::npos ? '?' : '&';
    std::string url = std::format("{}{}client_id={}", Stream.Url, separator, CLIENT_ID);
    if (!Blob.TrackAuthorization.empty())
        url += "&track_authorization=" + UrlEncode(Blob.TrackAuthorization);

    HttpResponse res = HttpGet(url);
    if (res.DidSucceed()) {
        nlohmann::json resJson = res.GetBodyAsJson();
        if (resJson.is_object() && resJson.contains("url") && resJson["url"].is_string()) {
            return resJson["url"].get<std::string>();
        }
        else {
            Logger::Error("[OnlineDownloader] Got invalid response from SoundCloud api /stream/hls missing url. Response code: " + std::to_string(res.StatusCode));
        }
    }
    else {
        Logger::Error("[OnlineDownloader] Got invalid response from SoundCloud api /steam/hls. Response code: " + std::to_string(res.StatusCode) + " " + res.Error);
    }

    return "";
}

std::string OnlineDownloader::ResolveUrl(const std::string& Base, const std::string& Relative)
{
    if (Relative.rfind("http://", 0) == 0 || Relative.rfind("https://", 0) == 0)
        return Relative;
    const size_t scheme = Base.find("://");
    if (Relative.rfind("/", 0) == 0) {
        const size_t host = scheme == std::string::npos ? std::string::npos : Base.find('/', scheme + 3);
        return (host == std::string::npos ? Base : Base.substr(0, host)) + Relative;
    }
    const std::string path = Base.substr(0, Base.find('?'));
    return path.substr(0, path.rfind('/') + 1) + Relative;
}

std::optional<std::string> OnlineDownloader::FetchHls(const std::string& PlaylistUrl, const ProgressCallback& OnProgress, std::string& Error)
{
    HttpResponse playlist = HttpGet(PlaylistUrl);
    if (!playlist.DidSucceed()) {
        Error = "Couldn't load the track's stream";
        Logger::Error("[OnlineDownloader] HLS playlist failed. Response code: " + std::to_string(playlist.StatusCode) + " " + playlist.Error);
        return std::nullopt;
    }

    std::string init;
    std::vector<std::string> segments;
    std::istringstream lines(playlist.Body);
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty())
            continue;
        if (line.rfind("#EXT-X-KEY:", 0) == 0 && line.find("METHOD=NONE") == std::string::npos) {
            Error = "This track's stream is encrypted and can't be downloaded";
            return std::nullopt;
        }
        if (line.rfind("#EXT-X-MAP:", 0) == 0) {
            size_t at = line.find("URI=\"");
            if (at != std::string::npos) {
                at += 5;
                const size_t end = line.find('"', at);
                init = ResolveUrl(PlaylistUrl, line.substr(at, end == std::string::npos ? std::string::npos : end - at));
            }
            continue;
        }
        if (line[0] == '#')
            continue;
        segments.push_back(ResolveUrl(PlaylistUrl, line));
    }

    if (segments.empty()) {
        Error = "The track's stream had no audio";
        return std::nullopt;
    }

    std::string out;
    if (!init.empty()) {
        HttpResponse head = HttpGet(init, 60);
        if (!head.DidSucceed()) {
            Error = "Couldn't download the start of the track";
            return std::nullopt;
        }
        out += head.Body;
    }

    std::vector<std::string> parts(segments.size());
    std::atomic<bool> failed = false;
    std::mutex lock;
    size_t finished = 0;
    TaskQueue::ParallelFor(segments.size(), [&](size_t i) {
        if (failed)
            return;
        HttpResponse segment = HttpGet(segments[i], 60);
        std::lock_guard guard(lock);
        if (!segment.DidSucceed()) {
            if (!failed.exchange(true)) {
                Error = "The download stopped partway through (" + (segment.bFailed ? segment.Error : "status " + std::to_string(segment.StatusCode)) + ")";
                Logger::Error("[OnlineDownloader] HLS segment " + std::to_string(i) + " failed: " + Error);
            }
            return;
        }
        parts[i] = std::move(segment.Body);
        finished++;
        if (OnProgress)
            OnProgress(static_cast<double>(finished) / static_cast<double>(segments.size()));
    }, 6);
    if (failed)
        return std::nullopt;

    for (const std::string& part : parts)
        out += part;
    return out;
}

std::string OnlineDownloader::SyncSafe(uint32_t Value)
{
    std::string out(4, '\0');
    out[0] = static_cast<char>((Value >> 21) & 0x7F);
    out[1] = static_cast<char>((Value >> 14) & 0x7F);
    out[2] = static_cast<char>((Value >> 7) & 0x7F);
    out[3] = static_cast<char>(Value & 0x7F);
    return out;
}

std::string OnlineDownloader::Id3Frame(const std::string& Id, const std::string& Body)
{
    return Id + SyncSafe(static_cast<uint32_t>(Body.size())) + std::string(2, '\0') + Body;
}

std::string OnlineDownloader::FetchArtwork(const TrackBlob& Blob)
{
    if (Blob.ArtworkURL.empty())
        return {};
    std::string url = Blob.ArtworkURL;
    const size_t large = url.rfind("-large.");
    if (large != std::string::npos)
        url.replace(large, 6, "-t500x500");
    HttpResponse art = HttpGet(url);
    return art.DidSucceed() ? art.Body : std::string();
}

std::string OnlineDownloader::BuildId3(const TrackBlob& Blob, const std::string& Artwork)
{
    const char utf8 = '\x03';
    std::string frames;
    if (!Blob.Title.empty())
        frames += Id3Frame("TIT2", utf8 + Blob.Title);
    if (!Blob.Artist.empty())
        frames += Id3Frame("TPE1", utf8 + Blob.Artist);
    if (!Blob.PermalinkURL.empty())
        frames += Id3Frame("WOAS", Blob.PermalinkURL);

    if (!Artwork.empty()) {
        const std::string mime = Artwork.rfind("\x89PNG", 0) == 0 ? "image/png" : "image/jpeg";
        std::string body;
        body += utf8;
        body += mime;
        body += '\0';
        body += '\x03';
        body += '\0';
        body += Artwork;
        frames += Id3Frame("APIC", body);
    }

    if (frames.empty())
        return {};
    std::string header = "ID3";
    header += '\x04';
    header += '\0';
    header += '\0';
    return header + SyncSafe(static_cast<uint32_t>(frames.size())) + frames;
}

bool OnlineDownloader::Remux(const std::filesystem::path& Input, const std::filesystem::path& Output, const TrackBlob& Blob, const std::string& Artwork)
{
    const std::string inPath = Utils::ToUtf8(Input);
    const std::string outPath = Utils::ToUtf8(Output);

    AVFormatContext* in = nullptr;
    if (avformat_open_input(&in, inPath.c_str(), nullptr, nullptr) < 0)
        return false;

    AVFormatContext* out = nullptr;
    AVPacket* packet = av_packet_alloc();
    AVStream* source = nullptr;
    AVStream* sound = nullptr;
    AVStream* cover = nullptr;
    bool opened = false;

    const int audio = avformat_find_stream_info(in, nullptr) >= 0 ? av_find_best_stream(in, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0) : -1;
    bool ok = audio >= 0 && packet && avformat_alloc_output_context2(&out, nullptr, "ipod", outPath.c_str()) >= 0 && out;

    if (ok) {
        source = in->streams[audio];
        sound = avformat_new_stream(out, nullptr);
        ok = sound && avcodec_parameters_copy(sound->codecpar, source->codecpar) >= 0;
    }
    if (ok) {
        sound->codecpar->codec_tag = 0;
        sound->time_base = source->time_base;
        if (!Artwork.empty()) {
            cover = avformat_new_stream(out, nullptr);
            ok = cover != nullptr;
            if (ok) {
                cover->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
                cover->codecpar->codec_id = Artwork.rfind("\x89PNG", 0) == 0 ? AV_CODEC_ID_PNG : AV_CODEC_ID_MJPEG;
                cover->disposition = AV_DISPOSITION_ATTACHED_PIC;
                cover->time_base = { 1, 90000 };
            }
        }
    }
    if (ok) {
        av_dict_set(&out->metadata, "title", Blob.Title.c_str(), 0);
        av_dict_set(&out->metadata, "artist", Blob.Artist.c_str(), 0);
        if (!Blob.PermalinkURL.empty())
            av_dict_set(&out->metadata, "comment", Blob.PermalinkURL.c_str(), 0);
        opened = avio_open(&out->pb, outPath.c_str(), AVIO_FLAG_WRITE) >= 0;
        ok = opened && avformat_write_header(out, nullptr) >= 0;
    }
    if (ok && cover) {
        ok = av_new_packet(packet, static_cast<int>(Artwork.size())) >= 0;
        if (ok) {
            std::memcpy(packet->data, Artwork.data(), Artwork.size());
            packet->stream_index = cover->index;
            packet->flags |= AV_PKT_FLAG_KEY;
            packet->pts = 0;
            packet->dts = 0;
            ok = av_write_frame(out, packet) >= 0;
        }
        av_packet_unref(packet);
    }
    while (ok && av_read_frame(in, packet) >= 0) {
        if (packet->stream_index == audio) {
            av_packet_rescale_ts(packet, source->time_base, sound->time_base);
            packet->stream_index = sound->index;
            packet->pos = -1;
            ok = av_write_frame(out, packet) >= 0;
        }
        av_packet_unref(packet);
    }
    if (ok)
        ok = av_write_trailer(out) >= 0;

    if (opened)
        avio_closep(&out->pb);
    av_packet_free(&packet);
    if (out)
        avformat_free_context(out);
    avformat_close_input(&in);
    return ok;
}

std::optional<std::string> OnlineDownloader::FetchAudio(const TrackBlob& Blob, const Transcoding& Stream, const ProgressCallback& OnProgress, std::string& Error)
{
    const std::string link = GetMediaLink(Blob, Stream);
    if (link.empty()) {
        Error = "SoundCloud didn't give a stream for this track";
        return std::nullopt;
    }
    if (Stream.Protocol != "progressive")
        return FetchHls(link, OnProgress, Error);

    HttpResponse res = HttpGet(link, 0, OnProgress);
    if (!res.DidSucceed()) {
        Error = res.bFailed ? "The download failed: " + res.Error : "SoundCloud refused the download (status " + std::to_string(res.StatusCode) + ")";
        return std::nullopt;
    }
    return std::move(res.Body);
}

std::optional<std::string> OnlineDownloader::FetchBest(const TrackBlob& Blob, const ProgressCallback& OnProgress, std::string& Error, std::string& Extension)
{
    for (const Transcoding& stream : Blob.Transcodings) {
        Error.clear();
        if (OnProgress)
            OnProgress(0.0);
        std::optional<std::string> audio = FetchAudio(Blob, stream, OnProgress, Error);
        if (audio && !audio->empty()) {
            Error.clear();
            Extension = stream.Extension();
            return audio;
        }
        Logger::Warning("[OnlineDownloader] " + stream.MimeType + " stream failed for " + Blob.Id + ": " + Error);
    }
    if (Error.empty())
        Error = "SoundCloud sent an empty file";
    return std::nullopt;
}

std::optional<std::filesystem::path> OnlineDownloader::Preview(const std::string& TrackId, std::string& Error)
{
    std::optional<TrackBlob> blob = Find(TrackId);
    if (!blob) {
        Error = "Search for this track again, then retry";
        return std::nullopt;
    }

    if (std::optional<std::filesystem::path> cached = CachedPreview(TrackId))
        return cached;

    std::string extension;
    const std::optional<std::string> audio = FetchBest(*blob, {}, Error, extension);
    if (!audio)
        return std::nullopt;

    std::error_code ec;
    const std::filesystem::path dir = PreviewDirectory();
    std::filesystem::create_directories(dir, ec);
    const std::filesystem::path target = dir / Utils::FromUtf8(TrackId + extension);
    if (extension == ".m4a") {
        std::filesystem::path raw = target;
        raw += L".part";
        if (!AppData::WriteFile(raw, *audio)) {
            Error = "Couldn't cache the preview";
            return std::nullopt;
        }
        if (Remux(raw, target, *blob, "")) {
            std::filesystem::remove(raw, ec);
        }
        else {
            std::filesystem::remove(target, ec);
            std::filesystem::rename(raw, target, ec);
        }
    }
    else if (!AppData::WriteFile(target, *audio)) {
        Error = "Couldn't cache the preview";
        return std::nullopt;
    }
    return target;
}

std::optional<std::filesystem::path> OnlineDownloader::CachedPreview(const std::string& TrackId)
{
    std::error_code ec;
    const std::filesystem::path dir = PreviewDirectory();
    for (const char* ext : { ".m4a", ".mp3" }) {
        const std::filesystem::path cached = dir / Utils::FromUtf8(TrackId + ext);
        if (std::filesystem::is_regular_file(cached, ec))
            return cached;
    }
    return std::nullopt;
}

std::filesystem::path OnlineDownloader::DownloadsDirectory()
{
    return AppData::Directory() / L"Downloads";
}

std::optional<std::filesystem::path> OnlineDownloader::Download(const std::string& TrackId, const std::filesystem::path& Destination, const ProgressCallback& OnProgress, std::string& Error)
{
    std::optional<TrackBlob> blob = Find(TrackId);
    if (!blob) {
        Error = "Search for this track again, then retry";
        return std::nullopt;
    }

    std::string extension;
    std::optional<std::string> audio;
    std::filesystem::path source;
    if (std::optional<std::filesystem::path> cached = CachedPreview(TrackId)) {
        extension = Utils::ToUtf8(cached->extension());
        if (extension == ".m4a")
            source = *cached;
        else
            audio = AppData::ReadFile(*cached);
        if (source.empty() && (!audio || audio->empty()))
            audio.reset();
        else {
            Logger::Info("[OnlineDownloader] Reusing the cached preview of " + TrackId);
            if (OnProgress)
                OnProgress(1.0);
        }
    }
    if (source.empty() && !audio) {
        audio = FetchBest(*blob, OnProgress, Error, extension);
        if (!audio)
            return std::nullopt;
    }

    const std::string artwork = FetchArtwork(*blob);

    std::string name = blob->Artist.empty() ? blob->Title : blob->Artist + " - " + blob->Title;
    if (name.size() > 150) {
        size_t cut = 150;
        while (cut > 0 && (static_cast<unsigned char>(name[cut]) & 0xC0) == 0x80)
            cut--;
        name.resize(cut);
    }

    std::error_code ec;
    std::filesystem::create_directories(Destination, ec);
    const std::filesystem::path target = Destination / Utils::FromUtf8(SanitizeFileName(name) + extension);

    if (extension == ".m4a") {
        std::filesystem::path raw = source;
        if (raw.empty()) {
            raw = target;
            raw += L".part";
            if (!AppData::WriteFile(raw, *audio)) {
                Error = "Couldn't save the file to " + Utils::ToUtf8(Destination);
                return std::nullopt;
            }
        }
        const bool remuxed = Remux(raw, target, *blob, artwork) || Remux(raw, target, *blob, "");
        if (remuxed) {
            if (source.empty())
                std::filesystem::remove(raw, ec);
        }
        else {
            Logger::Warning("[OnlineDownloader] Couldn't remux " + Utils::ToUtf8(target.filename()) + ", keeping the raw stream");
            std::filesystem::remove(target, ec);
            if (source.empty())
                std::filesystem::rename(raw, target, ec);
            else
                std::filesystem::copy_file(source, target, std::filesystem::copy_options::overwrite_existing, ec);
            if (ec) {
                Error = "Couldn't save the file to " + Utils::ToUtf8(Destination);
                return std::nullopt;
            }
        }
    }
    else if (!AppData::WriteFile(target, BuildId3(*blob, artwork) + *audio)) {
        Error = "Couldn't save the file to " + Utils::ToUtf8(Destination);
        return std::nullopt;
    }

    Logger::Success("[OnlineDownloader] Saved " + Utils::ToUtf8(target.filename()));
    return target;
}
