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
#include <libswresample/swresample.h>
}

#include "AudioLoader.hpp"

std::string AudioLoader::ToUtf8(const std::filesystem::path& Path)
{
	const std::wstring wide = Path.wstring();
	const int size = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
	std::string out(size > 0 ? size : 0, '\0');
	if (size > 0)
		WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), out.data(), size, nullptr, nullptr);
	return out;
}

std::optional<std::vector<float>> AudioLoader::Load(const std::filesystem::path& Path, const std::string& Filters, std::string& Error, const std::atomic<bool>* Cancel)
{
	AVFormatContext* format = nullptr;
	if (avformat_open_input(&format, ToUtf8(Path).c_str(), nullptr, nullptr) < 0) {
		Error = "couldn't open the file";
		return std::nullopt;
	}
	AVCodecContext* decoder = nullptr;
	AVFilterGraph* graph = nullptr;
	AVPacket* packet = av_packet_alloc();
	AVFrame* frame = av_frame_alloc();
	AVFrame* filtered = av_frame_alloc();
	const auto cleanup = [&] {
		av_frame_free(&filtered);
		av_frame_free(&frame);
		av_packet_free(&packet);
		avfilter_graph_free(&graph);
		avcodec_free_context(&decoder);
		avformat_close_input(&format);
	};
	const auto fail = [&](const char* Message) -> std::optional<std::vector<float>> {
		Error = Message;
		cleanup();
		return std::nullopt;
	};

	const AVCodec* codec = nullptr;
	const int stream = avformat_find_stream_info(format, nullptr) >= 0 ? av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0) : -1;
	if (stream < 0 || !codec || !packet || !frame || !filtered)
		return fail("no audio stream");
	decoder = avcodec_alloc_context3(codec);
	if (!decoder || avcodec_parameters_to_context(decoder, format->streams[stream]->codecpar) < 0 || avcodec_open2(decoder, codec, nullptr) < 0)
		return fail("couldn't open the decoder");

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
	bool bReady = graph && outputs && inputs &&
		avfilter_graph_create_filter(&source, avfilter_get_by_name("abuffer"), "in", sourceArgs.c_str(), nullptr, graph) >= 0 &&
		avfilter_graph_create_filter(&sink, avfilter_get_by_name("abuffersink"), "out", nullptr, nullptr, graph) >= 0;
	if (bReady) {
		outputs->name = av_strdup("in");
		outputs->filter_ctx = source;
		inputs->name = av_strdup("out");
		inputs->filter_ctx = sink;
		bReady = avfilter_graph_parse_ptr(graph, Filters.c_str(), &inputs, &outputs, nullptr) >= 0 && avfilter_graph_config(graph, nullptr) >= 0;
	}
	avfilter_inout_free(&inputs);
	avfilter_inout_free(&outputs);
	if (!bReady)
		return fail("couldn't set up the audio filters");

	std::vector<float> samples;
	const auto drain = [&] {
		while (av_buffersink_get_frame(sink, filtered) >= 0) {
			const float* data = reinterpret_cast<const float*>(filtered->data[0]);
			samples.insert(samples.end(), data, data + static_cast<size_t>(filtered->nb_samples) * filtered->ch_layout.nb_channels);
			av_frame_unref(filtered);
		}
	};
	const auto feed = [&] {
		while (avcodec_receive_frame(decoder, frame) >= 0) {
			if (av_buffersrc_add_frame(source, frame) < 0)
				av_frame_unref(frame);
			drain();
		}
	};
	while (av_read_frame(format, packet) >= 0) {
		if (packet->stream_index == stream && avcodec_send_packet(decoder, packet) >= 0)
			feed();
		av_packet_unref(packet);
		if (Cancel && Cancel->load())
			return fail("cancelled");
	}
	avcodec_send_packet(decoder, nullptr);
	feed();
	av_buffersrc_add_frame(source, nullptr);
	drain();
	cleanup();
	if (samples.empty()) {
		Error = "the file has no audio";
		return std::nullopt;
	}
	return samples;
}

std::vector<float> AudioLoader::Resample(const std::vector<float>& Input, int InputRate, int InputChannels, int OutputRate, int OutputChannels)
{
	AVChannelLayout inLayout{};
	AVChannelLayout outLayout{};
	av_channel_layout_default(&inLayout, InputChannels);
	av_channel_layout_default(&outLayout, OutputChannels);
	SwrContext* swr = nullptr;
	std::vector<float> output;
	if (swr_alloc_set_opts2(&swr, &outLayout, AV_SAMPLE_FMT_FLT, OutputRate, &inLayout, AV_SAMPLE_FMT_FLT, InputRate, 0, nullptr) < 0 || swr_init(swr) < 0) {
		swr_free(&swr);
		return output;
	}
	const int inFrames = static_cast<int>(Input.size() / InputChannels);
	const int capacity = swr_get_out_samples(swr, inFrames) + 64;
	output.resize(static_cast<size_t>(capacity) * OutputChannels);
	uint8_t* outPtr = reinterpret_cast<uint8_t*>(output.data());
	const uint8_t* inPtr = reinterpret_cast<const uint8_t*>(Input.data());
	int produced = swr_convert(swr, &outPtr, capacity, &inPtr, inFrames);
	if (produced < 0)
		produced = 0;
	uint8_t* tailPtr = reinterpret_cast<uint8_t*>(output.data() + static_cast<size_t>(produced) * OutputChannels);
	const int tail = swr_convert(swr, &tailPtr, capacity - produced, nullptr, 0);
	if (tail > 0)
		produced += tail;
	output.resize(static_cast<size_t>(produced) * OutputChannels);
	swr_free(&swr);
	av_channel_layout_uninit(&inLayout);
	av_channel_layout_uninit(&outLayout);
	return output;
}
