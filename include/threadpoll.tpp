//
// Created by X on 2025/9/16.
//

#ifndef LITECHAT_THREADPOLL_TPP
#define LITECHAT_THREADPOLL_TPP
#include <iostream>

#include "threadpool.h"

inline ThreadPool::ThreadPool(size_t n) : stop(false)
{
    for (size_t i = 0; i < n; i++)
    {
        workers.emplace_back([this]() { worker_loop(); });
    }
}

inline ThreadPool::~ThreadPool()
{
    {
        std::unique_lock<std::mutex> lock(mtx);
        stop = true;
    }

    cv.notify_all();

    for (auto& worker : workers)
    {
        if (worker.joinable())
        {
            worker.join();
        }
    }
}

inline void ThreadPool::enqueue(std::function<void()> task)
{
    {
        std::unique_lock<std::mutex> lock(mtx);
        tasks.push(std::move(task));
    }

    cv.notify_one();
}

inline void ThreadPool::worker_loop()
{
    while (true)
    {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(mtx);
            cv.wait(lock, [this]() { return stop || !tasks.empty(); });

            if (stop && tasks.empty())
            {
                return;
            }

            task = std::move(tasks.front());
            tasks.pop();
        }

        try
        {
            task();
        }
        catch (const std::exception& e)
        {
            std::cerr << "ThreadPool caught exception: " << e.what() << "\n";
        }
    }
}

#endif  // LITECHAT_THREADPOLL_TPP