/* -*- c++ -*- */
/*
 * Copyright 2026 Jeff Long
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <gnuradio-4.0/Block.hpp>
#include <gnuradio-4.0/Graph.hpp>
#include <gnuradio-4.0/Message.hpp>
#include <gnuradio-4.0/Scheduler.hpp>
#include <gnuradio-4.0/SchedulerModel.hpp>

#include <atomic>
#include <concepts>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace timesource::test {

/*| role: one tag as a sink saw it: the index of its sample in the whole stream, and its map.
*/
struct SeenTag {
    std::size_t      index = 0;
    gr::property_map map;
};

/*| role: a sink that records the samples and tags it receives, read safely from another thread.
*/
struct RecordingSink : gr::Block<RecordingSink> {
    gr::PortIn<std::uint8_t> in;
    GR_MAKE_REFLECTABLE(RecordingSink, in);

    mutable std::mutex       _mutex;
    std::vector<SeenTag>     _tags;
    std::atomic<std::size_t> _samples{0};

    gr::work::Status processBulk(gr::InputSpanLike auto& input) {
        const std::size_t before = _samples.load(std::memory_order_relaxed);
        {
            std::lock_guard lock(_mutex);
            for (const auto& [relIndex, tagMap] : input.tags()) {
                _tags.push_back({before + static_cast<std::size_t>(relIndex < 0 ? 0 : relIndex), tagMap.get()});
            }
        }
        _samples.store(before + input.size(), std::memory_order_release);
        return gr::work::Status::OK;
    }

    [[nodiscard]] std::size_t samples() const { return _samples.load(std::memory_order_acquire); }

    // The tags that carry a trigger name, in stream order.
    [[nodiscard]] std::vector<SeenTag> triggers() const {
        std::lock_guard      lock(_mutex);
        std::vector<SeenTag> found;
        for (const SeenTag& t : _tags) {
            if (t.map.contains("trigger_name")) {
                found.push_back(t);
            }
        }
        return found;
    }

    // Wait until count samples have arrived or the wait has passed; true for the former.
    [[nodiscard]] bool awaitSamples(std::size_t count, std::chrono::milliseconds wait) const {
        const auto deadline = std::chrono::steady_clock::now() + wait;
        while (samples() < count) {
            if (std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return true;
    }
};

/*| role: a graph held by the default scheduler on the scheduler's own thread, with a reader on
        the scheduler's message output.
    trap: a scheduler whose message output has no reader turns a block's error into an
        exception on its worker thread. adopt() connects the reader before the graph reaches
        the scheduler, and the reader is declared ahead of the scheduler.
*/
class ScheduledGraph {
public:
    [[nodiscard]] std::expected<void, gr::Error> adopt(gr::Graph&& graph) {
        if (auto connected = _scheduler.blockRef().msgOut.connect(_events); !connected) {
            return std::unexpected(connected.error());
        }
        if (auto exchanged = _scheduler.blockRef().exchange(std::move(graph)); !exchanged) {
            return std::unexpected(exchanged.error());
        }
        return {};
    }

    [[nodiscard]] std::expected<void, gr::Error> runAndWait() { return _scheduler.blockRef().runAndWait(); }
    void                                         start() { _scheduler.start(); }
    void                                         stop() { _scheduler.stop(); }

    // The error reports on the message output so far, oldest first.
    [[nodiscard]] std::vector<std::string> errors() {
        auto                     messages = _events.streamReader().get();
        std::vector<std::string> reasons;
        for (const gr::Message& message : messages) {
            if (!message.data.has_value()) {
                reasons.push_back(message.data.error().message);
            }
        }
        std::ignore = messages.consume(messages.size());
        return reasons;
    }

private:
    gr::MsgPortIn                                    _events;
    gr::SchedulerWrapper<gr::scheduler::Simple<>>    _scheduler;
};

/*| role: how a run under runAndWait() ended.
*/
struct RunEnd {
    bool        returned = false;
    bool        failed   = false;
    std::string error;
};

/*| contract: run the graph with runAndWait() on a thread of its own, as a program's main()
        does, and answer whether the call returned within the wait and how.
    trap: a run that never ends keeps its thread and its scheduler. Both are left on the heap,
        and the case reads the answer as a run that did not return.
*/
inline RunEnd runToEnd(gr::Graph flow, std::chrono::milliseconds wait = std::chrono::seconds(5)) {
    RunEnd end;
    auto*  scheduled = new ScheduledGraph();
    if (!scheduled->adopt(std::move(flow)).has_value()) {
        delete scheduled;
        return end;
    }
    auto        outcome = std::make_shared<std::atomic<int>>(0); // 0 running, 1 succeeded, 2 failed
    auto        error   = std::make_shared<std::string>();        // written before outcome, read after it
    std::thread runner([scheduled, outcome, error] {
        const auto result = scheduled->runAndWait();
        if (!result.has_value()) {
            *error = result.error().message;
        }
        outcome->store(result.has_value() ? 1 : 2, std::memory_order_release);
    });
    const auto deadline = std::chrono::steady_clock::now() + wait;
    while (outcome->load(std::memory_order_acquire) == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (outcome->load(std::memory_order_acquire) == 0) {
        runner.detach();
        return end;
    }
    runner.join();
    end.returned = true;
    end.failed   = outcome->load(std::memory_order_acquire) == 2;
    end.error    = *error;
    delete scheduled;
    return end;
}

/*| contract: the value under key in a tag map, or nothing where it is absent or of another type.
*/
template <typename T>
[[nodiscard]] std::optional<T> value(const gr::property_map& map, std::string_view key) {
    const auto it = map.find(key);
    if (it == map.end()) {
        return std::nullopt;
    }
    if constexpr (std::same_as<T, std::string>) {
        if (!it->second.is_string()) {
            return std::nullopt;
        }
        return it->second.value_or(std::string{});
    } else {
        if (const auto* held = it->second.template get_if<T>()) {
            return *held;
        }
        return std::nullopt;
    }
}

} // namespace timesource::test
