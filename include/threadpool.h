//
// Created by X on 2025/9/16.
//

#ifndef LITECHAT_THREADPOOL_H
#define LITECHAT_THREADPOOL_H
#include <vector>
#include <thread>
#include <queue>
#include <functional>
#include <condition_variable>
#include <mutex>

class ThreadPool {
public:
    explicit ThreadPool(size_t n);

    ~ThreadPool();

    void enqueue(std::function<void()> task);

private:
    std::vector<std::thread> workers;
    std::queue<std::function<void()> > tasks;

    std::mutex mtx;
    std::condition_variable cv;
    bool stop;

    void worker_loop();
};

#include "threadpoll.tpp"

#endif //LITECHAT_THREADPOOL_H
