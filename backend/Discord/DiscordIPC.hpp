#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <nlohmann/json.hpp>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

constexpr uint32_t MAX_MESSAGE_SIZE = 16 * 1024 * 1024;

struct DiscordIPCMessage
{
	uint32_t Opcode;
	std::string Payload;
	bool bOk;

	nlohmann::json GetPayloadAsJson();

	DiscordIPCMessage() : Opcode(0), Payload(""), bOk(false) {}
	DiscordIPCMessage(uint32_t InOpcode, const std::string& InPayload) : Opcode(InOpcode), Payload(InPayload), bOk(true) {}
};

struct RichPresenceTimestamps {
    uint64_t start;
    uint64_t end;

    RichPresenceTimestamps()
        : start(0), end(0) {
    }

    RichPresenceTimestamps(uint64_t InStart, uint64_t InEnd)
        : start(InStart), end(InEnd) {
    }
};

struct RichPresenceEmoji {
    std::string name;
    std::string id;
    bool animated;

    RichPresenceEmoji()
        : name(""), id(""), animated(false) {
    }

    RichPresenceEmoji(
        const std::string& InName,
        const std::string& InId,
        bool InAnimated
    )
        : name(InName), id(InId), animated(InAnimated) {
    }
};

struct RichPresenceParty {
    std::string id;
    uint32_t current_size;
    uint32_t max_size;

    RichPresenceParty()
        : id(""), current_size(0), max_size(0) {
    }

    RichPresenceParty(
        const std::string& InId,
        uint32_t InCurrentSize,
        uint32_t InMaxSize
    )
        : id(InId), current_size(InCurrentSize), max_size(InMaxSize) {
    }
};

struct RichPresenceAssets {
    std::string large_image;
    std::string large_text;
    std::string large_url;
    std::string small_image;
    std::string small_text;
    std::string small_url;

    RichPresenceAssets()
        : large_image(""), large_text(""), large_url(""),
        small_image(""), small_text(""), small_url("") {
    }

    RichPresenceAssets(
        const std::string& InLargeImage,
        const std::string& InLargeText,
        const std::string& InLargeUrl,
        const std::string& InSmallImage,
        const std::string& InSmallText,
        const std::string& InSmallUrl
    )
        : large_image(InLargeImage),
        large_text(InLargeText),
        large_url(InLargeUrl),
        small_image(InSmallImage),
        small_text(InSmallText),
        small_url(InSmallUrl) {
    }
};

struct RichPresenceSecrets {
    std::string join;
    std::string spectate;
    std::string match;

    RichPresenceSecrets()
        : join(""), spectate(""), match("") {
    }

    RichPresenceSecrets(
        const std::string& InJoin,
        const std::string& InSpectate,
        const std::string& InMatch
    )
        : join(InJoin), spectate(InSpectate), match(InMatch) {
    }
};

struct RichPresenceButton {
    std::string label;
    std::string url;

    RichPresenceButton()
        : label(""), url("") {
    }

    RichPresenceButton(
        const std::string& InLabel,
        const std::string& InUrl
    )
        : label(InLabel), url(InUrl) {
    }
};

enum class RichPresenceActivityType : uint8_t {
	Playing = 0,
	Streaming = 1,
	Listening = 2,
	Watching = 3,
	Custom = 4,
	Competing = 5
};

struct RichPresence {
    std::string name;
    RichPresenceActivityType type;
    std::string url;
    uint64_t created_at;
    RichPresenceTimestamps timestamps;
    uint64_t application_id;
    int status_display_type;
    std::string details;
    std::string details_url;
    std::string state;
    std::string state_url;
    RichPresenceEmoji emoji;
    RichPresenceParty party;
    RichPresenceAssets assets;
    RichPresenceSecrets secrets;
    bool instance;
    uint32_t flags;
    std::vector<RichPresenceButton> buttons;

    RichPresence()
        : name(""), type(RichPresenceActivityType::Playing), url(""), created_at(0),
        timestamps(), application_id(0), status_display_type(0),
        details(""), details_url(""), state(""), state_url(""),
        emoji(), party(), assets(), secrets(),
        instance(false), flags(0), buttons() {
    }

    RichPresence(
        const std::string& InName,
        RichPresenceActivityType InType,
        const std::string& InUrl,
        uint64_t InCreatedAt,
        const RichPresenceTimestamps& InTimestamps,
        uint64_t InApplicationId,
        int InStatusDisplayType,
        const std::string& InDetails,
        const std::string& InDetailsUrl,
        const std::string& InState,
        const std::string& InStateUrl,
        const RichPresenceEmoji& InEmoji,
        const RichPresenceParty& InParty,
        const RichPresenceAssets& InAssets,
        const RichPresenceSecrets& InSecrets,
        bool InInstance,
        uint32_t InFlags,
        const std::vector<RichPresenceButton>& InButtons
    )
        : name(InName),
        type(InType),
        url(InUrl),
        created_at(InCreatedAt),
        timestamps(InTimestamps),
        application_id(InApplicationId),
        status_display_type(InStatusDisplayType),
        details(InDetails),
        details_url(InDetailsUrl),
        state(InState),
        state_url(InStateUrl),
        emoji(InEmoji),
        party(InParty),
        assets(InAssets),
        secrets(InSecrets),
        instance(InInstance),
        flags(InFlags),
        buttons(InButtons) {
    }
};

class DiscordIPC
{
private:
	static inline std::vector<HANDLE> Pipes;
	static inline std::recursive_mutex Mutex;
private:
	static HANDLE TryCreatePipe(int Index);
	static bool ReadExact(HANDLE pipe, void* data, DWORD size);
	static bool WriteExact(HANDLE pipe, const void* data, DWORD size);
	static bool WaitForBytes(HANDLE Pipe, DWORD Count, DWORD TimeoutMs);
	static DiscordIPCMessage ReadPacket(HANDLE Pipe, DWORD TimeoutMs = 3000);
	static bool WritePacket(HANDLE Pipe, const DiscordIPCMessage& Message);
	static bool SendMessageJson(HANDLE Pipe, int32_t Opcode, const nlohmann::json& Message);
	static void DrainResponses(HANDLE Pipe);
	static void SendToAll(const nlohmann::json& Payload);
	static nlohmann::json BuildActivity(const RichPresence& Presence);
	static std::string Nonce();
	static std::string StringField(const nlohmann::json& Object, const char* Key);
public:
	static bool Initialize(const std::string& ClientId);
	static void Shutdown();
	static bool IsConnected();
	static void SendRichPresence(const RichPresence& Presence);
	static void ClearRichPresence();
};

