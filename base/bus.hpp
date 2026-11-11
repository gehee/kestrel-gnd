#pragma once
// How what one thread makes reaches another without the maker ever waiting
// for the taker.
//
//   Topic<T>        newest value wins (link stats, air info): one writer, any
//                   readers, a seqlock - the writer never waits, a reader
//                   retries the rare read a write overlapped.
//   Ring<T, N>      items in order, one writer and one reader, sized at
//                   compile time (the renderer's latency samples). Full: the
//                   new one is dropped and counted; the writer never waits.
//   Waker           a consumer's "something for you": coalesced, a futex.
//   PicturePool     the video's memory: each picture is written once into a
//                   pooled buffer; every consumer gets a view of it.
//   SliceView       a borrowed view, valid during the call; retain() keeps
//                   the picture (a reference, no copy), copy_to() owns a copy.
//
// They are the hand-offs inside the parts that cross threads (system/part.hpp:
// the radio's model keeps each part's state in a Topic, its streams in a Ring);
// a caller sees the part's interface, never these.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <functional>
#include <linux/futex.h>
#include <memory>
#include <mutex>
#include <pthread.h>
#include <sys/syscall.h>
#include <type_traits>
#include <unistd.h>
#include <utility>
#include <vector>

namespace bus {

// ---- Waker ------------------------------------------------------------------
// The consumer sleeps in wait(); any producer poke()s. Pokes before the wait
// are not lost, and many pokes make one wake-up.
class Waker {
public:
    // poke() stores the word then reads sleeping_; wait() stores sleeping_
    // then reads the word. Each must see the other's store, so all four are
    // seq_cst (an acquire load may pass an earlier release store on ARM).
    void poke() {
        if (word_.exchange(1, std::memory_order_seq_cst) == 0 && sleeping_.load(std::memory_order_seq_cst))
            syscall(SYS_futex, &word_, FUTEX_WAKE_PRIVATE, 1, nullptr, nullptr, 0);
    }
    // Sleeps until poked or timeout_us passes (0: no timeout); true if poked.
    bool wait(uint64_t timeout_us = 0) {
        if (word_.exchange(0, std::memory_order_acquire)) return true;
        sleeping_.store(true, std::memory_order_seq_cst);
        timespec ts{(time_t)(timeout_us / 1000000), (long)(timeout_us % 1000000) * 1000};
        if (word_.load(std::memory_order_seq_cst) == 0)
            syscall(SYS_futex, &word_, FUTEX_WAIT_PRIVATE, 0, timeout_us ? &ts : nullptr, nullptr, 0);
        sleeping_.store(false, std::memory_order_relaxed);
        return word_.exchange(0, std::memory_order_acquire) != 0;
    }
private:
    std::atomic<int> word_{0};
    std::atomic<bool> sleeping_{false};
};

// ---- PiMutex ----------------------------------------------------------------
// A mutex with priority inheritance. The radio thread runs SCHED_FIFO; a lock
// it shares with an ordinary thread (the HUD, the web view) would let that
// thread, preempted inside its few lines, hold the radio up for as long as it
// stays preempted. Here the radio thread waiting lends the holder its
// priority, so the wait is the section's length. Lockable (std::lock_guard).
class PiMutex {
public:
    PiMutex() {
        pthread_mutexattr_t a;
        pthread_mutexattr_init(&a);
        pthread_mutexattr_setprotocol(&a, PTHREAD_PRIO_INHERIT);
        pthread_mutex_init(&m_, &a);
        pthread_mutexattr_destroy(&a);
    }
    ~PiMutex() { pthread_mutex_destroy(&m_); }
    PiMutex(const PiMutex&) = delete;
    PiMutex& operator=(const PiMutex&) = delete;
    void lock() { pthread_mutex_lock(&m_); }
    void unlock() { pthread_mutex_unlock(&m_); }
    bool try_lock() { return pthread_mutex_trylock(&m_) == 0; }
private:
    pthread_mutex_t m_;
};

// ---- Topic ------------------------------------------------------------------
// The value is kept as atomic words so the overlapping copy is not a data race
// (each word is a plain load or store on ARM64 and x86).
template <class T> class Topic {
    static_assert(std::is_trivially_copyable<T>::value, "Topic<T>: a plain struct (no strings or vectors)");
    static constexpr size_t kWords = (sizeof(T) + 7) / 8;
public:
    // Read before the first publish, it is T's defaults (version 0).
    Topic() {
        uint64_t w[kWords] = {};
        const T v{};
        std::memcpy(w, &v, sizeof(T));
        for (size_t i = 0; i < kWords; i++) words_[i].store(w[i], std::memory_order_relaxed);
    }
    void publish(const T& v) {
        uint64_t w[kWords] = {};
        std::memcpy(w, &v, sizeof(T));
        const uint32_t s = seq_.load(std::memory_order_relaxed);
        seq_.store(s + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        for (size_t i = 0; i < kWords; i++) words_[i].store(w[i], std::memory_order_relaxed);
        seq_.store(s + 2, std::memory_order_release);
        if (waker_) waker_->poke();
    }
    // The newest value and its version (0: never published). A write takes
    // a copy's time; a reader that keeps finding one under way (its writer
    // preempted mid-copy, perhaps by this very reader at a higher priority)
    // sleeps a moment instead of spinning, so the writer gets to finish.
    uint32_t read(T& out) const {
        uint64_t w[kWords];
        for (unsigned tries = 0;; tries++) {
            if (tries >= 64) {
                const timespec ts{0, 20000};       // 20 us
                nanosleep(&ts, nullptr);
            }
            const uint32_t s0 = seq_.load(std::memory_order_acquire);
            if (s0 & 1) continue;                  // a write is under way
            for (size_t i = 0; i < kWords; i++) w[i] = words_[i].load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (seq_.load(std::memory_order_relaxed) == s0) {
                std::memcpy(&out, w, sizeof(T));
                return s0 >> 1;
            }
        }
    }
    uint32_t version() const { return seq_.load(std::memory_order_acquire) >> 1; }
    void set_waker(Waker* w) { waker_ = w; }      // before the threads start
private:
    std::atomic<uint32_t> seq_{0};
    std::atomic<uint64_t> words_[kWords] = {};
    Waker* waker_ = nullptr;
};

// ---- Ring -------------------------------------------------------------------
template <class T, size_t N> class Ring {
    static_assert((N & (N - 1)) == 0, "Ring<T, N>: N a power of two");
public:
    // was_empty: the reader had taken everything before this item, so it may
    // be going to sleep - a writer that wakes it only then (not for each item)
    // loses no wake. Decided after the item is in: the writer stores the head
    // then reads the tail, the reader stores the tail then reads the head (all
    // four seq_cst), so one of them sees the other - at worst a wake that
    // finds the item already taken, never a lost one.
    bool push(T v, bool* was_empty = nullptr) {
        const size_t h = head_.load(std::memory_order_relaxed);
        if (h - tail_.load(std::memory_order_acquire) == N) {
            drops_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        slots_[h & (N - 1)] = std::move(v);
        head_.store(h + 1, std::memory_order_seq_cst);
        if (was_empty) *was_empty = tail_.load(std::memory_order_seq_cst) == h;
        if (waker_) waker_->poke();
        return true;
    }
    bool pop(T& out) {
        const size_t t = tail_.load(std::memory_order_relaxed);
        if (t == head_.load(std::memory_order_seq_cst)) return false;
        out = std::move(slots_[t & (N - 1)]);
        slots_[t & (N - 1)] = T();                 // let go of what it held
        tail_.store(t + 1, std::memory_order_seq_cst);
        return true;
    }
    size_t size() const { return head_.load(std::memory_order_seq_cst) - tail_.load(std::memory_order_acquire); }
    uint64_t drops() const { return drops_.load(std::memory_order_relaxed); }
    void set_waker(Waker* w) { waker_ = w; }
private:
    T slots_[N];
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};
    std::atomic<uint64_t> drops_{0};
    Waker* waker_ = nullptr;
};

// ---- Pictures -----------------------------------------------------------------
class PicturePool;

struct PictureBuf {
    uint8_t* base = nullptr;       // headroom, then the picture
    uint32_t cap = 0;              // bytes after the headroom
    uint32_t headroom = 0;         // room in front: parameter sets without moving the slices
    uint32_t len = 0;              // bytes written after the headroom
    uint64_t pts = 0;
    bool keyframe = false;
    // Every slice of it came, by the numbers the air unit gives them: 1 yes,
    // 0 no (the radio lost some), -1 not said (no numbers in its headers).
    int8_t whole = -1;
    std::atomic<int> refs{0};
    PicturePool* pool = nullptr;
    int index = 0;
    uint8_t* data() const { return base + headroom; }
};

class PictureRef {
public:
    PictureRef() = default;
    explicit PictureRef(PictureBuf* b) : b_(b) {}          // takes a reference already counted
    PictureRef(const PictureRef& o) : b_(o.b_) { if (b_) b_->refs.fetch_add(1, std::memory_order_relaxed); }
    PictureRef(PictureRef&& o) noexcept : b_(o.b_) { o.b_ = nullptr; }
    PictureRef& operator=(PictureRef o) noexcept { std::swap(b_, o.b_); return *this; }
    ~PictureRef() { reset(); }
    inline void reset();
    PictureBuf* get() const { return b_; }
    PictureBuf* operator->() const { return b_; }
    explicit operator bool() const { return b_ != nullptr; }
private:
    PictureBuf* b_ = nullptr;
};

// Up to 64 buffers, handed out and returned through one bitmap: acquire()
// (the radio thread) and release (any thread) never lock.
class PicturePool {
public:
    // The memory is the caller's (a HAL buffer group on Rockchip, plain
    // memory elsewhere): count buffers of headroom + cap bytes each.
    PicturePool(uint8_t* mem, int count, uint32_t cap, uint32_t headroom) {
        if (count > 64) count = 64;
        for (int i = 0; i < count; i++) {
            bufs_[i].base = mem + (size_t)i * (cap + headroom);
            bufs_[i].cap = cap;
            bufs_[i].headroom = headroom;
            bufs_[i].pool = this;
            bufs_[i].index = i;
        }
        free_.store(count == 64 ? ~0ull : ((1ull << count) - 1));
    }
    // An empty buffer, or an empty ref if all are held (counted).
    PictureRef acquire() {
        uint64_t f = free_.load(std::memory_order_acquire);
        while (f) {
            const int i = __builtin_ctzll(f);
            if (free_.compare_exchange_weak(f, f & ~(1ull << i), std::memory_order_acq_rel)) {
                PictureBuf& b = bufs_[i];
                b.len = 0; b.pts = 0; b.keyframe = false; b.whole = -1;
                b.refs.store(1, std::memory_order_relaxed);
                return PictureRef(&b);
            }
        }
        empty_.fetch_add(1, std::memory_order_relaxed);
        return PictureRef();
    }
    void release(PictureBuf* b) { free_.fetch_or(1ull << b->index, std::memory_order_release); }
    int free_count() const { return __builtin_popcountll(free_.load(std::memory_order_relaxed)); }
    uint64_t empty_count() const { return empty_.load(std::memory_order_relaxed); }
private:
    PictureBuf bufs_[64];
    std::atomic<uint64_t> free_{0};
    std::atomic<uint64_t> empty_{0};
};

inline void PictureRef::reset() {
    if (b_ && b_->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) b_->pool->release(b_);
    b_ = nullptr;
}

// What a sink is handed: borrowed for the call. The picture is still being
// built while its slices are handed out: read only [off, off + len), and
// never the picture's len or the bytes past this slice until on_picture.
// retain() keeps the buffer alive, not a finished picture.
struct SliceView {
    const PictureRef* pic = nullptr;
    uint32_t off = 0, len = 0;
    bool last = false;                               // the picture's last slice
    const uint8_t* data() const { return (*pic)->data() + off; }
    PictureRef retain() const { return *pic; }       // a reference: no copy
    size_t copy_to(uint8_t* dst) const {             // the one explicit copy
        copies().fetch_add(len, std::memory_order_relaxed);
        std::memcpy(dst, data(), len);
        return len;
    }
    // Bytes copied out of pictures by any sink, for the stats.
    static std::atomic<uint64_t>& copies() { static std::atomic<uint64_t> n{0}; return n; }
};

// A consumer of the video, called on the radio thread: it returns at once,
// keeping a picture with a reference (or copying it) to work on elsewhere.
struct VideoSink {
    virtual ~VideoSink() = default;
    virtual void on_slice(const SliceView&) {}           // as each slice lands
    virtual void on_picture(const PictureRef&) {}        // the whole picture, once its last slice is in
    // Whole pictures wanted: the radio builds them for a sink that does even
    // with no decoder running (a recorder on a box without a screen).
    virtual bool wants_pictures() const { return true; }
    // Every valid VPS/SPS/PPS so far (Annex-B, one per type and id), each
    // time the set changes.
    virtual void on_parameter_sets(const std::vector<std::vector<uint8_t>>&) {}
    // It needs a keyframe (a recording starts on one): true once per need,
    // and the radio asks the air unit for one. Polled after each picture.
    virtual bool take_keyframe_request() { return false; }
    // It records the camera's stream as it comes (a STREAM recording) right
    // now: pictures in strips are no one stream, so while this is true the
    // air unit is asked for whole pictures. Polled on the radio thread.
    virtual bool needs_stream() const { return false; }
};

}  // namespace bus
