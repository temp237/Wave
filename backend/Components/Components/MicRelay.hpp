#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "../../../MicRelay/RelayBuffer.hpp"

class MicRelay
{
public:
	MicRelay() = delete;

	static void Load();
	static bool IsEnabled();
	static bool IsInstalled();
	static bool SetEnabled(bool bEnabled, std::string& Error);
	static void SetGain(double Gain);
	static void Write(const float* Data, size_t Frames, uint32_t Channels, uint32_t SampleRate, LONGLONG SampleTime);
	static void SetClock(double Seconds, bool bPlaying);
	static nlohmann::json StateJson();
	static std::optional<int> HandleCommandLine();

private:
	static constexpr const wchar_t* ClsidString = L"{BB0FD71A-4C49-4D75-B4E8-49134B799F08}";
	static constexpr const wchar_t* InstallFlag = L"--mic-relay-install";
	static constexpr const wchar_t* UninstallFlag = L"--mic-relay-uninstall";
	static constexpr const wchar_t* DllName = L"WaveMicApo.dll";
	static constexpr const wchar_t* CaptureRoot = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Capture\\";
	static constexpr const wchar_t* BackupRoot = L"SOFTWARE\\Wave\\MicRelay\\Endpoints\\";
	static constexpr const wchar_t* AudioService = L"Audiosrv";
	static constexpr const wchar_t* StateRoot = L"SOFTWARE\\Wave\\MicRelay";
	static constexpr DWORD InstallVersion = 3;

	static RelayBuffer::Header* EnsureView();
	static void ReleaseView();
	static std::filesystem::path ExeDirectory();
	static std::filesystem::path InstalledDll();
	static bool RunElevated(const wchar_t* Flag, std::string& Error);

	static int Install();
	static int Uninstall();
	static bool EnablePrivileges();
	static std::vector<std::wstring> CaptureEndpoints();
	static bool RegisterApo(const std::filesystem::path& Dll);
	static void UnregisterApo();
	static bool PatchEndpoint(const std::wstring& Endpoint);
	static void RestoreEndpoint(const std::wstring& Endpoint);
	static bool BackupValue(HKEY Source, const std::wstring& BackupKey, const std::wstring& Name);
	static std::vector<std::wstring> StopAudio();
	static void StartAudio(const std::vector<std::wstring>& Services);

	static inline std::atomic<bool> bEnabled = false;
	static constexpr double LeadSeconds = 0.2;
	static inline std::atomic<float> Gain = 0.8f;
	static inline std::atomic<RelayBuffer::Header*> View = nullptr;
	static inline HANDLE Mapping = nullptr;
	static inline std::mutex ViewMutex;
	static inline std::atomic<ULONGLONG> LastOpenAttempt = 0;
};
