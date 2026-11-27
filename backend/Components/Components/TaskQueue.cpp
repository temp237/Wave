#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <objbase.h>

#include <algorithm>
#include <atomic>
#include <vector>

#include "TaskQueue.hpp"
#include "Logger.hpp"

void TaskQueue::Start()
{
    if (Worker.joinable())
        return;
    bStopping = false;
    Worker = std::thread([this] { Loop(); });
}

void TaskQueue::Post(std::function<void()> Task)
{
    {
        std::lock_guard lock(Mutex);
        Tasks.push_back(std::move(Task));
    }
    Wake.notify_one();
}

void TaskQueue::Stop()
{
    {
        std::lock_guard lock(Mutex);
        bStopping = true;
        Tasks.clear();
    }
    Wake.notify_all();
    if (Worker.joinable())
        Worker.join();
}

void TaskQueue::ParallelFor(size_t Count, const std::function<void(size_t)>& Body, size_t MaxThreads)
{
    if (Count == 0)
        return;
    const size_t cores = std::max<size_t>(1, std::thread::hardware_concurrency());
    const size_t threads = std::min({ Count, cores, std::max<size_t>(1, MaxThreads) });

    std::atomic<size_t> next = 0;
    auto run = [&] {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        for (size_t i = next++; i < Count; i = next++) {
            try {
                Body(i);
            }
            catch (const std::exception& e) {
                Logger::Error(std::string("[TaskQueue] Parallel task failed: ") + e.what());
            }
        }
        if (SUCCEEDED(hr))
            CoUninitialize();
    };

    std::vector<std::thread> pool;
    pool.reserve(threads);
    for (size_t t = 0; t < threads; t++)
        pool.emplace_back(run);
    for (auto& thread : pool)
        thread.join();
}

void TaskQueue::Loop()
{
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock lock(Mutex);
            Wake.wait(lock, [this] { return bStopping || !Tasks.empty(); });
            if (bStopping)
                break;
            task = std::move(Tasks.front());
            Tasks.pop_front();
        }
        try {
            task();
        }
        catch (const std::exception& e) {
            Logger::Error(std::string("[TaskQueue] Task failed: ") + e.what());
        }
    }
    if (SUCCEEDED(hr))
        CoUninitialize();
}
