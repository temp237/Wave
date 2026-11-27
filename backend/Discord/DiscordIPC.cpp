#include "DiscordIPC.hpp"
#include "../Components/Components/Logger.hpp"

#include <algorithm>
#include <chrono>

nlohmann::json DiscordIPCMessage::GetPayloadAsJson()
{
    if (!bOk)
        return nlohmann::json();
    nlohmann::json j = nlohmann::json::parse(Payload, nullptr, false);
    return j.is_discarded() ? nlohmann::json() : j;
}

HANDLE DiscordIPC::TryCreatePipe(int Index)
{
    const std::string pipeName = "\\\\?\\pipe\\discord-ipc-" + std::to_string(Index);
    return CreateFileA(pipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
}

bool DiscordIPC::ReadExact(HANDLE pipe, void* data, DWORD size)
{
    auto* ptr = static_cast<char*>(data);
    DWORD totalRead = 0;
    while (totalRead < size) {
        DWORD bytesRead = 0;
        if (!ReadFile(pipe, ptr + totalRead, size - totalRead, &bytesRead, nullptr) || bytesRead == 0)
            return false;
        totalRead += bytesRead;
    }
    return true;
}

bool DiscordIPC::WriteExact(HANDLE pipe, const void* data, DWORD size)
{
    const auto* ptr = static_cast<const char*>(data);
    DWORD totalWritten = 0;
    while (totalWritten < size) {
        DWORD bytesWritten = 0;
        if (!WriteFile(pipe, ptr + totalWritten, size - totalWritten, &bytesWritten, nullptr) || bytesWritten == 0)
            return false;
        totalWritten += bytesWritten;
    }
    return true;
}

bool DiscordIPC::WaitForBytes(HANDLE Pipe, DWORD Count, DWORD TimeoutMs)
{
    const ULONGLONG deadline = GetTickCount64() + TimeoutMs;
    for (;;) {
        DWORD available = 0;
        if (!PeekNamedPipe(Pipe, nullptr, 0, nullptr, &available, nullptr))
            return false;
        if (available >= Count)
            return true;
        if (GetTickCount64() >= deadline)
            return false;
        Sleep(10);
    }
}

DiscordIPCMessage DiscordIPC::ReadPacket(HANDLE Pipe, DWORD TimeoutMs)
{
    uint32_t header[2]{};
    if (!WaitForBytes(Pipe, sizeof(header), TimeoutMs) || !ReadExact(Pipe, header, sizeof(header)))
        return DiscordIPCMessage();

    DiscordIPCMessage message;
    message.Opcode = header[0];
    const uint32_t length = header[1];
    if (length > MAX_MESSAGE_SIZE) {
        Logger::Error("[DiscordIPC] IPC message exceeds maximum size.");
        return message;
    }

    message.Payload.resize(length);
    if (length > 0 && (!WaitForBytes(Pipe, (std::min)(length, uint32_t(4096)), TimeoutMs) || !ReadExact(Pipe, message.Payload.data(), length))) {
        message.Payload.clear();
        return message;
    }
    message.bOk = true;
    return message;
}

bool DiscordIPC::WritePacket(HANDLE Pipe, const DiscordIPCMessage& Message)
{
    if (Message.Payload.size() > MAX_MESSAGE_SIZE) {
        Logger::Error("[DiscordIPC] IPC message exceeds maximum size.");
        return false;
    }
    const uint32_t header[2] = { Message.Opcode, static_cast<uint32_t>(Message.Payload.size()) };
    if (!WriteExact(Pipe, header, sizeof(header)))
        return false;
    return Message.Payload.empty() || WriteExact(Pipe, Message.Payload.data(), static_cast<DWORD>(Message.Payload.size()));
}

bool DiscordIPC::SendMessageJson(HANDLE Pipe, int32_t Opcode, const nlohmann::json& Message)
{
    return WritePacket(Pipe, DiscordIPCMessage(static_cast<uint32_t>(Opcode), Message.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace)));
}

void DiscordIPC::DrainResponses(HANDLE Pipe)
{
    for (;;) {
        DWORD available = 0;
        if (!PeekNamedPipe(Pipe, nullptr, 0, nullptr, &available, nullptr) || available < 8)
            return;
        DiscordIPCMessage response = ReadPacket(Pipe);
        if (!response.bOk)
            return;
        const nlohmann::json j = response.GetPayloadAsJson();
        if (StringField(j, "evt") == "ERROR") {
            const std::string reason = j.contains("data") ? StringField(j["data"], "message") : std::string();
            Logger::Warning("[DiscordIPC] Discord rejected an update: " + (reason.empty() ? std::string("unknown error") : reason));
        }
    }
}

std::string DiscordIPC::StringField(const nlohmann::json& Object, const char* Key)
{
    if (!Object.is_object())
        return {};
    const auto it = Object.find(Key);
    return it != Object.end() && it->is_string() ? it->get<std::string>() : std::string();
}

std::string DiscordIPC::Nonce()
{
    return std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

nlohmann::json DiscordIPC::BuildActivity(const RichPresence& Presence)
{
    nlohmann::json Activity = {
        {"type", Presence.type}
    };

    if (!Presence.name.empty())
        Activity["name"] = Presence.name;

    if (!Presence.url.empty())
        Activity["url"] = Presence.url;

    if (!Presence.details.empty())
        Activity["details"] = Presence.details;

    if (!Presence.details_url.empty())
        Activity["details_url"] = Presence.details_url;

    if (!Presence.state.empty())
        Activity["state"] = Presence.state;

    if (!Presence.state_url.empty())
        Activity["state_url"] = Presence.state_url;

    if (Presence.created_at != 0)
        Activity["created_at"] = Presence.created_at;

    if (Presence.application_id != 0)
        Activity["application_id"] = std::to_string(Presence.application_id);

    if (Presence.status_display_type != 0)
        Activity["status_display_type"] = Presence.status_display_type;

    // Timestamps
    if (Presence.timestamps.start != 0 ||
        Presence.timestamps.end != 0)
    {
        nlohmann::json Timestamps = nlohmann::json::object();

        if (Presence.timestamps.start != 0)
            Timestamps["start"] = Presence.timestamps.start;

        if (Presence.timestamps.end != 0)
            Timestamps["end"] = Presence.timestamps.end;

        Activity["timestamps"] = Timestamps;
    }

    // Assets
    const auto& Assets = Presence.assets;

    if (!Assets.large_image.empty() ||
        !Assets.large_text.empty() ||
        !Assets.large_url.empty() ||
        !Assets.small_image.empty() ||
        !Assets.small_text.empty() ||
        !Assets.small_url.empty())
    {
        nlohmann::json AssetsJson = nlohmann::json::object();

        if (!Assets.large_image.empty())
            AssetsJson["large_image"] = Assets.large_image;

        if (!Assets.large_text.empty())
            AssetsJson["large_text"] = Assets.large_text;

        if (!Assets.large_url.empty())
            AssetsJson["large_url"] = Assets.large_url;

        if (!Assets.small_image.empty())
            AssetsJson["small_image"] = Assets.small_image;

        if (!Assets.small_text.empty())
            AssetsJson["small_text"] = Assets.small_text;

        if (!Assets.small_url.empty())
            AssetsJson["small_url"] = Assets.small_url;

        Activity["assets"] = AssetsJson;
    }

    // Emoji
    const auto& Emoji = Presence.emoji;

    if (!Emoji.name.empty() || !Emoji.id.empty())
    {
        nlohmann::json EmojiJson = nlohmann::json::object();

        if (!Emoji.name.empty())
            EmojiJson["name"] = Emoji.name;

        if (!Emoji.id.empty())
            EmojiJson["id"] = Emoji.id;

        if (Emoji.animated)
            EmojiJson["animated"] = true;

        Activity["emoji"] = EmojiJson;
    }

    // Party
    const auto& Party = Presence.party;

    if (!Party.id.empty() ||
        Party.current_size != 0 ||
        Party.max_size != 0)
    {
        nlohmann::json PartyJson = nlohmann::json::object();

        if (!Party.id.empty())
            PartyJson["id"] = Party.id;

        if (Party.current_size != 0 || Party.max_size != 0)
        {
            PartyJson["size"] = {
                Party.current_size,
                Party.max_size
            };
        }

        Activity["party"] = PartyJson;
    }

    // Secrets
    const auto& Secrets = Presence.secrets;

    if (!Secrets.join.empty() ||
        !Secrets.spectate.empty() ||
        !Secrets.match.empty())
    {
        nlohmann::json SecretsJson = nlohmann::json::object();

        if (!Secrets.join.empty())
            SecretsJson["join"] = Secrets.join;

        if (!Secrets.spectate.empty())
            SecretsJson["spectate"] = Secrets.spectate;

        if (!Secrets.match.empty())
            SecretsJson["match"] = Secrets.match;

        Activity["secrets"] = SecretsJson;
    }

    // Instance
    if (Presence.instance)
        Activity["instance"] = true;

    // Flags
    if (Presence.flags != 0)
        Activity["flags"] = Presence.flags;

    // Buttons (Discord allows a maximum of 2)
    if (!Presence.buttons.empty())
    {
        Activity["buttons"] = nlohmann::json::array();

        const size_t ButtonCount =
            (std::min)(Presence.buttons.size(), size_t(2));

        for (size_t i = 0; i < ButtonCount; ++i)
        {
            const auto& Button = Presence.buttons[i];

            Activity["buttons"].push_back({
                {"label", Button.label},
                {"url", Button.url}
                });
        }
    }

    return Activity;
}

bool DiscordIPC::Initialize(const std::string& ClientId)
{
    std::lock_guard lock(Mutex);
    for (HANDLE pipe : Pipes)
        CloseHandle(pipe);
    Pipes.clear();

    for (int index = 0; index < 10; index++) {
        HANDLE pipe = TryCreatePipe(index);
        if (pipe == INVALID_HANDLE_VALUE)
            continue;

        nlohmann::json handshake;
        handshake["v"] = 1;
        handshake["client_id"] = ClientId;
        DiscordIPCMessage response;
        if (SendMessageJson(pipe, 0, handshake))
            response = ReadPacket(pipe);

        const nlohmann::json j = response.GetPayloadAsJson();
        if (response.bOk && response.Opcode == 1 && StringField(j, "evt") == "READY") {
            Pipes.push_back(pipe);
            continue;
        }
        Logger::Warning("[DiscordIPC] Pipe " + std::to_string(index) + " didn't complete the handshake");
        CloseHandle(pipe);
    }

    if (!Pipes.empty())
        Logger::Success("[DiscordIPC] Connected to Discord (" + std::to_string(Pipes.size()) + " pipe" + (Pipes.size() == 1 ? ")" : "s)"));
    return !Pipes.empty();
}

void DiscordIPC::Shutdown()
{
    std::lock_guard lock(Mutex);
    ClearRichPresence();
    for (HANDLE pipe : Pipes)
        CloseHandle(pipe);
    Pipes.clear();
}

bool DiscordIPC::IsConnected()
{
    std::lock_guard lock(Mutex);
    return !Pipes.empty();
}

void DiscordIPC::SendToAll(const nlohmann::json& Payload)
{
    std::lock_guard lock(Mutex);
    for (auto it = Pipes.begin(); it != Pipes.end();) {
        if (SendMessageJson(*it, 1, Payload)) {
            DrainResponses(*it);
            ++it;
        }
        else {
            Logger::Warning("[DiscordIPC] Lost the connection to Discord");
            CloseHandle(*it);
            it = Pipes.erase(it);
        }
    }
}

void DiscordIPC::SendRichPresence(const RichPresence& Presence)
{
    nlohmann::json payload;
    payload["cmd"] = "SET_ACTIVITY";
    payload["args"] = { { "activity", BuildActivity(Presence) }, { "pid", GetCurrentProcessId() } };
    payload["nonce"] = Nonce();
    SendToAll(payload);
}

void DiscordIPC::ClearRichPresence()
{
    nlohmann::json payload;
    payload["cmd"] = "SET_ACTIVITY";
    payload["args"] = { { "activity", nullptr }, { "pid", GetCurrentProcessId() } };
    payload["nonce"] = Nonce();
    SendToAll(payload);
}
