#include "../Includes.hpp"

void IPCPipe::Start(std::string PipeName, int32_t SizeBits)
{
    pipe = CreateNamedPipeA(
        std::string("\\\\.\\pipe\\" + PipeName).c_str(),
        PIPE_ACCESS_OUTBOUND,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
        1, SizeBits, SizeBits, 0, NULL);


    if (pipe == INVALID_HANDLE_VALUE) {
        Logger::Error("[IPCPipe] Unable to create pipe");
        return;
    }

    BOOL connected =  ConnectNamedPipe(pipe, nullptr);

    if (!connected && GetLastError() != ERROR_PIPE_CONNECTED)
    {
        Logger::Error("[IPCPipe] Failed to connect");
        CloseHandle(pipe);
        return;
    }

    if (OnConnectionHandler) {
        std::string res = OnConnectionHandler();
        DWORD written = 0;

        bool success = WriteFile(
            pipe,
            res.data(),
            static_cast<DWORD>(res.size()),
            &written,
            nullptr
        );

        FlushFileBuffers(pipe);

        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }
}

void IPCPipe::onConnection(std::function<std::string()> Handler)
{
    OnConnectionHandler = Handler;
}
