#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <mmdeviceapi.h>
#include <audioenginebaseapo.h>

#include <chrono>
#include <thread>

#include "MicRelay.hpp"
#include "Logger.hpp"
#include "Settings.hpp"
#include "Utils.hpp"

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

void MicRelay::Load()
{
	bEnabled = Settings::Bool(Settings::Get("microphone"), "relay", false);
	if (bEnabled && !IsInstalled())
		Logger::Warning("[MicRelay] Relay is on but the microphone effect isn't installed");
}

bool MicRelay::IsEnabled()
{
	return bEnabled;
}

std::filesystem::path MicRelay::ExeDirectory()
{
	wchar_t buffer[MAX_PATH * 4] = {};
	const DWORD length = GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
	if (length == 0 || length >= std::size(buffer))
		return {};
	return std::filesystem::path(buffer).parent_path();
}

std::filesystem::path MicRelay::InstalledDll()
{
	PWSTR programFiles = nullptr;
	std::filesystem::path path;
	if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramFiles, 0, nullptr, &programFiles)))
		path = std::filesystem::path(programFiles) / L"Wave" / DllName;
	CoTaskMemFree(programFiles);
	return path;
}

bool MicRelay::IsInstalled()
{
	HKEY key = nullptr;
	const std::wstring apoKey = std::wstring(L"SOFTWARE\\Classes\\AudioEngine\\AudioProcessingObjects\\") + ClsidString;
	if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, apoKey.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS)
		return false;
	RegCloseKey(key);

	DWORD version = 0;
	DWORD versionSize = sizeof(version);
	if (RegGetValueW(HKEY_LOCAL_MACHINE, StateRoot, L"InstallVersion", RRF_RT_REG_DWORD, nullptr, &version, &versionSize) != ERROR_SUCCESS || version != InstallVersion)
		return false;

	WIN32_FILE_ATTRIBUTE_DATA installed = {};
	if (!GetFileAttributesExW(InstalledDll().c_str(), GetFileExInfoStandard, &installed))
		return false;
	WIN32_FILE_ATTRIBUTE_DATA shipped = {};
	if (!GetFileAttributesExW((ExeDirectory() / DllName).c_str(), GetFileExInfoStandard, &shipped))
		return true;
	return installed.nFileSizeLow == shipped.nFileSizeLow && installed.nFileSizeHigh == shipped.nFileSizeHigh &&
		CompareFileTime(&installed.ftLastWriteTime, &shipped.ftLastWriteTime) == 0;
}

bool MicRelay::SetEnabled(bool bOn, std::string& Error)
{
	if (bOn && !IsInstalled()) {
		if (!std::filesystem::exists(ExeDirectory() / DllName)) {
			Error = "WaveMicApo.dll is missing next to WaveBackend.exe";
			return false;
		}
		Logger::Info("[MicRelay] Installing the microphone effect (asking for admin)");
		if (!RunElevated(InstallFlag, Error))
			return false;
		if (!IsInstalled()) {
			Error = "the microphone effect didn't install";
			return false;
		}
	}
	bEnabled = bOn;
	if (!bOn) {
		std::lock_guard lock(ViewMutex);
		ReleaseView();
	}
	Settings::Merge("microphone", { { "relay", bOn } });
	Logger::Info(bOn ? "[MicRelay] Relaying music to the microphone" : "[MicRelay] Stopped relaying music to the microphone");
	return true;
}

void MicRelay::SetGain(double Value)
{
	if (!std::isfinite(Value))
		Value = 1.0;
	Gain = static_cast<float>(std::clamp(Value, 0.0, 1.0));
}

nlohmann::json MicRelay::StateJson()
{
	return { { "relay", IsEnabled() }, { "installed", IsInstalled() } };
}

void MicRelay::ReleaseView()
{
	if (RelayBuffer::Header* view = View.exchange(nullptr))
		UnmapViewOfFile(view);
	if (Mapping)
		CloseHandle(Mapping);
	Mapping = nullptr;
}

RelayBuffer::Header* MicRelay::EnsureView()
{
	if (RelayBuffer::Header* view = View.load()) {
		if (RelayBuffer::IsValid(view))
			return view;
		Logger::Warning("[MicRelay] Dropped an outdated microphone effect buffer");
		ReleaseView();
	}
	const ULONGLONG now = GetTickCount64();
	if (now - LastOpenAttempt.load() < 1000)
		return nullptr;
	LastOpenAttempt = now;

	HANDLE mapping = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, RelayBuffer::MappingName);
	if (!mapping)
		return nullptr;
	auto* view = static_cast<RelayBuffer::Header*>(MapViewOfFile(mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, RelayBuffer::Size));
	if (!view) {
		CloseHandle(mapping);
		return nullptr;
	}
	Mapping = mapping;
	View = view;
	Logger::Info("[MicRelay] Connected to the microphone effect");
	return view;
}

void MicRelay::SetClock(double Seconds, bool bPlaying)
{
	if (!bEnabled || !std::isfinite(Seconds))
		return;
	std::lock_guard lock(ViewMutex);
	RelayBuffer::Header* view = EnsureView();
	if (!RelayBuffer::IsValid(view))
		return;
	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);
	RelayBuffer::WriteClock(view, static_cast<LONG64>((Seconds + LeadSeconds) * 10'000'000.0), now.QuadPart, bPlaying);
}

void MicRelay::Write(const float* Data, size_t Frames, uint32_t Channels, uint32_t SampleRate, LONGLONG SampleTime)
{
	if (!bEnabled || !Data || Frames == 0 || Channels == 0 || SampleRate == 0)
		return;
	std::lock_guard lock(ViewMutex);
	RelayBuffer::Header* view = EnsureView();
	if (!RelayBuffer::IsValid(view))
		return;

	const float gain = Gain.load();
	float* samples = RelayBuffer::Samples(view);
	if (view->SampleRate != static_cast<LONG>(SampleRate))
		InterlockedExchange(&view->SampleRate, static_cast<LONG>(SampleRate));

	const LONG64 start = view->WritePosition;
	if (SampleTime >= 0)
		RelayBuffer::WriteAnchor(view, start, SampleTime);
	for (size_t f = 0; f < Frames; f++) {
		const float left = Data[f * Channels];
		const float right = Channels > 1 ? Data[f * Channels + 1] : left;
		const uint32_t index = static_cast<uint32_t>(start + static_cast<LONG64>(f)) & RelayBuffer::Mask;
		samples[index * 2] = left * gain;
		samples[index * 2 + 1] = right * gain;
	}
	InterlockedExchange64(&view->WritePosition, start + static_cast<LONG64>(Frames));
	InterlockedExchange64(&view->LastWriteTick, static_cast<LONG64>(GetTickCount64()));
}

bool MicRelay::RunElevated(const wchar_t* Flag, std::string& Error)
{
	wchar_t exe[MAX_PATH * 4] = {};
	GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));

	SHELLEXECUTEINFOW info = { sizeof(info) };
	info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
	info.lpVerb = L"runas";
	info.lpFile = exe;
	info.lpParameters = Flag;
	info.nShow = SW_HIDE;
	if (!ShellExecuteExW(&info) || !info.hProcess) {
		const DWORD error = GetLastError();
		Error = error == ERROR_CANCELLED ? "admin permission was declined" : "couldn't start the installer (" + std::to_string(error) + ")";
		return false;
	}
	WaitForSingleObject(info.hProcess, 120000);
	DWORD code = 1;
	GetExitCodeProcess(info.hProcess, &code);
	CloseHandle(info.hProcess);
	if (code != 0) {
		Error = "the microphone effect installer failed (" + std::to_string(code) + ")";
		return false;
	}
	return true;
}

std::optional<int> MicRelay::HandleCommandLine()
{
	int count = 0;
	LPWSTR* args = CommandLineToArgvW(GetCommandLineW(), &count);
	if (!args)
		return std::nullopt;
	std::optional<int> result;
	for (int i = 1; i < count && !result; i++) {
		if (_wcsicmp(args[i], InstallFlag) == 0)
			result = Install();
		else if (_wcsicmp(args[i], UninstallFlag) == 0)
			result = Uninstall();
	}
	LocalFree(args);
	return result;
}

bool MicRelay::EnablePrivileges()
{
	HANDLE token = nullptr;
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
		return false;
	bool ok = true;
	for (const wchar_t* name : { SE_BACKUP_NAME, SE_RESTORE_NAME, SE_TAKE_OWNERSHIP_NAME }) {
		TOKEN_PRIVILEGES privileges = {};
		privileges.PrivilegeCount = 1;
		privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
		if (!LookupPrivilegeValueW(nullptr, name, &privileges.Privileges[0].Luid) ||
			!AdjustTokenPrivileges(token, FALSE, &privileges, 0, nullptr, nullptr) ||
			GetLastError() == ERROR_NOT_ALL_ASSIGNED)
			ok = false;
	}
	CloseHandle(token);
	return ok;
}

std::vector<std::wstring> MicRelay::CaptureEndpoints()
{
	std::vector<std::wstring> endpoints;
	IMMDeviceEnumerator* enumerator = nullptr;
	if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator))))
		return endpoints;
	IMMDeviceCollection* devices = nullptr;
	if (SUCCEEDED(enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &devices))) {
		UINT count = 0;
		devices->GetCount(&count);
		for (UINT i = 0; i < count; i++) {
			IMMDevice* device = nullptr;
			if (FAILED(devices->Item(i, &device)))
				continue;
			LPWSTR id = nullptr;
			if (SUCCEEDED(device->GetId(&id)) && id) {
				const std::wstring full(id);
				const size_t dot = full.rfind(L'.');
				if (dot != std::wstring::npos && dot + 1 < full.size())
					endpoints.push_back(full.substr(dot + 1));
				CoTaskMemFree(id);
			}
			device->Release();
		}
		devices->Release();
	}
	enumerator->Release();
	return endpoints;
}

bool MicRelay::RegisterApo(const std::filesystem::path& Dll)
{
	const std::wstring clsidKey = std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + ClsidString;
	const std::wstring serverKey = clsidKey + L"\\InprocServer32";
	const std::wstring apoKey = std::wstring(L"SOFTWARE\\Classes\\AudioEngine\\AudioProcessingObjects\\") + ClsidString;

	const auto setString = [](HKEY Key, const wchar_t* Name, const std::wstring& Value) {
		return RegSetValueExW(Key, Name, 0, REG_SZ, reinterpret_cast<const BYTE*>(Value.c_str()), static_cast<DWORD>((Value.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
	};
	const auto setDword = [](HKEY Key, const wchar_t* Name, DWORD Value) {
		return RegSetValueExW(Key, Name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&Value), sizeof(Value)) == ERROR_SUCCESS;
	};

	HKEY key = nullptr;
	bool ok = RegCreateKeyExW(HKEY_LOCAL_MACHINE, clsidKey.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) == ERROR_SUCCESS;
	if (ok) {
		ok = setString(key, nullptr, L"Wave Mic Relay");
		RegCloseKey(key);
	}
	if (ok && RegCreateKeyExW(HKEY_LOCAL_MACHINE, serverKey.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) == ERROR_SUCCESS) {
		ok = setString(key, nullptr, Dll.wstring()) && setString(key, L"ThreadingModel", L"Both");
		RegCloseKey(key);
	}
	else {
		ok = false;
	}
	if (ok && RegCreateKeyExW(HKEY_LOCAL_MACHINE, apoKey.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) == ERROR_SUCCESS) {
		wchar_t iid[64] = {};
		StringFromGUID2(__uuidof(IAudioProcessingObject), iid, static_cast<int>(std::size(iid)));
		ok = setString(key, L"FriendlyName", L"Wave Mic Relay") &&
			setString(key, L"Copyright", L"Wave") &&
			setDword(key, L"MajorVersion", 1) &&
			setDword(key, L"MinorVersion", 0) &&
			setDword(key, L"Flags", APO_FLAG_DEFAULT) &&
			setDword(key, L"MinInputConnections", 1) &&
			setDword(key, L"MaxInputConnections", 1) &&
			setDword(key, L"MinOutputConnections", 1) &&
			setDword(key, L"MaxOutputConnections", 1) &&
			setDword(key, L"MaxInstances", 0xFFFFFFFF) &&
			setDword(key, L"NumAPOInterfaces", 1) &&
			setString(key, L"APOInterface0", iid);
		RegCloseKey(key);
	}
	else {
		ok = false;
	}
	return ok;
}

void MicRelay::UnregisterApo()
{
	RegDeleteTreeW(HKEY_LOCAL_MACHINE, (std::wstring(L"SOFTWARE\\Classes\\AudioEngine\\AudioProcessingObjects\\") + ClsidString).c_str());
	RegDeleteTreeW(HKEY_LOCAL_MACHINE, (std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + ClsidString).c_str());
}

bool MicRelay::BackupValue(HKEY Source, const std::wstring& BackupKey, const std::wstring& Name)
{
	HKEY backup = nullptr;
	if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, BackupKey.c_str(), 0, nullptr, 0, KEY_READ | KEY_WRITE, nullptr, &backup, nullptr) != ERROR_SUCCESS)
		return false;
	if (RegQueryValueExW(backup, Name.c_str(), nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS) {
		RegCloseKey(backup);
		return true;
	}
	DWORD type = 0;
	DWORD size = 0;
	bool ok;
	if (RegQueryValueExW(Source, Name.c_str(), nullptr, &type, nullptr, &size) == ERROR_SUCCESS) {
		std::vector<BYTE> data(size);
		ok = RegQueryValueExW(Source, Name.c_str(), nullptr, &type, data.data(), &size) == ERROR_SUCCESS &&
			RegSetValueExW(backup, Name.c_str(), 0, type, data.data(), size) == ERROR_SUCCESS;
	}
	else {
		ok = RegSetValueExW(backup, Name.c_str(), 0, REG_NONE, nullptr, 0) == ERROR_SUCCESS;
	}
	RegCloseKey(backup);
	return ok;
}

bool MicRelay::PatchEndpoint(const std::wstring& Endpoint)
{
	const std::wstring backupKey = BackupRoot + Endpoint;
	const std::wstring clsid = ClsidString;
	const std::wstring endpointName = L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},7";
	const std::wstring endpointModesName = L"{d3993a3f-99c2-4402-b5ec-a92a0367664b},7";
	const std::vector<std::wstring> modes = {
		L"{C18E2F7E-933D-4965-B7D1-1EEF228D2AF3}",
		L"{9E90EA20-B493-4FD1-A1A8-7E1361A956CF}",
		L"{98951333-B9CD-48B1-A0A3-FF40682D73F7}",
		L"{FC1CFC9B-B9D6-4CFA-B5E0-4BB2166878B2}",
	};

	RestoreEndpoint(Endpoint);

	const auto setString = [](HKEY Key, const std::wstring& Name, const std::wstring& Value) {
		return RegSetValueExW(Key, Name.c_str(), 0, REG_SZ, reinterpret_cast<const BYTE*>(Value.c_str()), static_cast<DWORD>((Value.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
	};
	const auto setMulti = [](HKEY Key, const std::wstring& Name, const std::vector<std::wstring>& Values) {
		std::wstring multi;
		for (const std::wstring& value : Values) {
			multi += value;
			multi.push_back(L'\0');
		}
		multi.push_back(L'\0');
		return RegSetValueExW(Key, Name.c_str(), 0, REG_MULTI_SZ, reinterpret_cast<const BYTE*>(multi.data()), static_cast<DWORD>(multi.size() * sizeof(wchar_t))) == ERROR_SUCCESS;
	};
	const auto valueType = [](HKEY Key, const std::wstring& Name) -> DWORD {
		DWORD type = REG_NONE;
		return RegQueryValueExW(Key, Name.c_str(), nullptr, &type, nullptr, nullptr) == ERROR_SUCCESS ? type : REG_NONE;
	};
	const auto enableSysFx = [&](HKEY Key, const std::wstring& BackupPath) {
		const std::wstring sysFx = L"{1da5d803-d492-4edd-8c23-e0c0ffee7f0e},5";
		DWORD type = 0;
		DWORD value = 0;
		DWORD size = sizeof(value);
		if (RegQueryValueExW(Key, sysFx.c_str(), nullptr, &type, reinterpret_cast<BYTE*>(&value), &size) != ERROR_SUCCESS || type != REG_DWORD || value == 0)
			return true;
		const DWORD enabled = 0;
		return BackupValue(Key, BackupPath, sysFx) &&
			RegSetValueExW(Key, sysFx.c_str(), 0, REG_DWORD, reinterpret_cast<const BYTE*>(&enabled), sizeof(enabled)) == ERROR_SUCCESS;
	};

	HKEY fx = nullptr;
	const std::wstring fxPath = CaptureRoot + Endpoint + L"\\FxProperties";
	if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, fxPath.c_str(), 0, nullptr, REG_OPTION_BACKUP_RESTORE, KEY_ALL_ACCESS, nullptr, &fx, nullptr) != ERROR_SUCCESS)
		return false;

	const bool endpointIsMulti = valueType(fx, endpointName) == REG_MULTI_SZ;
	bool ok = BackupValue(fx, backupKey, endpointName) && (endpointIsMulti ? setMulti(fx, endpointName, { clsid }) : setString(fx, endpointName, clsid));
	ok = ok && BackupValue(fx, backupKey, endpointModesName) && setMulti(fx, endpointModesName, modes);
	ok = ok && enableSysFx(fx, backupKey);
	RegCloseKey(fx);

	HKEY props = nullptr;
	const std::wstring propsPath = CaptureRoot + Endpoint + L"\\Properties";
	if (ok && RegCreateKeyExW(HKEY_LOCAL_MACHINE, propsPath.c_str(), 0, nullptr, REG_OPTION_BACKUP_RESTORE, KEY_ALL_ACCESS, nullptr, &props, nullptr) == ERROR_SUCCESS) {
		ok = enableSysFx(props, backupKey + L"\\Properties");
		RegCloseKey(props);
	}
	return ok;
}

void MicRelay::RestoreEndpoint(const std::wstring& Endpoint)
{
	const auto restore = [](const std::wstring& BackupPath, const std::wstring& TargetPath) {
		HKEY backup = nullptr;
		if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, BackupPath.c_str(), 0, KEY_READ, &backup) != ERROR_SUCCESS)
			return;
		HKEY target = nullptr;
		if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, TargetPath.c_str(), 0, nullptr, REG_OPTION_BACKUP_RESTORE, KEY_ALL_ACCESS, nullptr, &target, nullptr) == ERROR_SUCCESS) {
			for (DWORD i = 0;; i++) {
				wchar_t name[256] = {};
				DWORD nameLength = static_cast<DWORD>(std::size(name));
				DWORD type = 0;
				DWORD size = 0;
				if (RegEnumValueW(backup, i, name, &nameLength, nullptr, &type, nullptr, &size) != ERROR_SUCCESS)
					break;
				if (type == REG_NONE) {
					RegDeleteValueW(target, name);
					continue;
				}
				std::vector<BYTE> data(size);
				nameLength = static_cast<DWORD>(std::size(name));
				if (RegEnumValueW(backup, i, name, &nameLength, nullptr, &type, data.data(), &size) == ERROR_SUCCESS)
					RegSetValueExW(target, name, 0, type, data.data(), size);
			}
			RegCloseKey(target);
		}
		RegCloseKey(backup);
	};
	const std::wstring backupKey = BackupRoot + Endpoint;
	restore(backupKey, CaptureRoot + Endpoint + L"\\FxProperties");
	restore(backupKey + L"\\Properties", CaptureRoot + Endpoint + L"\\Properties");
	RegDeleteTreeW(HKEY_LOCAL_MACHINE, backupKey.c_str());
}

std::vector<std::wstring> MicRelay::StopAudio()
{
	std::vector<std::wstring> stopped;
	SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
	if (!manager)
		return stopped;

	const auto stopOne = [&](const wchar_t* Name) {
		SC_HANDLE service = OpenServiceW(manager, Name, SERVICE_STOP | SERVICE_QUERY_STATUS);
		if (!service)
			return;
		SERVICE_STATUS status = {};
		if (ControlService(service, SERVICE_CONTROL_STOP, &status) || GetLastError() == ERROR_SERVICE_NOT_ACTIVE) {
			for (int i = 0; i < 100; i++) {
				if (!QueryServiceStatus(service, &status) || status.dwCurrentState == SERVICE_STOPPED)
					break;
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
			}
			stopped.push_back(Name);
		}
		CloseServiceHandle(service);
	};

	SC_HANDLE audio = OpenServiceW(manager, AudioService, SERVICE_ENUMERATE_DEPENDENTS);
	if (audio) {
		DWORD needed = 0;
		DWORD count = 0;
		EnumDependentServicesW(audio, SERVICE_ACTIVE, nullptr, 0, &needed, &count);
		if (needed > 0) {
			std::vector<BYTE> buffer(needed);
			auto* dependents = reinterpret_cast<ENUM_SERVICE_STATUSW*>(buffer.data());
			if (EnumDependentServicesW(audio, SERVICE_ACTIVE, dependents, needed, &needed, &count)) {
				for (DWORD i = 0; i < count; i++)
					stopOne(dependents[i].lpServiceName);
			}
		}
		CloseServiceHandle(audio);
	}
	stopOne(AudioService);
	CloseServiceHandle(manager);
	return stopped;
}

void MicRelay::StartAudio(const std::vector<std::wstring>& Services)
{
	SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
	if (!manager)
		return;
	const auto startOne = [&](const std::wstring& Name) {
		SC_HANDLE service = OpenServiceW(manager, Name.c_str(), SERVICE_START);
		if (service) {
			StartServiceW(service, 0, nullptr);
			CloseServiceHandle(service);
		}
	};
	startOne(AudioService);
	for (const std::wstring& name : Services) {
		if (_wcsicmp(name.c_str(), AudioService) != 0)
			startOne(name);
	}
	CloseServiceHandle(manager);
}

int MicRelay::Install()
{
	Logger::Info("[MicRelay] Installer started");
	const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	if (!EnablePrivileges())
		Logger::Warning("[MicRelay] Couldn't enable registry restore privileges");

	const std::filesystem::path source = ExeDirectory() / DllName;
	const std::filesystem::path target = InstalledDll();
	if (target.empty() || !std::filesystem::exists(source)) {
		Logger::Error("[MicRelay] " + Utils::ToUtf8(source) + " is missing");
		if (SUCCEEDED(com))
			CoUninitialize();
		return 2;
	}

	const std::vector<std::wstring> endpoints = CaptureEndpoints();
	const std::vector<std::wstring> stopped = StopAudio();

	std::error_code ec;
	std::filesystem::create_directories(target.parent_path(), ec);
	const bool copied = CopyFileW(source.c_str(), target.c_str(), FALSE) != FALSE;
	const DWORD copyError = copied ? 0 : GetLastError();
	bool ok = copied && RegisterApo(target);
	size_t patched = 0;
	for (const std::wstring& endpoint : endpoints) {
		if (ok && PatchEndpoint(endpoint))
			patched++;
	}

	StartAudio(stopped);
	if (SUCCEEDED(com))
		CoUninitialize();

	if (ok && patched > 0) {
		const DWORD version = InstallVersion;
		RegSetKeyValueW(HKEY_LOCAL_MACHINE, StateRoot, L"InstallVersion", REG_DWORD, &version, sizeof(version));
	}

	if (!copied)
		Logger::Error("[MicRelay] Couldn't copy the microphone effect (" + std::to_string(copyError) + ")");
	Logger::Info("[MicRelay] Attached to " + std::to_string(patched) + " of " + std::to_string(endpoints.size()) + " microphones");
	return ok && patched > 0 ? 0 : 3;
}

int MicRelay::Uninstall()
{
	Logger::Info("[MicRelay] Uninstaller started");
	EnablePrivileges();

	std::vector<std::wstring> endpoints;
	HKEY root = nullptr;
	if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, BackupRoot, 0, KEY_READ, &root) == ERROR_SUCCESS) {
		for (DWORD i = 0;; i++) {
			wchar_t name[256] = {};
			DWORD length = static_cast<DWORD>(std::size(name));
			if (RegEnumKeyExW(root, i, name, &length, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
				break;
			endpoints.emplace_back(name);
		}
		RegCloseKey(root);
	}

	const std::vector<std::wstring> stopped = StopAudio();
	for (const std::wstring& endpoint : endpoints)
		RestoreEndpoint(endpoint);
	UnregisterApo();
	DeleteFileW(InstalledDll().c_str());
	RegDeleteTreeW(HKEY_LOCAL_MACHINE, StateRoot);
	StartAudio(stopped);
	Logger::Info("[MicRelay] Removed from " + std::to_string(endpoints.size()) + " microphones");
	return 0;
}
