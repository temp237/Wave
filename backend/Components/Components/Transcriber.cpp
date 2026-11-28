#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <whisper.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <limits>

#include "Transcriber.hpp"

Transcriber::~Transcriber()
{
	for (whisper_state* state : All)
		whisper_free_state(state);
	if (Context)
		whisper_free(Context);
}

bool Transcriber::Load(const std::filesystem::path& Model, std::string& Error)
{
	whisper_log_set([](ggml_log_level, const char*, void*) {}, nullptr);
	whisper_context_params params = whisper_context_default_params();
	params.use_gpu = true;
	params.flash_attn = false;
	params.dtw_token_timestamps = true;
	params.dtw_aheads_preset = Model.filename().string().find("turbo") != std::string::npos ? WHISPER_AHEADS_LARGE_V3_TURBO : WHISPER_AHEADS_LARGE_V3;
	Context = whisper_init_from_file_with_params(Model.string().c_str(), params);
	if (!Context) {
		Error = "couldn't load the Whisper model";
		return false;
	}
	return true;
}

whisper_state* Transcriber::Acquire()
{
	std::lock_guard lock(Lock);
	if (!Idle.empty()) {
		whisper_state* state = Idle.back();
		Idle.pop_back();
		return state;
	}
	whisper_state* state = whisper_init_state(Context);
	if (state)
		All.push_back(state);
	return state;
}

void Transcriber::Release(whisper_state* State)
{
	std::lock_guard lock(Lock);
	Idle.push_back(State);
}

std::string Transcriber::Trim(const std::string& Text)
{
	const size_t first = Text.find_first_not_of(" \t\r\n");
	if (first == std::string::npos)
		return {};
	return Text.substr(first, Text.find_last_not_of(" \t\r\n") - first + 1);
}

std::string Transcriber::Lower(const std::string& Text)
{
	std::string out;
	for (const char c : Text) {
		const unsigned char u = static_cast<unsigned char>(c);
		if (std::isalnum(u) || u >= 0x80 || c == '\'')
			out += static_cast<char>(std::tolower(u));
	}
	return out;
}

bool Transcriber::IsSymbols(const std::string& Text)
{
	static const std::array<const char*, 12> symbols = {
		"\xC2\xB6", "\xE2\x99\xAA", "\xE2\x99\xAB", "\xE2\x99\xAC", "\xC2\xA9", "\xC2\xAE",
		"\xE2\x84\xA2", "\xC2\xAB", "\xC2\xBB", "\xE2\x80\xA2", "\xE2\x80\xA6", "\xE2\x80\x94"
	};
	std::string rest = Text;
	for (const char* symbol : symbols) {
		for (size_t at = rest.find(symbol); at != std::string::npos; at = rest.find(symbol))
			rest.erase(at, std::char_traits<char>::length(symbol));
	}
	for (const char c : rest) {
		const unsigned char u = static_cast<unsigned char>(c);
		if (std::isalnum(u) || u >= 0x80)
			return false;
	}
	return true;
}

bool Transcriber::IsNoise(const std::string& Text)
{
	std::string lower;
	for (const char c : Text) {
		if (std::isalnum(static_cast<unsigned char>(c)) || (static_cast<unsigned char>(c) >= 0x80))
			lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		else if (c == ' ' && !lower.empty() && lower.back() != ' ')
			lower += ' ';
	}
	lower = Trim(lower);
	if (lower.empty() || Text.find("\xC2\xA9") != std::string::npos)
		return true;
	static const std::array<const char*, 4> exact = { "music", "instrumental", "outro", "intro" };
	for (const char* phrase : exact) {
		if (lower == phrase)
			return true;
	}
	static const std::array<const char*, 12> contained = {
		"thanks for watching", "thank you for watching", "subscribe", "subtitle", "transcript", "translated by",
		"amara org", "be right back", "www ", " com ", "copyright", "all rights reserved"
	};
	const std::string padded = " " + lower + " ";
	for (const char* phrase : contained) {
		if (padded.find(phrase) != std::string::npos)
			return true;
	}
	return false;
}

Transcriber::VocalMap Transcriber::FindActivity(const std::vector<float>& Mono)
{
	VocalMap map;
	const size_t frame = static_cast<size_t>(SampleRate * FrameSeconds);
	const size_t count = Mono.size() / frame;
	std::vector<float>& levels = map.Levels;
	levels.resize(count);
	for (size_t f = 0; f < count; f++) {
		double sum = 0.0;
		for (size_t i = 0; i < frame; i++) {
			const float s = Mono[f * frame + i];
			sum += s * s;
		}
		levels[f] = static_cast<float>(10.0 * std::log10(sum / frame + 1e-12));
	}
	std::vector<Activity>& runs = map.Runs;
	if (levels.empty())
		return map;
	std::vector<float> sorted = levels;
	const size_t top = sorted.size() * 98 / 100;
	std::nth_element(sorted.begin(), sorted.begin() + top, sorted.end());
	map.Reference = sorted[top];
	const double threshold = std::max(ActivityFloorDb, sorted[top] - ActivityRangeDb);

	for (size_t f = 0; f < count; f++) {
		if (levels[f] < threshold)
			continue;
		const double start = f * FrameSeconds;
		const double end = start + FrameSeconds;
		if (!runs.empty() && start - runs.back().End <= ActivityBridge)
			runs.back().End = end;
		else
			runs.push_back({ start, end });
	}
	return map;
}

double Transcriber::SingingFloor(const VocalMap& Map)
{
	return std::max(QuietSegmentDb, Map.Reference - QuietBelowReferenceDb);
}

void Transcriber::FindVocalBounds(const VocalMap& Map, double& Start, double& End)
{
	const double floor = SingingFloor(Map);
	double runStart = -1.0;
	double runEnd = -1.0;
	const auto close = [&] {
		if (runStart >= 0 && runEnd - runStart >= MinVocalRunSeconds) {
			if (Start < 0)
				Start = runStart;
			End = runEnd;
		}
	};
	for (size_t f = 0; f < Map.Levels.size(); f++) {
		if (Map.Levels[f] < floor)
			continue;
		const double start = f * FrameSeconds;
		if (runStart >= 0 && start - runEnd <= ActivityBridge) {
			runEnd = start + FrameSeconds;
			continue;
		}
		close();
		runStart = start;
		runEnd = start + FrameSeconds;
	}
	close();
}

void Transcriber::DropQuietSegments(std::vector<Word>& Words, const VocalMap& Map)
{
	const double floor = SingingFloor(Map);
	std::vector<Word> kept;
	kept.reserve(Words.size());
	size_t first = 0;
	for (size_t i = 0; i < Words.size(); i++) {
		if (!Words[i].bSegmentEnd && i + 1 < Words.size())
			continue;
		const size_t from = std::min(Map.Levels.size(), static_cast<size_t>(Words[first].Start / FrameSeconds));
		const size_t to = std::min(Map.Levels.size(), static_cast<size_t>(Words[i].End / FrameSeconds) + 1);
		float peak = -std::numeric_limits<float>::infinity();
		for (size_t f = from; f < to; f++)
			peak = std::max(peak, Map.Levels[f]);
		if (peak >= floor)
			kept.insert(kept.end(), Words.begin() + first, Words.begin() + i + 1);
		first = i + 1;
	}
	Words = std::move(kept);
}

std::vector<Transcriber::Chunk> Transcriber::PlanChunks(const VocalMap& Map, double Duration)
{
	std::vector<Activity> pieces;
	for (const Activity& run : Map.Runs) {
		double start = run.Start;
		while (run.End - start > ChunkSeconds - 2.0 * ChunkPad) {
			const size_t from = static_cast<size_t>((start + ChunkSeconds * 0.6) / FrameSeconds);
			const size_t to = std::min(Map.Levels.size(), static_cast<size_t>((start + ChunkSeconds - 2.0 * ChunkPad) / FrameSeconds));
			size_t quietest = to > from ? from : static_cast<size_t>((start + ChunkSeconds * 0.8) / FrameSeconds);
			for (size_t f = from; f < to; f++) {
				if (Map.Levels[f] < Map.Levels[quietest])
					quietest = f;
			}
			const double cut = quietest * FrameSeconds;
			pieces.push_back({ start, cut });
			start = cut;
		}
		pieces.push_back({ start, run.End });
	}

	std::vector<Chunk> chunks;
	for (size_t i = 0; i < pieces.size(); i++) {
		const Activity& piece = pieces[i];
		const double voiced = piece.End - piece.Start;
		if (!chunks.empty() && piece.End + ChunkPad - chunks.back().Start <= ChunkSeconds) {
			chunks.back().End = piece.End;
			chunks.back().Voiced += voiced;
			continue;
		}
		chunks.push_back({ piece.Start, piece.End, voiced });
	}
	for (size_t i = 0; i < chunks.size(); i++) {
		Chunk& chunk = chunks[i];
		const double floor = i > 0 ? chunks[i - 1].End : 0.0;
		const double ceiling = i + 1 < chunks.size() ? chunks[i + 1].Start : Duration;
		chunk.Start = std::max(floor, chunk.Start - ChunkPad);
		chunk.End = std::min(ceiling, chunk.End + ChunkPad);
		if (chunk.End - chunk.Start < MinChunkSeconds) {
			const double grow = (MinChunkSeconds - (chunk.End - chunk.Start)) / 2.0;
			chunk.Start = std::max(floor, chunk.Start - grow);
			chunk.End = std::min(ceiling, chunk.End + grow);
		}
	}
	return chunks;
}

bool Transcriber::LooksRepetitive(const std::vector<Word>& Words)
{
	if (Words.size() < 8)
		return false;
	std::vector<std::string> lowered;
	for (const Word& word : Words)
		lowered.push_back(Lower(word.Text));
	std::sort(lowered.begin(), lowered.end());
	size_t best = 1;
	size_t run = 1;
	for (size_t i = 1; i < lowered.size(); i++) {
		run = lowered[i] == lowered[i - 1] ? run + 1 : 1;
		best = std::max(best, run);
	}
	return best * 2 > lowered.size();
}

int Transcriber::DetectLanguage(whisper_state* State, const std::vector<float>& Mono, const std::vector<Chunk>& Chunks)
{
	const Chunk* loudest = nullptr;
	for (const Chunk& chunk : Chunks) {
		if (!loudest || chunk.Voiced > loudest->Voiced)
			loudest = &chunk;
	}
	if (!loudest)
		return -1;
	const size_t first = std::min(Mono.size(), static_cast<size_t>(loudest->Start * SampleRate));
	const size_t last = std::min(Mono.size(), static_cast<size_t>(loudest->End * SampleRate));
	if (last <= first || whisper_pcm_to_mel_with_state(Context, State, Mono.data() + first, static_cast<int>(last - first), Threads) != 0)
		return -1;
	std::vector<float> probs(whisper_lang_max_id() + 1, 0.f);
	return whisper_lang_auto_detect_with_state(Context, State, 0, Threads, probs.data());
}

bool Transcriber::DecodeChunk(whisper_state* State, const std::vector<float>& Mono, const Chunk& Piece, const char* Language, float Temperature, const std::atomic<bool>* Cancel, std::vector<Word>& Out)
{
	const size_t first = std::min(Mono.size(), static_cast<size_t>(Piece.Start * SampleRate));
	const size_t last = std::min(Mono.size(), static_cast<size_t>(Piece.End * SampleRate));
	if (last <= first)
		return true;

	whisper_full_params params = whisper_full_default_params(WHISPER_SAMPLING_BEAM_SEARCH);
	params.n_threads = Threads;
	params.language = Language;
	params.translate = false;
	params.no_context = true;
	params.token_timestamps = true;
	params.suppress_nst = true;
	params.print_progress = false;
	params.print_realtime = false;
	params.print_special = false;
	params.print_timestamps = false;
	params.beam_search.beam_size = 5;
	params.temperature = Temperature;
	if (Temperature > 0.f)
		params.temperature_inc = 0.f;
	if (Cancel) {
		params.abort_callback = [](void* Flag) { return static_cast<const std::atomic<bool>*>(Flag)->load(); };
		params.abort_callback_user_data = const_cast<std::atomic<bool>*>(Cancel);
	}
	if (whisper_full_with_state(Context, State, params, Mono.data() + first, static_cast<int>(last - first)) != 0)
		return false;

	const double offset = static_cast<double>(first) / SampleRate;
	const whisper_token eot = whisper_token_eot(Context);
	const int segments = whisper_full_n_segments_from_state(State);
	for (int s = 0; s < segments; s++) {
		if (whisper_full_get_segment_no_speech_prob_from_state(State, s) > NoSpeechLimit)
			continue;
		const double segmentStart = offset + whisper_full_get_segment_t0_from_state(State, s) / 100.0;
		const double segmentEnd = offset + whisper_full_get_segment_t1_from_state(State, s) / 100.0;
		const size_t firstWord = Out.size();
		const int tokens = whisper_full_n_tokens_from_state(State, s);
		for (int t = 0; t < tokens; t++) {
			const whisper_token_data data = whisper_full_get_token_data_from_state(State, s, t);
			if (data.id >= eot)
				continue;
			const char* text = whisper_full_get_token_text_from_state(Context, State, s, t);
			if (!text || !*text)
				continue;
			const double start = std::clamp(offset + (data.t_dtw >= 0 ? data.t_dtw : data.t0) / 100.0, segmentStart, segmentEnd);
			const double end = std::clamp(std::max(offset + data.t1 / 100.0, start), segmentStart, segmentEnd);
			if (text[0] == ' ' || Out.size() == firstWord)
				Out.push_back(Word{ start, end, text });
			else {
				Out.back().Text += text;
				Out.back().End = std::max(Out.back().End, end);
			}
		}
		if (Out.size() > firstWord)
			Out.back().bSegmentEnd = true;
	}
	return true;
}

bool Transcriber::DecodeAdaptive(whisper_state* State, const std::vector<float>& Mono, const VocalMap& Map, const Chunk& Piece, const char* Language, const std::atomic<bool>* Cancel, std::vector<Word>& Out)
{
	std::vector<Word> words;
	if (!DecodeChunk(State, Mono, Piece, Language, 0.f, Cancel, words))
		return false;
	const auto voicedIn = [&](double From, double To) {
		double voiced = 0.0;
		for (const Activity& run : Map.Runs)
			voiced += std::max(0.0, std::min(run.End, To) - std::max(run.Start, From));
		return voiced;
	};
	const bool bThin = Piece.Voiced >= ThinVoicedSeconds && words.size() < Piece.Voiced * ThinWordsPerSecond;
	const bool bRepetitive = LooksRepetitive(words);
	if (!bThin && !bRepetitive) {
		Out.insert(Out.end(), words.begin(), words.end());
		return true;
	}

	if (Piece.End - Piece.Start >= MinSplitSeconds) {
		const double length = Piece.End - Piece.Start;
		const size_t from = static_cast<size_t>((Piece.Start + length * 0.3) / FrameSeconds);
		const size_t to = std::min(Map.Levels.size(), static_cast<size_t>((Piece.Start + length * 0.7) / FrameSeconds));
		size_t quietest = from;
		for (size_t f = from; f < to; f++) {
			if (Map.Levels[f] < Map.Levels[quietest])
				quietest = f;
		}
		const double cut = std::clamp(quietest * FrameSeconds, Piece.Start + 1.0, Piece.End - 1.0);
		const Chunk left{ Piece.Start, cut, voicedIn(Piece.Start, cut) };
		const Chunk right{ cut, Piece.End, voicedIn(cut, Piece.End) };
		std::vector<Word> split;
		if (!DecodeAdaptive(State, Mono, Map, left, Language, Cancel, split) || !DecodeAdaptive(State, Mono, Map, right, Language, Cancel, split))
			return false;
		if (split.size() > words.size() || bRepetitive)
			words = std::move(split);
	}
	else {
		std::vector<Word> retry;
		if (!DecodeChunk(State, Mono, Piece, Language, RetryTemperature, Cancel, retry))
			return false;
		if (!LooksRepetitive(retry) && (retry.size() > words.size() || bRepetitive))
			words = std::move(retry);
	}
	if (!LooksRepetitive(words))
		Out.insert(Out.end(), words.begin(), words.end());
	return true;
}

std::string Transcriber::RhymeKey(const std::string& Text)
{
	std::string letters;
	for (const char c : Lower(Text)) {
		if (std::isalpha(static_cast<unsigned char>(c)))
			letters += c;
	}
	const auto vowel = [](char c) { return c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u' || c == 'y'; };
	size_t i = letters.size();
	while (i > 0 && !vowel(letters[i - 1]))
		i--;
	while (i > 0 && vowel(letters[i - 1]))
		i--;
	const std::string key = letters.substr(i);
	return key.size() >= 2 && key.size() < letters.size() + 1 ? key : std::string();
}

void Transcriber::FixTimings(std::vector<Word>& Words, const std::vector<Activity>& Active)
{
	const auto runAt = [&](double Time) -> const Activity* {
		auto it = std::upper_bound(Active.begin(), Active.end(), Time, [](double t, const Activity& a) { return t < a.Start; });
		if (it == Active.begin())
			return nullptr;
		--it;
		return Time <= it->End ? &*it : nullptr;
	};

	std::vector<Word> kept;
	kept.reserve(Words.size());
	for (Word word : Words) {
		if (IsSymbols(word.Text))
			continue;
		if (!Active.empty() && !runAt(word.Start)) {
			auto next = std::upper_bound(Active.begin(), Active.end(), word.Start, [](double t, const Activity& a) { return t < a.Start; });
			const Activity* forward = next != Active.end() ? &*next : nullptr;
			const Activity* backward = next != Active.begin() ? &*std::prev(next) : nullptr;
			const double toForward = forward ? forward->Start - word.Start : std::numeric_limits<double>::infinity();
			const double toBackward = backward ? word.Start - backward->End : std::numeric_limits<double>::infinity();
			if (toForward <= SnapReach && toForward <= toBackward * 2.0)
				word.Start = forward->Start;
			else if (toBackward <= SnapReach * 0.5)
				word.Start = std::max(backward->Start, backward->End - MinWordSeconds);
			else
				continue;
		}
		if (!kept.empty())
			word.Start = std::max(word.Start, kept.back().Start + 0.04);
		kept.push_back(std::move(word));
	}

	for (size_t i = 0; i < kept.size(); i++) {
		Word& word = kept[i];
		double end = word.Start + MaxWordSeconds;
		if (i + 1 < kept.size())
			end = std::min(end, kept[i + 1].Start);
		if (const Activity* run = runAt(word.Start))
			end = std::min(end, run->End + 0.05);
		word.End = std::max(end, word.Start + MinWordSeconds);
	}
	Words = std::move(kept);
}

std::string Transcriber::LineText(const std::vector<Word>& Words, size_t First, size_t Last)
{
	std::string text;
	for (size_t i = First; i <= Last; i++) {
		std::string word = Words[i].Text;
		const size_t at = word.find_first_not_of(' ');
		if (at != std::string::npos && word[at] == 'i' && (at + 1 == word.size() || word[at + 1] == '\'' || !std::isalpha(static_cast<unsigned char>(word[at + 1]))))
			word[at] = 'I';
		text += word;
	}
	return Trim(text);
}

double Transcriber::LineCost(const std::vector<Word>& Words, size_t First, size_t Last)
{
	const size_t chars = LineText(Words, First, Last).size();
	const size_t count = Last - First + 1;
	const double duration = Words[Last].End - Words[First].Start;
	double cost = 1.0;
	if (chars > 34)
		cost += (chars - 34) * 0.06;
	if (chars > 48)
		cost += (chars - 48) * 0.4;
	if (chars > 72)
		cost += 60.0;
	if (chars < 10)
		cost += (10 - chars) * 0.18;
	if (count == 1)
		cost += 1.2;
	if (duration > 7.0)
		cost += (duration - 7.0) * 2.0;
	const std::string endRhyme = RhymeKey(Words[Last].Text);
	if (!endRhyme.empty()) {
		for (size_t i = First + 1; i < Last; i++) {
			if (RhymeKey(Words[i].Text) == endRhyme && Lower(Words[i].Text) != Lower(Words[Last].Text)) {
				cost -= 1.4;
				break;
			}
		}
	}
	for (size_t i = First; i < Last; i++) {
		const double gap = Words[i + 1].Start - Words[i].End;
		if (gap > 0.3)
			cost += (gap - 0.3) * 7.0;
		if (gap > 2.0)
			cost += 100.0;
	}
	return cost;
}

double Transcriber::BreakCost(const Word& Before, const Word& After, bool bMixedCase)
{
	static const std::array<const char*, 22> weakEndings = {
		"the", "a", "an", "to", "my", "your", "his", "her", "our", "their", "of", "and", "in", "on",
		"with", "by", "for", "at", "from", "is", "that", "it's"
	};
	static const std::array<const char*, 7> starters = { "i", "i'm", "i'ma", "i'll", "i've", "i'd", "yeah" };

	const auto isPunctuated = [](const std::string& Text) {
		const char c = Text.empty() ? ' ' : Text.back();
		return c == '.' || c == '?' || c == '!' || c == ',' || c == ';' || c == ':';
	};
	const double gap = After.Start - Before.End;
	double cost = -std::min(std::max(gap, 0.0), 1.2) * 3.0;
	const std::string before = Trim(Before.Text);
	const char last = before.empty() ? ' ' : before.back();
	if (last == '.' || last == '?' || last == '!')
		cost -= 3.0;
	else if (last == ',' || last == ';' || last == ':')
		cost -= 2.2;
	else if (isPunctuated(Trim(After.Text)))
		cost += 1.2;
	const std::string endWord = Lower(before);
	const std::string nextWord = Lower(After.Text);
	if (std::find(weakEndings.begin(), weakEndings.end(), endWord) != weakEndings.end())
		cost += 2.0;
	if (std::find(starters.begin(), starters.end(), nextWord) != starters.end())
		cost -= 0.5;
	const std::string after = Trim(After.Text);
	if (bMixedCase && !after.empty() && std::isupper(static_cast<unsigned char>(after[0])) && nextWord != "i" && nextWord.rfind("i'", 0) != 0)
		cost -= 0.6;
	if (Before.bSegmentEnd)
		cost -= !after.empty() && std::isupper(static_cast<unsigned char>(after[0])) ? 1.8 : 0.15;
	return cost;
}

std::vector<Transcriber::Line> Transcriber::BuildLines(const std::vector<Word>& Words)
{
	std::vector<Line> lines;
	const size_t n = Words.size();
	if (n == 0)
		return lines;

	size_t upper = 0;
	size_t lower = 0;
	for (const Word& word : Words) {
		const std::string text = Trim(word.Text);
		if (!text.empty() && std::isupper(static_cast<unsigned char>(text[0])) && Lower(text) != "i" && Lower(text).rfind("i'", 0) != 0)
			upper++;
		else if (!text.empty() && std::islower(static_cast<unsigned char>(text[0])))
			lower++;
	}
	const bool bMixedCase = upper * 8 >= lower;

	std::vector<double> best(n + 1, std::numeric_limits<double>::infinity());
	std::vector<size_t> from(n + 1, 0);
	best[0] = 0.0;
	for (size_t end = 1; end <= n; end++) {
		const size_t earliest = end > MaxLineWords ? end - MaxLineWords : 0;
		for (size_t start = earliest; start < end; start++) {
			if (!std::isfinite(best[start]))
				continue;
			double cost = best[start] + LineCost(Words, start, end - 1);
			if (start > 0)
				cost += BreakCost(Words[start - 1], Words[start], bMixedCase);
			if (cost < best[end]) {
				best[end] = cost;
				from[end] = start;
			}
		}
	}

	std::vector<std::pair<size_t, size_t>> spans;
	for (size_t end = n; end > 0; end = from[end])
		spans.emplace_back(from[end], end - 1);
	std::reverse(spans.begin(), spans.end());

	for (const auto& [first, last] : spans) {
		std::string text = LineText(Words, first, last);
		while (!text.empty() && (text.back() == '.' || text.back() == ',' || text.back() == ';' || text.back() == ':'))
			text.pop_back();
		text = Trim(text);
		if (!text.empty() && std::islower(static_cast<unsigned char>(text[0])))
			text[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(text[0])));
		if (IsNoise(text))
			continue;
		lines.push_back({ Words[first].Start, Words[last].End, std::move(text) });
	}
	return lines;
}

std::optional<Transcriber::Result> Transcriber::Transcribe(const std::vector<float>& Mono, std::string& Error, const std::atomic<bool>* Cancel)
{
	if (!Context) {
		Error = "the Whisper model isn't loaded";
		return std::nullopt;
	}
	whisper_state* state = Acquire();
	if (!state) {
		Error = "couldn't create a Whisper state";
		return std::nullopt;
	}

	const VocalMap map = FindActivity(Mono);
	const std::vector<Chunk> chunks = PlanChunks(map, static_cast<double>(Mono.size()) / SampleRate);
	const int language = DetectLanguage(state, Mono, chunks);
	const char* languageName = language >= 0 ? whisper_lang_str(language) : "auto";
	Result result;
	for (const Chunk& piece : chunks) {
		if (!DecodeAdaptive(state, Mono, map, piece, languageName, Cancel, result.Words)) {
			Release(state);
			Error = Cancel && Cancel->load() ? "cancelled" : "Whisper couldn't transcribe the vocals";
			return std::nullopt;
		}
	}
	Release(state);

	FindVocalBounds(map, result.VocalStart, result.VocalEnd);

	FixTimings(result.Words, map.Runs);
	DropQuietSegments(result.Words, map);
	result.Lines = BuildLines(result.Words);
	return result;
}
