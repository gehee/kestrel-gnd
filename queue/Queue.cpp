#include "Queue.hpp"

Queue::~Queue() {}


void Queue::put(Msg&& msg)
{
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        if (ids_.find(msg.getMsgId()) != ids_.end()){
            return;
        }
        ids_.insert(msg.getMsgId());
        queue_.push(msg.move());
        if (size_ > 0 && queue_.size() > (size_t)size_) {
            if (queue_.front()) ids_.erase(queue_.front()->getMsgId());
            queue_.pop();
        }
    }

    queueCond_.notify_one();
}

std::unique_ptr<Msg> Queue::get(int timeoutMillis)
{
    std::unique_lock<std::mutex> lock(queueMutex_);

    if (timeoutMillis <= 0)
        queueCond_.wait(lock, [this]{return !queue_.empty();});
    else
    {
        // wait_for returns false if the return is due to timeout
        auto timeoutOccured = !queueCond_.wait_for(
            lock,
            std::chrono::milliseconds(timeoutMillis),
            [this]{return !queue_.empty();});

        if (timeoutOccured)
            return nullptr;
    }

    // Move the stored pointer out rather than cloning the message: front()->move()
    // heap-allocated a second Msg for every frame, on top of the one put() made.
    auto msg = std::move(queue_.front());
    queue_.pop();
    if (msg) ids_.erase(msg->getMsgId());
    return msg;
}

std::unique_ptr<Msg> Queue::tryGet()
{
    std::unique_lock<std::mutex> lock(queueMutex_);
    if (!queue_.empty())
    {
        // As in get(): move the pointer out instead of cloning it, and never
        // dereference it unchecked - an empty slot here used to be a fatal
        // dereference inside the renderer thread rather than a dropped frame.
        auto msg = std::move(queue_.front());
        queue_.pop();
        // Keep the dedup set in sync (get() does this; tryGet didn't) — the
        // renderer's flush paths drain via tryGet, and every leaked id both
        // grows the set forever and silently rejects future puts of that id.
        if (msg) ids_.erase(msg->getMsgId());
        return msg;
    }
    else
    {
        return{ nullptr };
    }
}

size_t Queue::size()
{
    std::unique_lock<std::mutex> lock(queueMutex_);
    auto size = queue_.size();
    return size;
}