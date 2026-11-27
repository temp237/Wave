#pragma once
#define WIN32_LEAN_AND_MEAN
#include <cstring>
#include <string>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <functional>
#include <mutex>

#pragma comment(lib, "Ws2_32.lib")

enum class ConnectionStatus : uint32_t {
	Disconnected,
	Failed,
	Listening, // bound to a port, waiting for the frontend
	Connected  // frontend connected
};

class TcpSocket
{
private:
	SOCKET listenSock; // server socket that waits for connections
	SOCKET sock;       // the connection to the frontend
	ConnectionStatus Status;
	int32_t Port;
	std::function<void(const std::string&)> _OnRecvHandler;
	std::mutex SendMutex;
public:
	TcpSocket() : listenSock(INVALID_SOCKET), sock(INVALID_SOCKET), Status(ConnectionStatus::Disconnected), Port(-1), _OnRecvHandler({}) {}
	~TcpSocket() { End(); }
public:
	int32_t GetPort();
	void onRecv(std::function<void(const std::string&)> Handler);
	void Start();   // bind to a free localhost port and listen (doesn't block)
	bool Accept();  // block until the frontend connects
	bool Send(const std::string& data);
	void Run();     // block, receiving data until the frontend disconnects
	void End();
	bool IsListening();
	bool IsConnected();
};
