#include "Components/Includes.hpp"

constexpr bool SHOW_CONSOLE = false;
constexpr int ArtworkSize = 1024;

int main() {
	if (const std::optional<int> code = MicRelay::HandleCommandLine())
		return *code;

	if constexpr (SHOW_CONSOLE)
		Logger::OpenConsole();

	Equalizer::Load();
	DeEsser::Load();
	Normalize::Load();
	Lyrics::Start();
	Startup::Refresh();
	MicRelay::Load();
	AudioRelay::Open();
	DiscordPresence::Load();
	Player::Init();
	Queue::Init();
	{
		const nlohmann::json playback = Settings::Get("playback");
		const double volume = Settings::Number(playback, "volume", 0.8);
		Player::SetVolume(Settings::Bool(playback, "muted", false) ? 0.0 : volume);
	}

	TaskQueue library;
	library.Start();
	TaskQueue covers;
	covers.Start();

	TcpSocket socket;
	socket.Start();
	if (!socket.IsListening())
		return 1;

	Logger::Info("[Wave] Listening on 127.0.0.1:" + std::to_string(socket.GetPort()));

	// Give port to frontend via IPC pipeline
	IPCPipe pipe;
	pipe.onConnection([&]() {
		return std::to_string(socket.GetPort());
	});
	pipe.Start("waveport", 256);
	Logger::Info("[IPC] Port sent to frontend");

	// Frontend connects now
	if (!socket.Accept())
		return 1;
	Logger::Success("[Wave] Frontend connected");

	DiscordPresence::Initialize();
	Player::OnStateChanged([&](const PlayerState& state) {
		DiscordPresence::OnPlayerState(state);
		Queue::Persist(state.Status != PlayerStatus::Playing && state.Status != PlayerStatus::Loading);
		socket.Send(EncodeMessage({ MessageType::PlayerState, {
			Player::StatusName(state.Status), state.FileId, state.Position, state.Duration } }));
	});
	Queue::OnChanged([&](const std::string& json) {
		socket.Send(EncodeMessage({ MessageType::QueueState, { json } }));
	});
	Visualizer::Start([&](const Visualizer::Frame& frame) {
		std::vector<Value> bands(frame.begin(), frame.end());
		socket.Send(EncodeMessage({ MessageType::Spectrum, std::move(bands) }));
	});

	socket.onRecv([&](const std::string& payload) {
		auto msg = DecodeMessage(payload);
		if (!msg) {
			Logger::Error("[Message] Malformed message (" + std::to_string(payload.size()) + " bytes)");
			return;
		}

		switch (msg->type) {
		case MessageType::Debug:
			Logger::Debug("[Message] Debug, " + std::to_string(msg->values.size()) + " value(s)");
			for (size_t i = 0; i < msg->values.size(); i++)
				Logger::Debug("[Message] #" + std::to_string(i) + "  " + ValueToString(msg->values[i]));
			socket.Send(EncodeMessage(*msg));
			break;
		case MessageType::ImportFile: {
			const std::string* path = msg->StringAt(0);
			const std::string* folder = msg->StringAt(1);
			library.Post([&socket, path = path ? *path : std::string(), folder = folder ? *folder : AppData::LooseFolder] {
				std::optional<std::string> id;
				if (!path.empty())
					id = AppData::RegisterFile(Utils::FromUtf8(path), folder);
				else
					Logger::Error("[Message] ImportFile needs a path");
				socket.Send(EncodeMessage({ MessageType::ImportFile, { id.has_value(), id.value_or("") } }));
			});
			break;
		}
		case MessageType::ImportFolder: {
			const std::string* path = msg->StringAt(0);
			library.Post([&socket, path = path ? *path : std::string()] {
				std::optional<size_t> count;
				if (!path.empty())
					count = AppData::RegisterFolder(Utils::FromUtf8(path));
				else
					Logger::Error("[Message] ImportFolder needs a path");
				socket.Send(EncodeMessage({ MessageType::ImportFolder, { count.has_value(), static_cast<uint32_t>(count.value_or(0)) } }));
			});
			break;
		}
		case MessageType::GetFileTree:
			library.Post([&socket] {
				AppData::RefreshMetadata();
				socket.Send(EncodeMessage({ MessageType::GetFileTree, { AppData::GetFileTreeJson() } }));
			});
			break;
		case MessageType::GetCover: {
			const std::string* id = msg->StringAt(0);
			auto cover = id ? AppData::GetCover(*id) : std::nullopt;
			socket.Send(EncodeMessage({ MessageType::GetCover, { cover.has_value(), cover ? cover->first : std::string(), cover ? cover->second : std::string() } }));
			break;
		}
		case MessageType::Play: {
			const std::string* id = msg->StringAt(0);
			std::optional<File> file = id ? AppData::FindFile(*id) : std::nullopt;
			bool ok = false;
			if (file)
				ok = Player::Play(file->Path, *id);
			else
				Logger::Error("[Message] Play: unknown file");
			socket.Send(EncodeMessage({ MessageType::Play, { ok } }));
			break;
		}
		case MessageType::Pause:
			socket.Send(EncodeMessage({ MessageType::Pause, { Player::Pause() } }));
			break;
		case MessageType::Resume:
			socket.Send(EncodeMessage({ MessageType::Resume, { Player::Resume() } }));
			break;
		case MessageType::Stop:
			Queue::Stop();
			socket.Send(EncodeMessage({ MessageType::Stop, { true } }));
			break;
		case MessageType::Seek: {
			const double* seconds = msg->DoubleAt(0);
			socket.Send(EncodeMessage({ MessageType::Seek, { seconds ? Player::Seek(*seconds) : false } }));
			break;
		}
		case MessageType::SetVolume: {
			const double* volume = msg->DoubleAt(0);
			const bool* muted = msg->BoolAt(1);
			const bool* persist = msg->BoolAt(2);
			bool ok = false;
			if (volume) {
				const bool isMuted = muted && *muted;
				ok = Player::SetVolume(isMuted ? 0.0 : *volume);
				if (persist && *persist)
					Settings::Merge("playback", { { "volume", *volume }, { "muted", isMuted } });
			}
			socket.Send(EncodeMessage({ MessageType::SetVolume, { ok } }));
			break;
		}
		case MessageType::GetPlaylists:
			socket.Send(EncodeMessage({ MessageType::GetPlaylists, { AppData::GetPlaylistsJson() } }));
			break;
		case MessageType::CreatePlaylist: {
			const std::string* name = msg->StringAt(0);
			auto created = name ? AppData::CreatePlaylist(*name, msg->StringsFrom(1)) : std::nullopt;
			socket.Send(EncodeMessage({ MessageType::CreatePlaylist, { created.has_value(), created ? created->ToJson().dump() : std::string() } }));
			break;
		}
		case MessageType::RenamePlaylist: {
			const std::string* id = msg->StringAt(0);
			const std::string* name = msg->StringAt(1);
			socket.Send(EncodeMessage({ MessageType::RenamePlaylist, { id && name && AppData::RenamePlaylist(*id, *name) } }));
			break;
		}
		case MessageType::DeletePlaylist: {
			const std::string* id = msg->StringAt(0);
			socket.Send(EncodeMessage({ MessageType::DeletePlaylist, { id && AppData::DeletePlaylist(*id) } }));
			break;
		}
		case MessageType::AddToPlaylist: {
			const std::string* id = msg->StringAt(0);
			auto added = id ? AppData::AddToPlaylist(*id, msg->StringsFrom(1)) : std::nullopt;
			socket.Send(EncodeMessage({ MessageType::AddToPlaylist, { added.has_value(), static_cast<uint32_t>(added.value_or(0)) } }));
			break;
		}
		case MessageType::RemoveFromPlaylist: {
			const std::string* id = msg->StringAt(0);
			const std::vector<std::string> tracks = msg->StringsFrom(1);
			auto removed = id ? AppData::RemoveFromPlaylist(*id, tracks) : std::nullopt;
			if (removed && *removed > 0 && Queue::CurrentSource() == "pl:" + *id)
				Queue::RemoveTracks(tracks);
			socket.Send(EncodeMessage({ MessageType::RemoveFromPlaylist, { removed.has_value(), static_cast<uint32_t>(removed.value_or(0)) } }));
			break;
		}
		case MessageType::GetCovers:
			covers.Post([&socket, ids = msg->StringsFrom(0)] {
				std::vector<Value> values;
				for (auto& entry : AppData::GetCovers(ids)) {
					for (auto& part : entry)
						values.push_back(std::move(part));
				}
				socket.Send(EncodeMessage({ MessageType::GetCovers, std::move(values) }));
			});
			break;
		case MessageType::QueueSet: {
			const std::string* source = msg->StringAt(0);
			const uint32_t* start = msg->U32At(1);
			const bool ok = Queue::Set(msg->StringsFrom(2), start ? *start : 0, source ? *source : std::string());
			socket.Send(EncodeMessage({ MessageType::QueueSet, { ok } }));
			break;
		}
		case MessageType::QueueNext:
			socket.Send(EncodeMessage({ MessageType::QueueNext, { Queue::Next() } }));
			break;
		case MessageType::QueuePrevious:
			socket.Send(EncodeMessage({ MessageType::QueuePrevious, { Queue::Previous() } }));
			break;
		case MessageType::QueueJump: {
			const uint32_t* index = msg->U32At(0);
			socket.Send(EncodeMessage({ MessageType::QueueJump, { index && Queue::Jump(*index) } }));
			break;
		}
		case MessageType::QueueRemove: {
			const uint32_t* index = msg->U32At(0);
			socket.Send(EncodeMessage({ MessageType::QueueRemove, { index && Queue::Remove(*index) } }));
			break;
		}
		case MessageType::QueuePlayNext: {
			const std::string* id = msg->StringAt(0);
			socket.Send(EncodeMessage({ MessageType::QueuePlayNext, { id && Queue::PlayNext(*id) } }));
			break;
		}
		case MessageType::QueueAdd:
			socket.Send(EncodeMessage({ MessageType::QueueAdd, { Queue::Add(msg->StringsFrom(0)) } }));
			break;
		case MessageType::QueueClear:
			Queue::Clear();
			socket.Send(EncodeMessage({ MessageType::QueueClear, { true } }));
			break;
		case MessageType::SetShuffle: {
			const bool* on = msg->BoolAt(0);
			if (on) {
				Queue::SetShuffle(*on);
				Settings::Merge("playback", { { "shuffle", *on } });
			}
			socket.Send(EncodeMessage({ MessageType::SetShuffle, { on != nullptr } }));
			break;
		}
		case MessageType::SetRepeat: {
			const std::string* mode = msg->StringAt(0);
			if (mode) {
				const RepeatMode repeat = Queue::ParseRepeat(*mode);
				Queue::SetRepeat(repeat);
				Settings::Merge("playback", { { "repeat", Queue::RepeatName(repeat) } });
			}
			socket.Send(EncodeMessage({ MessageType::SetRepeat, { mode != nullptr } }));
			break;
		}
		case MessageType::GetQueue:
			socket.Send(EncodeMessage({ MessageType::GetQueue, { Queue::StateJson() } }));
			break;
		case MessageType::GetSettings: {
			const Logger::LogStats logs = Logger::Stats();
			const nlohmann::json settings = {
				{ "equalizer", Equalizer::ToJson() },
				{ "deesser", DeEsser::ToJson() },
				{ "normalize", Normalize::StateJson() },
				{ "cache", AppData::CacheInfo() },
				{ "lyrics", AppData::LyricsInfo() },
				{ "logs", { { "bytes", logs.Bytes }, { "files", logs.Files } } },
				{ "discord", { { "enabled", DiscordPresence::IsEnabled() } } },
				{ "playback", Settings::Get("playback") },
				{ "interface", Settings::Get("interface") },
				{ "startup", { { "enabled", Startup::IsEnabled() } } },
				{ "microphone", MicRelay::StateJson() },
				{ "dataFolder", Utils::ToUtf8(AppData::Directory()) },
			};
			socket.Send(EncodeMessage({ MessageType::GetSettings, { settings.dump() } }));
			break;
		}
		case MessageType::SetEqualizer: {
			const std::string* json = msg->StringAt(0);
			bool ok = false;
			if (json) {
				const nlohmann::json j = nlohmann::json::parse(*json, nullptr, false);
				ok = Equalizer::FromJson(j) && Equalizer::Save();
			}
			socket.Send(EncodeMessage({ MessageType::SetEqualizer, { ok } }));
			break;
		}
		case MessageType::SetNormalize: {
			const bool* on = msg->BoolAt(0);
			if (on)
				library.Post([on = *on] { Normalize::SetEnabled(on); });
			socket.Send(EncodeMessage({ MessageType::SetNormalize, { on != nullptr } }));
			break;
		}
		case MessageType::GetNormalize:
			library.Post([&socket] {
				socket.Send(EncodeMessage({ MessageType::GetNormalize, { Normalize::StateJson().dump() } }));
			});
			break;
		case MessageType::GetLyrics: {
			const std::string* id = msg->StringAt(0);
			const std::optional<File> file = id ? AppData::FindFile(*id) : std::nullopt;
			nlohmann::json result = { { "id", id ? *id : std::string() }, { "status", "missing" }, { "lines", nlohmann::json::array() } };
			if (file) {
				std::string status = file->LyricsStatus;
				if (status.empty())
					status = Lyrics::IsRunning() ? "pending" : "unavailable";
				result["status"] = status;
				for (const LyricLine& line : file->Lyrics)
					result["lines"].push_back({ { "start", line.Start }, { "end", line.End }, { "text", line.Text } });
				if (Lyrics::NeedsWork(file->LyricsStatus, file->LyricsVersion))
					Lyrics::Prioritize(*id, file->Path);
			}
			socket.Send(EncodeMessage({ MessageType::GetLyrics, { result.dump() } }));
			break;
		}
		case MessageType::SetVisualizer: {
			const bool* on = msg->BoolAt(0);
			if (on)
				Visualizer::SetEnabled(*on);
			socket.Send(EncodeMessage({ MessageType::SetVisualizer, { on != nullptr } }));
			break;
		}
		case MessageType::GetArtwork:
			covers.Post([&socket, id = msg->StringAt(0) ? *msg->StringAt(0) : std::string()] {
				const std::optional<File> file = AppData::FindFile(id);
				const std::optional<CoverImage> cover = file ? Metadata::MakeCover(file->Path, ArtworkSize) : std::nullopt;
				socket.Send(EncodeMessage({ MessageType::GetArtwork, { cover.has_value(), cover ? Metadata::ImageMime(cover->Jpeg) : std::string(), cover ? Utils::Base64(cover->Jpeg) : std::string(), cover ? cover->Accent : 0u } }));
			});
			break;
		case MessageType::SetDeEsser: {
			const std::string* json = msg->StringAt(0);
			bool ok = false;
			if (json) {
				const nlohmann::json j = nlohmann::json::parse(*json, nullptr, false);
				ok = DeEsser::FromJson(j) && DeEsser::Save();
			}
			socket.Send(EncodeMessage({ MessageType::SetDeEsser, { ok } }));
			break;
		}
		case MessageType::RemoveFromLibrary:
			library.Post([&socket, ids = msg->StringsFrom(0)] {
				Queue::RemoveTracks(ids);
				const size_t removed = AppData::RemoveFromLibrary(ids);
				socket.Send(EncodeMessage({ MessageType::RemoveFromLibrary, { true, static_cast<uint32_t>(removed) } }));
			});
			break;
		case MessageType::GetPlayerState: {
			const PlayerState state = Player::GetState();
			socket.Send(EncodeMessage({ MessageType::GetPlayerState, {
				Player::StatusName(state.Status), state.FileId, state.Position, state.Duration } }));
			break;
		}
		case MessageType::SetInterface: {
			const std::string* json = msg->StringAt(0);
			bool ok = false;
			if (json) {
				const nlohmann::json patch = nlohmann::json::parse(*json, nullptr, false);
				if (patch.is_object())
					ok = Settings::Merge("interface", patch);
			}
			socket.Send(EncodeMessage({ MessageType::SetInterface, { ok } }));
			break;
		}
		case MessageType::SetMicRelay: {
			const bool* enabled = msg->BoolAt(0);
			library.Post([&socket, on = enabled && *enabled, valid = enabled != nullptr] {
				std::string error = valid ? std::string() : "missing value";
				const bool ok = valid && MicRelay::SetEnabled(on, error);
				if (!ok)
					Logger::Error("[MicRelay] " + error);
				socket.Send(EncodeMessage({ MessageType::SetMicRelay, { ok, error } }));
			});
			break;
		}
		case MessageType::SetRunOnStartup: {
			const bool* enabled = msg->BoolAt(0);
			const bool ok = enabled && Startup::SetEnabled(*enabled);
			socket.Send(EncodeMessage({ MessageType::SetRunOnStartup, { ok } }));
			break;
		}
		case MessageType::SetDiscordPresence: {
			const bool* enabled = msg->BoolAt(0);
			if (enabled) {
				DiscordPresence::SetEnabled(*enabled);
				DiscordPresence::Save();
			}
			socket.Send(EncodeMessage({ MessageType::SetDiscordPresence, { enabled != nullptr } }));
			break;
		}
		case MessageType::ClearCache:
			library.Post([&socket] {
				const uint64_t freed = AppData::ClearCoverCache();
				AppData::RefreshMetadata();
				const nlohmann::json result = { { "freed", freed }, { "cache", AppData::CacheInfo() } };
				socket.Send(EncodeMessage({ MessageType::ClearCache, { true, result.dump() } }));
			});
			break;
		case MessageType::ClearLyrics:
			library.Post([&socket] {
				const size_t cleared = Lyrics::ClearAll();
				const nlohmann::json result = { { "cleared", cleared }, { "lyrics", AppData::LyricsInfo() } };
				socket.Send(EncodeMessage({ MessageType::ClearLyrics, { true, result.dump() } }));
			});
			break;
		case MessageType::ClearLogs: {
			const uint64_t freed = Logger::Clear();
			const Logger::LogStats stats = Logger::Stats();
			const nlohmann::json result = { { "freed", freed }, { "logs", { { "bytes", stats.Bytes }, { "files", stats.Files } } } };
			Logger::Info("[Logs] Cleared " + std::to_string(freed) + " bytes of logs");
			socket.Send(EncodeMessage({ MessageType::ClearLogs, { true, result.dump() } }));
			break;
		}
		default:
			Logger::Warning("[Message] Unknown type " + std::to_string(static_cast<int>(msg->type)));
		}
	});

	// Run() = keep alive
	socket.Run();
	Logger::Info("[Wave] Frontend disconnected, shutting down");
	library.Stop();
	covers.Stop();
	Queue::Shutdown();
	Player::OnStateChanged({});
	Visualizer::Stop();
	DiscordPresence::Shutdown();
	Normalize::Shutdown();
	Lyrics::Shutdown();
	Player::Shutdown();
	return 0;
}
