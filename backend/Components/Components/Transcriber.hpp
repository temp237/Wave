#pragma once
#include <atomic>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

struct whisper_context;
struct whisper_state;

class Transcriber
{
public:
	static constexpr int SampleRate = 16000;

	struct Line {
		double Start = 0.0;
		double End = 0.0;
		std::string Text;
	};

	struct Word {
		double Start = 0.0;
		double End = 0.0;
		std::string Text;
		bool bSegmentEnd = false;
	};

	struct Result {
		std::vector<Line> Lines;
		std::vector<Word> Words;
		double VocalStart = -1.0;
		double VocalEnd = -1.0;
	};

	Transcriber() = default;
	~Transcriber();
	Transcriber(const Transcriber&) = delete;
	Transcriber& operator=(const Transcriber&) = delete;

	bool Load(const std::filesystem::path& Model, std::string& Error);
	std::optional<Result> Transcribe(const std::vector<float>& Mono, std::string& Error, const std::atomic<bool>* Cancel = nullptr);
	static std::vector<Line> BuildLines(const std::vector<Word>& Words);

private:
	struct Activity {
		double Start = 0.0;
		double End = 0.0;
	};

	struct VocalMap {
		std::vector<float> Levels;
		std::vector<Activity> Runs;
		float Reference = 0.f;
	};

	struct Chunk {
		double Start = 0.0;
		double End = 0.0;
		double Voiced = 0.0;
	};

	static constexpr double ChunkSeconds = 28.0;
	static constexpr double ChunkPad = 0.3;
	static constexpr double MinChunkSeconds = 1.2;
	static constexpr double ThinWordsPerSecond = 0.6;
	static constexpr double ThinVoicedSeconds = 3.0;
	static constexpr float RetryTemperature = 0.4f;
	static constexpr double MinSplitSeconds = 8.0;
	static constexpr double FrameSeconds = 0.02;
	static constexpr double ActivityRangeDb = 38.0;
	static constexpr double ActivityFloorDb = -58.0;
	static constexpr double ActivityBridge = 0.15;
	static constexpr double MinVocalRunSeconds = 0.3;
	static constexpr double QuietSegmentDb = -36.0;
	static constexpr double QuietBelowReferenceDb = 16.0;
	static constexpr double SnapReach = 1.5;
	static constexpr double MaxWordSeconds = 1.6;
	static constexpr double MinWordSeconds = 0.08;
	static constexpr float NoSpeechLimit = 0.85f;
	static constexpr size_t MaxLineWords = 16;
	static constexpr int Threads = 4;

	whisper_state* Acquire();
	void Release(whisper_state* State);
	static VocalMap FindActivity(const std::vector<float>& Mono);
	static std::vector<Chunk> PlanChunks(const VocalMap& Map, double Duration);
	bool DecodeChunk(whisper_state* State, const std::vector<float>& Mono, const Chunk& Piece, const char* Language, float Temperature, const std::atomic<bool>* Cancel, std::vector<Word>& Out);
	int DetectLanguage(whisper_state* State, const std::vector<float>& Mono, const std::vector<Chunk>& Chunks);
	static bool LooksRepetitive(const std::vector<Word>& Words);
	static std::string RhymeKey(const std::string& Text);
	static void FixTimings(std::vector<Word>& Words, const std::vector<Activity>& Active);
	static void DropQuietSegments(std::vector<Word>& Words, const VocalMap& Map);
	static double LineCost(const std::vector<Word>& Words, size_t First, size_t Last);
	static double BreakCost(const Word& Before, const Word& After, bool bMixedCase);
	static std::string LineText(const std::vector<Word>& Words, size_t First, size_t Last);
	static std::string Lower(const std::string& Text);
	static bool IsNoise(const std::string& Text);
	static bool IsSymbols(const std::string& Text);
	bool DecodeAdaptive(whisper_state* State, const std::vector<float>& Mono, const VocalMap& Map, const Chunk& Piece, const char* Language, const std::atomic<bool>* Cancel, std::vector<Word>& Out);
	static std::string Trim(const std::string& Text);

	whisper_context* Context = nullptr;
	std::mutex Lock;
	std::vector<whisper_state*> Idle;
	std::vector<whisper_state*> All;
};
