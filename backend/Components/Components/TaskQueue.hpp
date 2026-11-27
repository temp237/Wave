#pragma once
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

class TaskQueue
{
private:
	std::thread Worker;
	std::mutex Mutex;
	std::condition_variable Wake;
	std::deque<std::function<void()>> Tasks;
	bool bStopping = false;

	void Loop();

public:
	TaskQueue() = default;
	~TaskQueue() { Stop(); }
	TaskQueue(const TaskQueue&) = delete;
	TaskQueue& operator=(const TaskQueue&) = delete;

	void Start();
	void Post(std::function<void()> Task);
	void Stop();

	static void ParallelFor(size_t Count, const std::function<void(size_t)>& Body, size_t MaxThreads = 8);
};
