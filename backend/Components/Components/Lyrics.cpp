#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <optional>
#include <sstream>

#include "Lyrics.hpp"
#include "AppData.hpp"
#include "AudioLoader.hpp"
#include "Logger.hpp"
#include "ModelStore.hpp"
#include "Separator.hpp"
#include "Transcriber.hpp"
#include "Utils.hpp"

std::filesystem::path Lyrics::ModelsDirectory()
{
	return AppData::Directory() / "models";
}

bool Lyrics::NeedsWork(const std::string& Status, int StoredVersion)
{
	return Status.empty() || (StoredVersion < Version && Status != "skipped");
}

void Lyrics::Start()
{
	if (bRunning.exchange(true))
		return;
	bCancel = false;
	Booter = std::thread(Boot);
}

bool Lyrics::PrepareModels()
{
	const std::filesystem::path models = ModelsDirectory();
	while (bRunning) {
		std::string error;
		if (ModelStore::Ensure(models, bCancel, error))
			return true;
		if (bCancel)
			return false;
		Logger::Warning("[Lyrics] Couldn't get the lyrics models (" + error + "), trying again in 10 minutes");
		std::unique_lock lock(Mutex);
		Wake.wait_for(lock, DownloadRetry, [] { return !bRunning.load(); });
	}
	return false;
}

void Lyrics::Boot()
{
	if (!PrepareModels())
		return;

	const std::filesystem::path models = ModelsDirectory();
	const auto started = std::chrono::steady_clock::now();
	std::string error;
	auto separation = std::make_unique<Separator>();
	if (!separation->Load(models / ModelStore::Separation.File, error)) {
		Logger::Error("[Lyrics] Couldn't load the vocal separation model: " + error);
		bRunning = false;
		return;
	}
	auto transcription = std::make_unique<Transcriber>();
	if (!transcription->Load(models / ModelStore::Whisper.File, error)) {
		Logger::Error("[Lyrics] Couldn't load Whisper: " + error);
		bRunning = false;
		return;
	}
	Separation = std::move(separation);
	Transcription = std::move(transcription);
	const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
	Logger::Info("[Lyrics] Models ready in " + std::to_string(elapsed) + " ms (separation on " + (Separation->IsGpu() ? "GPU" : "CPU") + ")");

	if (!bRunning)
		return;
	QueueMissing();
	std::lock_guard lock(Mutex);
	for (unsigned i = 0; i < WorkerCount; i++)
		Workers.emplace_back(Work);
}

void Lyrics::QueueMissing()
{
	const auto files = AppData::FilesNeedingLyrics(Version);
	std::lock_guard lock(Mutex);
	for (const auto& [id, path] : files) {
		if (Queued.insert(id).second)
			Pending.emplace_back(id, path);
	}
	Logger::Info("[Lyrics] " + std::to_string(Pending.size()) + " songs waiting for lyrics");
	Wake.notify_all();
}

void Lyrics::Prioritize(const std::string& Id, const std::filesystem::path& Path)
{
	if (!bRunning)
		return;
	const std::optional<File> file = AppData::FindFile(Id);
	if (!file || !NeedsWork(file->LyricsStatus, file->LyricsVersion))
		return;
	std::lock_guard lock(Mutex);
	if (auto it = std::find_if(Pending.begin(), Pending.end(), [&](const Job& job) { return job.first == Id; }); it != Pending.end())
		Pending.erase(it);
	else if (Queued.contains(Id))
		return;
	Pending.emplace_front(Id, Path);
	Queued.insert(Id);
	if (Upcoming.empty() || Upcoming.front() != Id)
		Upcoming.insert(Upcoming.begin(), Id);
	Wake.notify_one();
}

void Lyrics::FollowQueue(std::vector<std::string> Ids)
{
	if (Ids.size() > FollowLimit)
		Ids.resize(FollowLimit);
	std::lock_guard lock(Mutex);
	Upcoming = std::move(Ids);
}

size_t Lyrics::ClearAll()
{
	const size_t cleared = AppData::ClearLyrics();
	Unsaved = 0;
	bool bReady;
	{
		std::lock_guard lock(Mutex);
		bReady = bRunning && !Workers.empty();
	}
	if (bReady)
		QueueMissing();
	return cleared;
}

std::optional<Lyrics::Job> Lyrics::NextJob()
{
	for (const std::string& id : Upcoming) {
		auto it = std::find_if(Pending.begin(), Pending.end(), [&](const Job& job) { return job.first == id; });
		if (it != Pending.end()) {
			Job job = std::move(*it);
			Pending.erase(it);
			return job;
		}
	}
	if (Pending.empty())
		return std::nullopt;
	Job job = std::move(Pending.front());
	Pending.pop_front();
	return job;
}

void Lyrics::Work()
{
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
	while (true) {
		Job job;
		{
			std::unique_lock lock(Mutex);
			Wake.wait(lock, [] { return !bRunning || !Pending.empty(); });
			if (!bRunning)
				return;
			std::optional<Job> next = NextJob();
			if (!next)
				continue;
			job = std::move(*next);
		}
		Process(job.first, job.second);
		{
			std::lock_guard lock(Mutex);
			Queued.erase(job.first);
		}
		Save(false);
	}
}

void Lyrics::Store(const std::string& Id, std::vector<LyricLine> Lines, const std::string& Status, double VocalStart, double VocalEnd)
{
	AppData::SetLyrics(Id, std::move(Lines), Status, Version, VocalStart, VocalEnd);
	Unsaved++;
}

void Lyrics::Process(const std::string& Id, const std::filesystem::path& Path)
{
	const std::optional<File> file = AppData::FindFile(Id);
	if (!file || !NeedsWork(file->LyricsStatus, file->LyricsVersion))
		return;
	const std::string name = Utils::ToUtf8(Path.filename());
	if (file->Duration > MaxSeconds) {
		Store(Id, {}, "skipped");
		Logger::Info("[Lyrics] Skipped " + name + " (longer than 20 minutes)");
		return;
	}

	const auto started = std::chrono::steady_clock::now();
	auto mark = started;
	std::ostringstream stages;
	stages << std::fixed << std::setprecision(1);
	const auto lap = [&](const char* Stage) {
		const auto now = std::chrono::steady_clock::now();
		stages << (stages.tellp() > 0 ? ", " : "") << Stage << " " << std::chrono::duration<double>(now - mark).count() << " s";
		mark = now;
	};
	std::string error;
	const auto fail = [&](const std::string& Stage) {
		if (bCancel)
			return;
		if (file->LyricsStatus.empty())
			Store(Id, {}, "failed");
		Logger::Warning("[Lyrics] " + Stage + " failed for " + name + ": " + error);
	};

	std::optional<std::vector<float>> mix = AudioLoader::Load(Path, NormalizeFilters, error, &bCancel);
	if (!mix)
		return fail("Loading");
	lap("load");
	std::vector<float> vocals = Separation->Vocals(*mix, error, &bCancel);
	mix.reset();
	if (vocals.empty())
		return fail("Vocal separation");
	lap("separate");
	const std::vector<float> mono = AudioLoader::Resample(vocals, Separator::SampleRate, 2, Transcriber::SampleRate, 1);
	vocals.clear();
	if (mono.empty()) {
		error = "resampling produced no audio";
		return fail("Resampling");
	}
	std::optional<Transcriber::Result> result;
	{
		std::lock_guard lock(TranscribeLock);
		lap("wait");
		result = Transcription->Transcribe(mono, error, &bCancel);
	}
	if (!result)
		return fail("Transcription");
	lap("transcribe");

	std::vector<LyricLine> stored;
	stored.reserve(result->Lines.size());
	for (const Transcriber::Line& line : result->Lines)
		stored.push_back({ std::round(line.Start * 100.0) / 100.0, std::round(line.End * 100.0) / 100.0, line.Text });
	const size_t count = stored.size();
	Store(Id, std::move(stored), count ? "done" : "none", result->VocalStart, result->VocalEnd);

	const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
	std::ostringstream took;
	took << std::fixed << std::setprecision(1) << seconds;
	Logger::Info("[Lyrics] " + name + ": " + (count ? std::to_string(count) + " lines" : std::string("no vocals")) + " in " + took.str() + " s (" + stages.str() + ")");
}

void Lyrics::Save(bool bForce)
{
	bool bEmpty;
	{
		std::lock_guard lock(Mutex);
		bEmpty = Pending.empty();
	}
	if (Unsaved == 0 || (!bForce && !bEmpty && Unsaved < SaveEvery))
		return;
	Unsaved = 0;
	AppData::SaveFileTree();
}

void Lyrics::Shutdown()
{
	bCancel = true;
	bRunning = false;
	Wake.notify_all();
	if (Booter.joinable())
		Booter.join();
	for (auto& worker : Workers) {
		if (worker.joinable())
			worker.join();
	}
	Workers.clear();
	Save(true);
	Transcription.reset();
	Separation.reset();
}
