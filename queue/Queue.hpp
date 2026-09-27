#pragma once
#include "Msg.hpp"
#include <memory>
#include <chrono>
#include <condition_variable>
#include <queue>
#include <set>
#include <mutex>
#include <utility>

/**
 * Queue is a thread-safe message queue.
 * One-way messaging only: producers put(), consumers get()/tryGet().
 */
class Queue{

private:
    std::queue<std::unique_ptr<Msg>> queue_;
    // Dedup by Msg *id* (the producer passes the frame PTS), not by MsgUID -
    // the point is "never queue the same frame twice". It was typed MsgUID,
    // which described the wrong thing.
    std::set<uint64_t> ids_;
    std::mutex queueMutex_;
    std::condition_variable queueCond_;
    int size_;

public:
    Queue(int size = 0): queue_(), queueMutex_(), queueCond_(), size_(size){}

    ~Queue();

    void put(Msg&& msg);
    std::unique_ptr<Msg> get(int timeoutMillis = 0);
    std::unique_ptr<Msg> tryGet();
    size_t size();
 
};
