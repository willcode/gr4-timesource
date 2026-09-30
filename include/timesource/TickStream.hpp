/* -*- c++ -*- */
/*
 * Copyright 2026 Jeff Long
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <timesource/NmeaParser.hpp>

#include <gnuradio-4.0/Block.hpp>
#include <gnuradio-4.0/Message.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <mutex>
#include <optional>
#include <source_location>
#include <string_view>
#include <thread>
#include <utility>

namespace timesource {

// The host's wall clock in nanoseconds since the Unix epoch.
[[nodiscard]] inline std::uint64_t wallClockNs() noexcept { return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count()); }

/*| role: what a block's start() throws when it cannot proceed.
    contract: what() answers the reason alone. The framework puts the block in ERROR, and the
        scheduler ends the run with the reason as its error.
*/
struct StartFailure : gr::exception {
    using gr::exception::exception;

    [[nodiscard]] const char* what() const noexcept override { return message.c_str(); }
};

[[noreturn]] inline void throwStartFailure(std::string_view tag, std::string_view reason, std::source_location where = std::source_location::current()) {
    std::fprintf(stderr, "[%.*s] the start failed: %.*s\n", static_cast<int>(tag.size()), tag.data(), static_cast<int>(reason.size()), reason.data());
    throw StartFailure(reason, where);
}

/*| role: the io thread of a block: a std::thread with a stop request and a wait that ends at
        the request.
    contract: start() stops and joins any thread held, then runs body on a new one. stop()
        requests the stop, wakes a waitFor() in progress and joins. The destructor stops.
    invariant: the owning block declares this member last, so the thread ends before any
        member it reads is destroyed.
*/
class IoThread {
public:
    IoThread() = default;
    IoThread(const IoThread&)            = delete;
    IoThread& operator=(const IoThread&) = delete;
    ~IoThread() { stop(); }

    template <typename Body>
    void start(Body&& body) {
        stop();
        _stop.store(false, std::memory_order_release);
        _thread = std::thread(std::forward<Body>(body));
    }

    void stop() {
        {
            std::lock_guard lock(_mutex);
            _stop.store(true, std::memory_order_release);
        }
        _wake.notify_all();
        if (_thread.joinable()) {
            _thread.join();
        }
    }

    [[nodiscard]] bool stopRequested() const noexcept { return _stop.load(std::memory_order_acquire); }

    // Wait for the duration or the stop request; true where the thread is to go on.
    bool waitFor(std::chrono::nanoseconds duration) {
        std::unique_lock lock(_mutex);
        return !_wake.wait_for(lock, duration, [this] { return stopRequested(); });
    }

private:
    std::mutex              _mutex;
    std::condition_variable _wake;
    std::atomic<bool>       _stop{true};
    std::thread             _thread;
};

/*| role: the seconds an io thread has seen and the scheduler thread has still to write.
    contract: push() never blocks the io thread. A push onto a full queue drops the oldest
        second and counts it in shed().
*/
template <typename Tick>
class TickQueue {
public:
    static constexpr std::size_t kCapacity = 16;

    void push(Tick tick) {
        std::lock_guard lock(_mutex);
        if (_ticks.size() == kCapacity) {
            _ticks.pop_front();
            ++_shed;
        }
        _ticks.push_back(std::move(tick));
    }

    [[nodiscard]] std::optional<Tick> pop() {
        std::lock_guard lock(_mutex);
        if (_ticks.empty()) {
            return std::nullopt;
        }
        Tick tick = std::move(_ticks.front());
        _ticks.pop_front();
        return tick;
    }

    void clear() {
        std::lock_guard lock(_mutex);
        _ticks.clear();
        _shed = 0;
    }

    [[nodiscard]] std::size_t shed() const {
        std::lock_guard lock(_mutex);
        return _shed;
    }

private:
    mutable std::mutex _mutex;
    std::deque<Tick>   _ticks;
    std::size_t        _shed = 0;
};

/*| contract: the samples one second takes: one in ppsOnly, and sample_rate rounded to a whole
        number, one at the least, in clock.
*/
[[nodiscard]] inline std::size_t samplesPerSecond(EmitMode mode, float sampleRate) noexcept {
    if (mode == EmitMode::ppsOnly || !(sampleRate >= 1.f)) {
        return 1UZ;
    }
    return static_cast<std::size_t>(std::llround(static_cast<double>(sampleRate)));
}

/*| role: writes the queued seconds onto a block's output as zero-valued samples, each second's
        tag on its first sample.
    contract: write() runs on the scheduler thread. It writes what the output edge has room
        for and answers the count; a second that does not fit is finished by later calls,
        ahead of the next second's tag. makeTag turns a queued tick into its tag map.
    trap: tryReserve grants all of a request or nothing, so a request is capped at the room
        the edge has. A clock-mode second longer than the edge would otherwise never be
        written.
*/
template <typename Tick>
class TickWriter {
public:
    void reset() {
        _owed = 0;
        _tag.reset();
    }

    template <typename Block, typename MakeTag>
    std::size_t write(Block& blk, TickQueue<Tick>& queue, std::size_t perSecond, MakeTag&& makeTag) {
        auto&       writer  = blk.out.streamWriter();
        std::size_t written = 0;
        for (;;) {
            if (_owed == 0) {
                auto tick = queue.pop();
                if (!tick) {
                    break;
                }
                _owed = perSecond;
                _tag  = makeTag(*tick);
            }
            const std::size_t n = std::min(_owed, writer.available());
            if (n == 0) {
                break;
            }
            {
                auto span = writer.template tryReserve<gr::SpanReleasePolicy::ProcessNone>(n);
                if (span.empty()) {
                    break;
                }
                std::ranges::fill(span, std::uint8_t{0});
                if (_tag) {
                    blk.out.publishTag(std::move(*_tag), 0UZ);
                    _tag.reset();
                }
                span.publish(n);
            }
            _owed -= n;
            written += n;
        }
        return written;
    }

private:
    std::size_t                     _owed = 0;
    std::optional<gr::property_map> _tag;
};

} // namespace timesource
