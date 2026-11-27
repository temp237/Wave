#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>

#include <cstdio>
#include <fstream>
#include <vector>

#include "ModelStore.hpp"
#include "Logger.hpp"
#include "Utils.hpp"

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")

bool ModelStore::IsPresent(const std::filesystem::path& Path, uint64_t Size)
{
	std::error_code ec;
	return std::filesystem::is_regular_file(Path, ec) && std::filesystem::file_size(Path, ec) == Size && !ec;
}

std::string ModelStore::Sha256(const std::filesystem::path& Path)
{
	BCRYPT_ALG_HANDLE algorithm = nullptr;
	BCRYPT_HASH_HANDLE hash = nullptr;
	std::string hex;
	if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0)
		return hex;
	if (BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
		std::ifstream file(Path, std::ios::binary);
		std::vector<char> buffer(1 << 20);
		bool ok = static_cast<bool>(file);
		while (ok && file) {
			file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
			const std::streamsize read = file.gcount();
			if (read > 0 && BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer.data()), static_cast<ULONG>(read), 0) != 0)
				ok = false;
		}
		UCHAR digest[32] = {};
		if (ok && BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0) {
			char part[3];
			for (const UCHAR byte : digest) {
				std::snprintf(part, sizeof(part), "%02x", byte);
				hex += part;
			}
		}
		BCryptDestroyHash(hash);
	}
	BCryptCloseAlgorithmProvider(algorithm, 0);
	return hex;
}

bool ModelStore::Download(const Model& Item, const std::filesystem::path& Target, const std::atomic<bool>& Cancel, std::string& Error)
{
	URL_COMPONENTS parts{ sizeof(parts) };
	wchar_t host[256] = {};
	wchar_t path[2048] = {};
	parts.lpszHostName = host;
	parts.dwHostNameLength = static_cast<DWORD>(std::size(host));
	parts.lpszUrlPath = path;
	parts.dwUrlPathLength = static_cast<DWORD>(std::size(path));
	if (!WinHttpCrackUrl(Item.Url, 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS) {
		Error = "bad model address";
		return false;
	}

	HINTERNET session = WinHttpOpen(L"Wave", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
	HINTERNET connection = session ? WinHttpConnect(session, host, parts.nPort, 0) : nullptr;
	HINTERNET request = connection ? WinHttpOpenRequest(connection, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
	const auto close = [&] {
		if (request) WinHttpCloseHandle(request);
		if (connection) WinHttpCloseHandle(connection);
		if (session) WinHttpCloseHandle(session);
	};

	DWORD status = 0;
	DWORD statusSize = sizeof(status);
	if (!request || !WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
		!WinHttpReceiveResponse(request, nullptr) ||
		!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX)) {
		Error = "couldn't reach the download server (" + std::to_string(GetLastError()) + ")";
		close();
		return false;
	}
	if (status != 200) {
		Error = "the download server answered " + std::to_string(status);
		close();
		return false;
	}

	std::filesystem::path partial = Target;
	partial += PartSuffix;
	std::ofstream file(partial, std::ios::binary | std::ios::trunc);
	if (!file) {
		Error = "couldn't write " + Utils::ToUtf8(partial);
		close();
		return false;
	}

	std::vector<char> buffer(1 << 20);
	uint64_t received = 0;
	int reported = 0;
	bool ok = true;
	while (!Cancel) {
		DWORD read = 0;
		if (!WinHttpReadData(request, buffer.data(), static_cast<DWORD>(buffer.size()), &read)) {
			Error = "the download was interrupted (" + std::to_string(GetLastError()) + ")";
			ok = false;
			break;
		}
		if (read == 0)
			break;
		file.write(buffer.data(), read);
		received += read;
		if (received > Item.Size) {
			Error = "the download is larger than expected";
			ok = false;
			break;
		}
		const int percent = static_cast<int>(received * 100 / Item.Size);
		if (percent >= reported + ProgressStep) {
			reported = percent - percent % ProgressStep;
			Logger::Info("[Models] " + Utils::ToUtf8(std::filesystem::path(Item.File)) + ": " + std::to_string(reported) + "%");
		}
	}
	file.close();
	close();

	std::error_code ec;
	if (Cancel) {
		Error = "cancelled";
		ok = false;
	}
	else if (ok && received != Item.Size) {
		Error = "the download is incomplete (" + std::to_string(received) + " of " + std::to_string(Item.Size) + " bytes)";
		ok = false;
	}
	else if (ok && Sha256(partial) != Item.Sha256) {
		Error = "the download doesn't match its checksum";
		ok = false;
	}
	if (!ok) {
		std::filesystem::remove(partial, ec);
		return false;
	}
	std::filesystem::rename(partial, Target, ec);
	if (ec) {
		Error = "couldn't move the model into place: " + ec.message();
		std::filesystem::remove(partial, ec);
		return false;
	}
	return true;
}

bool ModelStore::Ensure(const std::filesystem::path& Directory, const std::atomic<bool>& Cancel, std::string& Error)
{
	std::error_code ec;
	std::filesystem::create_directories(Directory, ec);
	for (const Model& item : Required) {
		const std::filesystem::path target = Directory / item.File;
		if (IsPresent(target, item.Size))
			continue;
		const std::string name = Utils::ToUtf8(std::filesystem::path(item.File));
		Logger::Info("[Models] Downloading " + name + " (" + std::to_string(item.Size / (1024 * 1024)) + " MB)");
		if (!Download(item, target, Cancel, Error)) {
			Error = name + ": " + Error;
			return false;
		}
		Logger::Success("[Models] " + name + " downloaded and verified");
	}
	return true;
}
