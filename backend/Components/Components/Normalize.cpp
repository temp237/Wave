#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
}

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "Normalize.hpp"
#include "AppData.hpp"
#include "Logger.hpp"
#include "Settings.hpp"
#include "Utils.hpp"

void Normalize::Processor::Process(float* Data, size_t Frames, uint32_t Channels, uint32_t SampleRate)
{
	if (!Data || Channels == 0 || SampleRate == 0)
		return;
	if (SampleRate != Rate) {
		Rate = SampleRate;
		Coefficient = static_cast<float>(1.0 - std::exp(-1.0 / (SampleRate * 0.15)));
	}
	const float target = Normalize::Target.load();
	const uint32_t track = Normalize::TrackChange.load();
	if (track != SeenTrack) {
		SeenTrack = track;
		Gain = target;
	}
	if (Gain == 1.f && target == 1.f)
		return;
	for (size_t f = 0; f < Frames; f++) {
		Gain += Coefficient * (target - Gain);
		float* frame = Data + f * Channels;
		for (uint32_t c = 0; c < Channels; c++)
			frame[c] *= Gain;
	}
	if (std::fabs(Gain - target) < 1e-5f)
		Gain = target;
}

std::optional<Normalize::Loudness> Normalize::Analyze(const std::filesystem::path& Path)
{
	const std::string path = Utils::ToUtf8(Path);
	AVFormatContext* format = nullptr;
	if (avformat_open_input(&format, path.c_str(), nullptr, nullptr) < 0)
		return std::nullopt;

	AVCodecContext* decoder = nullptr;
	AVFilterGraph* graph = nullptr;
	AVPacket* packet = av_packet_alloc();
	AVFrame* frame = av_frame_alloc();
	AVFrame* measured = av_frame_alloc();
	std::optional<Loudness> result;

	const auto cleanup = [&] {
		av_frame_free(&measured);
		av_frame_free(&frame);
		av_packet_free(&packet);
		avfilter_graph_free(&graph);
		avcodec_free_context(&decoder);
		avformat_close_input(&format);
	};

	const AVCodec* codec = nullptr;
	const int stream = avformat_find_stream_info(format, nullptr) >= 0 ? av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0) : -1;
	if (stream < 0 || !codec || !packet || !frame || !measured) {
		cleanup();
		return std::nullopt;
	}
	decoder = avcodec_alloc_context3(codec);
	if (!decoder || avcodec_parameters_to_context(decoder, format->streams[stream]->codecpar) < 0 || avcodec_open2(decoder, codec, nullptr) < 0) {
		cleanup();
		return std::nullopt;
	}

	AVChannelLayout layout{};
	if (decoder->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC)
		av_channel_layout_default(&layout, decoder->ch_layout.nb_channels);
	else
		av_channel_layout_copy(&layout, &decoder->ch_layout);
	char layoutName[128] = {};
	av_channel_layout_describe(&layout, layoutName, sizeof(layoutName));
	av_channel_layout_uninit(&layout);

	const std::string sourceArgs = "sample_rate=" + std::to_string(decoder->sample_rate) +
		":sample_fmt=" + av_get_sample_fmt_name(decoder->sample_fmt) +
		":channel_layout=" + layoutName +
		":time_base=1/" + std::to_string(decoder->sample_rate);

	graph = avfilter_graph_alloc();
	AVFilterContext* source = nullptr;
	AVFilterContext* sink = nullptr;
	AVFilterInOut* outputs = avfilter_inout_alloc();
	AVFilterInOut* inputs = avfilter_inout_alloc();
	bool bGraphReady = graph && outputs && inputs &&
		avfilter_graph_create_filter(&source, avfilter_get_by_name("abuffer"), "in", sourceArgs.c_str(), nullptr, graph) >= 0 &&
		avfilter_graph_create_filter(&sink, avfilter_get_by_name("abuffersink"), "out", nullptr, nullptr, graph) >= 0;
	if (bGraphReady) {
		outputs->name = av_strdup("in");
		outputs->filter_ctx = source;
		inputs->name = av_strdup("out");
		inputs->filter_ctx = sink;
		bGraphReady = avfilter_graph_parse_ptr(graph, "ebur128=metadata=1:peak=true", &inputs, &outputs, nullptr) >= 0 &&
			avfilter_graph_config(graph, nullptr) >= 0;
	}
	avfilter_inout_free(&inputs);
	avfilter_inout_free(&outputs);
	if (!bGraphReady) {
		cleanup();
		return std::nullopt;
	}

	double integrated = NAN;
	double peak = 0.0;
	const auto drain = [&] {
		while (av_buffersink_get_frame(sink, measured) >= 0) {
			if (const AVDictionaryEntry* entry = av_dict_get(measured->metadata, "lavfi.r128.I", nullptr, 0))
				integrated = std::atof(entry->value);
			const AVDictionaryEntry* entry = nullptr;
			while ((entry = av_dict_get(measured->metadata, "lavfi.r128.true_peaks_ch", entry, AV_DICT_IGNORE_SUFFIX)))
				peak = std::max(peak, std::atof(entry->value));
			av_frame_unref(measured);
		}
	};
	const auto feed = [&] {
		while (avcodec_receive_frame(decoder, frame) >= 0) {
			if (av_buffersrc_add_frame(source, frame) < 0)
				av_frame_unref(frame);
			drain();
		}
	};

	while (bRunning && av_read_frame(format, packet) >= 0) {
		if (packet->stream_index == stream && avcodec_send_packet(decoder, packet) >= 0)
			feed();
		av_packet_unref(packet);
	}
	const bool bFinished = bRunning;
	if (bFinished) {
		avcodec_send_packet(decoder, nullptr);
		feed();
		av_buffersrc_add_frame(source, nullptr);
		drain();
	}
	cleanup();

	if (!bFinished)
		return std::nullopt;
	Loudness loudness;
	loudness.bValid = std::isfinite(integrated) && integrated > -60.0 && peak > 0.0;
	loudness.Integrated = std::isfinite(integrated) ? integrated : 0.0;
	loudness.Peak = peak > 0.0 ? peak : 1.0;
	return loudness;
}

double Normalize::GainDb(const Loudness& Value)
{
	if (!Value.bValid)
		return 0.0;
	double gain = std::min(TargetLufs - Value.Integrated, MaxBoostDb);
	if (gain > 0.0) {
		const double peakDb = 20.0 * std::log10(Value.Peak);
		gain = std::min(gain, std::max(0.0, -HeadroomDb - peakDb));
	}
	return gain;
}

void Normalize::ApplyCurrent()
{
	float target = 1.f;
	if (bEnabled) {
		if (auto it = Cache.find(CurrentId); it != Cache.end())
			target = static_cast<float>(std::pow(10.0, GainDb(it->second) / 20.0));
	}
	Target = target;
}

void Normalize::SetTrack(const std::string& Id, const std::filesystem::path& Path)
{
	std::lock_guard lock(Mutex);
	CurrentId = Id;
	if (bEnabled && !Cache.contains(Id)) {
		if (auto it = std::find_if(Pending.begin(), Pending.end(), [&](const auto& job) { return job.first == Id; }); it != Pending.end())
			Pending.erase(it);
		Pending.emplace_front(Id, Path);
		Queued.insert(Id);
		Wake.notify_one();
	}
	ApplyCurrent();
	TrackChange++;
}

void Normalize::Store(const std::string& Id, const Loudness& Value)
{
	std::lock_guard lock(Mutex);
	Cache[Id] = Value;
	Queued.erase(Id);
	if (Id == CurrentId)
		ApplyCurrent();
	if (++Unsaved >= 25 || Pending.empty())
		SaveCache();
}

void Normalize::Work()
{
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
	while (true) {
		std::pair<std::string, std::filesystem::path> job;
		{
			std::unique_lock lock(Mutex);
			Wake.wait(lock, [] { return !bRunning || !Pending.empty(); });
			if (!bRunning)
				return;
			job = std::move(Pending.front());
			Pending.pop_front();
			if (Cache.contains(job.first)) {
				Queued.erase(job.first);
				continue;
			}
		}
		const std::optional<Loudness> result = Analyze(job.second);
		if (!bRunning)
			return;
		Store(job.first, result.value_or(Loudness{}));
	}
}

void Normalize::QueueMissing()
{
	const auto files = AppData::AllFilePaths();
	std::lock_guard lock(Mutex);
	for (const auto& [id, path] : files) {
		if (!Cache.contains(id) && Queued.insert(id).second)
			Pending.emplace_back(id, path);
	}
	Wake.notify_all();
}

void Normalize::StartWorkers()
{
	if (bRunning.exchange(true))
		return;
	const unsigned count = std::clamp(std::thread::hardware_concurrency() / 4, 1u, 3u);
	for (unsigned i = 0; i < count; i++)
		Workers.emplace_back(Work);
	QueueMissing();
	Logger::Info("[Normalize] Measuring loudness with " + std::to_string(count) + " background thread(s)");
}

void Normalize::StopWorkers()
{
	bRunning = false;
	Wake.notify_all();
	for (auto& worker : Workers) {
		if (worker.joinable())
			worker.join();
	}
	Workers.clear();
	std::lock_guard lock(Mutex);
	Pending.clear();
	Queued.clear();
	SaveCache();
}

void Normalize::Load()
{
	av_log_set_level(AV_LOG_QUIET);
	bEnabled = Settings::Bool(Settings::Get("normalize"), "enabled", false);
	{
		std::lock_guard lock(Mutex);
		LoadCache();
		ApplyCurrent();
	}
	if (bEnabled)
		StartWorkers();
}

void Normalize::Shutdown()
{
	StopWorkers();
}

bool Normalize::IsEnabled()
{
	return bEnabled;
}

void Normalize::SetEnabled(bool bOn)
{
	bEnabled = bOn;
	Settings::Merge("normalize", { { "enabled", bOn } });
	{
		std::lock_guard lock(Mutex);
		ApplyCurrent();
	}
	if (bOn)
		StartWorkers();
	else
		StopWorkers();
	Logger::Info(bOn ? "[Normalize] Volume normalization on" : "[Normalize] Volume normalization off");
}

nlohmann::json Normalize::StateJson()
{
	const auto files = AppData::AllFilePaths();
	size_t measured = 0;
	{
		std::lock_guard lock(Mutex);
		for (const auto& file : files)
			measured += Cache.contains(file.first) ? 1 : 0;
	}
	if (bEnabled && measured < files.size())
		QueueMissing();
	return { { "enabled", IsEnabled() }, { "measured", measured }, { "total", files.size() }, { "target", TargetLufs } };
}

std::filesystem::path Normalize::CachePath()
{
	return AppData::Directory() / "loudness.json";
}

void Normalize::LoadCache()
{
	const nlohmann::json j = nlohmann::json::parse(AppData::ReadFile(CachePath()), nullptr, false);
	if (!j.is_object())
		return;
	for (const auto& [id, entry] : j.items()) {
		if (!entry.is_object())
			continue;
		Loudness value;
		value.bValid = entry.value("valid", false);
		value.Integrated = entry.value("lufs", 0.0);
		value.Peak = entry.value("peak", 1.0);
		Cache[id] = value;
	}
}

void Normalize::SaveCache()
{
	Unsaved = 0;
	nlohmann::json j = nlohmann::json::object();
	for (const auto& [id, value] : Cache)
		j[id] = { { "valid", value.bValid }, { "lufs", value.Integrated }, { "peak", value.Peak } };
	AppData::WriteFile(CachePath(), j.dump());
}
