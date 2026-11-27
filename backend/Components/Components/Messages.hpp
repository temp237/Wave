#pragma once
#include "Bits.hpp"
#include <optional>
#include <string>
#include <variant>
#include <vector>

enum class MessageType : uint8_t {
	Debug = 0,
	ImportFile = 1,
	ImportFolder = 2,
	GetFileTree = 3,
	Play = 4,
	Pause = 5,
	Resume = 6,
	Seek = 7,
	SetVolume = 8,
	Stop = 9,
	PlayerState = 10,
	GetCover = 11,
	GetPlaylists = 12,
	CreatePlaylist = 13,
	DeletePlaylist = 14,
	RenamePlaylist = 15,
	AddToPlaylist = 16,
	RemoveFromPlaylist = 17,
	GetCovers = 18,
	QueueSet = 19,
	QueueNext = 20,
	QueuePrevious = 21,
	QueueJump = 22,
	QueueRemove = 23,
	QueuePlayNext = 24,
	QueueAdd = 25,
	QueueClear = 26,
	SetShuffle = 27,
	SetRepeat = 28,
	QueueState = 29,
	GetQueue = 30,
	GetSettings = 31,
	SetEqualizer = 32,
	ClearCache = 33,
	SetDiscordPresence = 34,
	RemoveFromLibrary = 35,
	SetInterface = 36,
	GetPlayerState = 37,
	SetRunOnStartup = 38,
	SetMicRelay = 39,
	ClearLogs = 40,
	SetDeEsser = 41,
	SetNormalize = 42,
	GetNormalize = 43,
	GetLyrics = 44,
	SetVisualizer = 45,
	Spectrum = 46,
	GetArtwork = 47,
	ClearLyrics = 48,
};

// Index = wire tag. Order must match the Value enum in frontend lib.rs.
using Value = std::variant<bool, uint8_t, int8_t, uint16_t, int16_t, uint32_t, int32_t, uint64_t, int64_t, float, double, std::string>;

constexpr uint32_t TAG_BITS = 4;

struct Message {
	MessageType type;
	std::vector<Value> values;

	const uint32_t* U32At(size_t i) const {
		return i < values.size() ? std::get_if<uint32_t>(&values[i]) : nullptr;
	}

	const bool* BoolAt(size_t i) const {
		return i < values.size() ? std::get_if<bool>(&values[i]) : nullptr;
	}

	const double* DoubleAt(size_t i) const {
		return i < values.size() ? std::get_if<double>(&values[i]) : nullptr;
	}

	std::vector<std::string> StringsFrom(size_t i) const {
		std::vector<std::string> out;
		for (; i < values.size(); i++) {
			if (const std::string* s = std::get_if<std::string>(&values[i]))
				out.push_back(*s);
		}
		return out;
	}

	const std::string* StringAt(size_t i) const {
		return i < values.size() ? std::get_if<std::string>(&values[i]) : nullptr;
	}
};

inline void WriteValue(BitWriter& w, const Value& value)
{
	w.WriteBits(value.index(), TAG_BITS);
	std::visit([&](const auto& v) {
		using T = std::decay_t<decltype(v)>;
		if constexpr (std::is_same_v<T, bool>) w.WriteBool(v);
		else if constexpr (std::is_same_v<T, float>) w.WriteF32(v);
		else if constexpr (std::is_same_v<T, double>) w.WriteF64(v);
		else if constexpr (std::is_same_v<T, std::string>) w.WriteString(v);
		else w.Write(v);
	}, value);
}

inline std::optional<Value> ReadValue(BitReader& r)
{
	Value v;
	switch (r.ReadBits(TAG_BITS)) {
	case 0: v = r.ReadBool(); break;
	case 1: v = r.Read<uint8_t>(); break;
	case 2: v = r.Read<int8_t>(); break;
	case 3: v = r.Read<uint16_t>(); break;
	case 4: v = r.Read<int16_t>(); break;
	case 5: v = r.Read<uint32_t>(); break;
	case 6: v = r.Read<int32_t>(); break;
	case 7: v = r.Read<uint64_t>(); break;
	case 8: v = r.Read<int64_t>(); break;
	case 9: v = r.ReadF32(); break;
	case 10: v = r.ReadF64(); break;
	case 11: v = r.ReadString(); break;
	default: return std::nullopt;
	}
	if (!r.Ok())
		return std::nullopt;
	return v;
}

inline std::string EncodeMessage(const Message& msg)
{
	BitWriter w;
	w.Write(static_cast<uint8_t>(msg.type));
	w.Write(static_cast<uint16_t>(msg.values.size()));
	for (const auto& v : msg.values)
		WriteValue(w, v);
	return std::string(w.Data().begin(), w.Data().end());
}

inline std::optional<Message> DecodeMessage(const std::string& payload)
{
	BitReader r(payload);
	Message msg;
	msg.type = static_cast<MessageType>(r.Read<uint8_t>());
	uint16_t count = r.Read<uint16_t>();
	if (!r.Ok())
		return std::nullopt;
	for (uint16_t i = 0; i < count; i++) {
		auto v = ReadValue(r);
		if (!v)
			return std::nullopt;
		msg.values.push_back(std::move(*v));
	}
	return msg;
}

inline std::string ValueToString(const Value& value)
{
	return std::visit([](const auto& v) -> std::string {
		using T = std::decay_t<decltype(v)>;
		if constexpr (std::is_same_v<T, bool>) return std::string("bool ") + (v ? "true" : "false");
		else if constexpr (std::is_same_v<T, std::string>) return "string \"" + v + "\"";
		else if constexpr (std::is_same_v<T, float>) return "f32 " + std::to_string(v);
		else if constexpr (std::is_same_v<T, double>) return "f64 " + std::to_string(v);
		else if constexpr (std::is_signed_v<T>) return "i" + std::to_string(sizeof(T) * 8) + " " + std::to_string(static_cast<int64_t>(v));
		else return "u" + std::to_string(sizeof(T) * 8) + " " + std::to_string(static_cast<uint64_t>(v));
	}, value);
}
