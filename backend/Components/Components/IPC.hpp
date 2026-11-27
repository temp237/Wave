#pragma once
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <string>
#include <functional>

class IPCPipe
{
private:
	HANDLE pipe;
	std::function<std::string()> OnConnectionHandler;
public:
	IPCPipe() : pipe(nullptr) {}
public:
	void Start(std::string PipeName, int32_t SizeBits = 65536);
	void onConnection(std::function<std::string()> Handler);
};

