#include "TcpSocket.hpp"
#include "../Includes.hpp"

int32_t TcpSocket::GetPort()
{
	return Port;
}

void TcpSocket::onRecv(std::function<void(const std::string&)> Handler)
{
	_OnRecvHandler = Handler;
}

void TcpSocket::Start()
{
	WSAData wsaData;
	if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
		Status = ConnectionStatus::Failed;
		Logger::Error("[TcpSocket] WSAStartup failed");
		return;
	}

	listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (listenSock == INVALID_SOCKET) {
		Status = ConnectionStatus::Failed;
		Logger::Error("[TcpSocket] Socket creation failed");
		WSACleanup();
		return;
	}

	sockaddr_in serverAddr{};
	serverAddr.sin_family = AF_INET;
	serverAddr.sin_port = 0;
	inet_pton(AF_INET, "127.0.0.1", &serverAddr.sin_addr);

	if (bind(listenSock, (SOCKADDR*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
		Status = ConnectionStatus::Failed;
		Logger::Error("[TcpSocket] Socket binding failed");
		closesocket(listenSock);
		listenSock = INVALID_SOCKET;
		WSACleanup();
		return;
	}

	if (listen(listenSock, 1) == SOCKET_ERROR) {
		Status = ConnectionStatus::Failed;
		Logger::Error("[TcpSocket] Listen failed");
		closesocket(listenSock);
		listenSock = INVALID_SOCKET;
		WSACleanup();
		return;
	}

	int len = sizeof(serverAddr);
	getsockname(listenSock, (SOCKADDR*)&serverAddr, &len);
	Port = ntohs(serverAddr.sin_port);
	Status = ConnectionStatus::Listening;
}

bool TcpSocket::Accept()
{
	if (Status != ConnectionStatus::Listening)
		return false;

	sock = accept(listenSock, nullptr, nullptr);

	closesocket(listenSock);
	listenSock = INVALID_SOCKET;

	if (sock == INVALID_SOCKET) {
		Status = ConnectionStatus::Failed;
		Logger::Error("[TcpSocket] Accept failed");
		return false;
	}

	Status = ConnectionStatus::Connected;
	return true;
}

bool TcpSocket::Send(const std::string& data)
{
	if (!IsConnected())
		return false;

	// Frame: u32 little-endian length, then the payload
	uint32_t len = static_cast<uint32_t>(data.size());
	std::string frame(reinterpret_cast<const char*>(&len), 4);
	frame += data;

	std::lock_guard lock(SendMutex);
	size_t sent = 0;
	while (sent < frame.size()) {
		int n = send(sock, frame.data() + sent, static_cast<int>(frame.size() - sent), 0);
		if (n == SOCKET_ERROR)
			return false;
		sent += n;
	}
	return true;
}

void TcpSocket::Run()
{
	char buffer[4096];
	std::string pending;
	while (IsConnected()) {
		int n = recv(sock, buffer, sizeof(buffer), 0);
		if (n <= 0)
			break;
		pending.append(buffer, n);

		while (pending.size() >= 4) {
			uint32_t len;
			std::memcpy(&len, pending.data(), 4);
			if (len > 16 * 1024 * 1024) {
				Logger::Error("[TcpSocket] Frame too large, disconnecting");
				Status = ConnectionStatus::Disconnected;
				return;
			}
			if (pending.size() < 4 + size_t(len))
				break;
			std::string payload = pending.substr(4, len);
			pending.erase(0, 4 + size_t(len));
			if (_OnRecvHandler)
				_OnRecvHandler(payload);
		}
	}
	Status = ConnectionStatus::Disconnected;
}

void TcpSocket::End()
{
	if (sock == INVALID_SOCKET && listenSock == INVALID_SOCKET)
		return;
	if (sock != INVALID_SOCKET)
		closesocket(sock);
	if (listenSock != INVALID_SOCKET)
		closesocket(listenSock);
	WSACleanup();
	Status = ConnectionStatus::Disconnected;
	sock = INVALID_SOCKET;
	listenSock = INVALID_SOCKET;
}

bool TcpSocket::IsListening()
{
	return Status == ConnectionStatus::Listening;
}

bool TcpSocket::IsConnected()
{
	return Status == ConnectionStatus::Connected;
}
