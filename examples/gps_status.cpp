/* -*- c++ -*- */
/*
 * Copyright 2026 Jeff Long
 * SPDX-License-Identifier: MIT
 */
#include <timesource/GpsSource.hpp>

#include <gnuradio-4.0/Block.hpp>
#include <gnuradio-4.0/Graph.hpp>
#include <gnuradio-4.0/Message.hpp>
#include <gnuradio-4.0/Scheduler.hpp>
#include <gnuradio-4.0/Tag.hpp>
#include <gnuradio-4.0/Tensor.hpp>

#include <atomic>
#include <charconv>
#include <chrono>
#include <concepts>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <format>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

namespace {

std::atomic<bool> gStop{false};

extern "C" void onSignal(int) { gStop.store(true, std::memory_order_relaxed); }

/*| contract: the value under key in a tag map, or nothing where it is absent or of another type.
*/
template <typename T>
[[nodiscard]] std::optional<T> valueOf(const gr::property_map& map, std::string_view key) {
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

/*| contract: one status line for a GpsSource tag. The line holds the UTC time, "locked" or
        "unlocked", the fix type ("none", "fix2D" or "fix3D"), the satellites in use, HDOP,
        latitude and longitude in degrees, altitude in meters, and the device.
    frame: an unlocked tag carries the host clock as its time; a locked one carries the UTC
        second the receiver reported.
*/
[[nodiscard]] std::string statusLine(const gr::property_map& tag) {
    const std::string   name   = valueOf<std::string>(tag, "trigger_name").value_or("");
    const bool          locked = !name.ends_with("(unlocked)");
    const std::uint64_t timeNs = valueOf<std::uint64_t>(tag, "trigger_time").value_or(0U);
    const auto          when   = std::chrono::sys_seconds{std::chrono::seconds{static_cast<std::int64_t>(timeNs / 1'000'000'000U)}};

    const auto meta      = valueOf<gr::property_map>(tag, "trigger_meta_info").value_or(gr::property_map{});
    const auto where     = valueOf<gr::property_map>(meta, "geolocation").value_or(gr::property_map{});
    const auto point     = valueOf<gr::Tensor<float>>(where, "coordinates").value_or(gr::Tensor<float>{});
    const auto longitude = point.size() > 0 ? static_cast<double>(point.data()[0]) : 0.0;
    const auto latitude  = point.size() > 1 ? static_cast<double>(point.data()[1]) : 0.0;
    const auto altitude  = point.size() > 2 ? static_cast<double>(point.data()[2]) : 0.0;

    return std::format("{:%Y-%m-%d %H:%M:%S} UTC {} {} sats {} hdop {:.2f} lat {:.5f} lon {:.5f} alt {:.1f} m device {}", when, locked ? "locked" : "unlocked", valueOf<std::string>(meta, "fix_type").value_or("none"), valueOf<std::int32_t>(meta, "satellites").value_or(0), static_cast<double>(valueOf<float>(meta, "hdop").value_or(0.f)), latitude, longitude, altitude, valueOf<std::string>(meta, "device_info").value_or("?"));
}

/*| role: writes one status line to standard output for each tag that carries a trigger name.
*/
struct StatusPrinter : gr::Block<StatusPrinter> {
    gr::PortIn<std::uint8_t> in;

    GR_MAKE_REFLECTABLE(StatusPrinter, in);

    std::size_t nLines = 0UZ;

    gr::work::Status processBulk(gr::InputSpanLike auto& input) {
        for (const auto& [relIndex, tagMap] : input.tags()) {
            std::ignore = relIndex;
            if (tagMap.get().contains("trigger_name")) {
                std::println("{}", statusLine(tagMap.get()));
                std::fflush(stdout);
                ++nLines;
            }
        }
        return gr::work::Status::OK;
    }
};

struct Options {
    std::string   device;
    std::uint32_t baud    = timesource::bitsPerSecond(timesource::BaudRate{});
    double        seconds = 0.0;
};

void printUsage() {
    std::println("usage: gps_status [--device <path>] [--baud <rate>] [--seconds <s>]");
    std::println("");
    std::println("Prints the status of a GPS receiver once per second: UTC time, fix,");
    std::println("satellites in use, HDOP, latitude, longitude, altitude and device.");
    std::println("");
    std::println("  --device <path>  serial port of the receiver (default: detect one)");
    std::println("  --baud <rate>    4800, 9600, 19200, 38400, 57600, 115200 or 230400");
    std::println("                   (default: 4800)");
    std::println("  --seconds <s>    stop after s seconds (default: run until Ctrl-C)");
    std::println("  --help           print this text");
    std::println("");
    std::println("The default rate is 4800 baud, the rate NMEA 0183 names and the");
    std::println("default of GpsSource. At a wrong rate no sentence arrives.");
}

[[nodiscard]] std::optional<std::string> baudName(std::uint32_t rate) {
    for (const std::uint32_t known : {4800U, 9600U, 19200U, 38400U, 57600U, 115200U, 230400U}) {
        if (rate == known) {
            return std::format("Baud{}", rate);
        }
    }
    return std::nullopt;
}

/*| contract: the options, or an exit status: 0 after --help, 2 for an argument refused.
*/
[[nodiscard]] std::expected<Options, int> parseOptions(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            printUsage();
            return std::unexpected(0);
        }
        if (i + 1 >= argc || (arg != "--device" && arg != "--baud" && arg != "--seconds")) {
            std::println(stderr, "gps_status: unknown or incomplete argument '{}'; see --help", arg);
            return std::unexpected(2);
        }
        const std::string_view value = argv[++i];
        if (arg == "--device") {
            options.device = std::string(value);
        } else if (arg == "--baud") {
            const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), options.baud);
            if (ec != std::errc{} || end != value.data() + value.size() || !baudName(options.baud)) {
                std::println(stderr, "gps_status: --baud takes 4800, 9600, 19200, 38400, 57600, 115200 or 230400");
                return std::unexpected(2);
            }
        } else {
            const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), options.seconds);
            if (ec != std::errc{} || end != value.data() + value.size() || !(options.seconds > 0.0)) {
                std::println(stderr, "gps_status: --seconds takes a positive number");
                return std::unexpected(2);
            }
        }
    }
    return options;
}

} // namespace

/*| role: a graph of timesource::GpsSource into StatusPrinter, run until the receiver's port
        fails to open, the run length passes, or SIGINT or SIGTERM arrives.
    frame: the port is opened for reading only; nothing is written to the receiver.
    trap: a scheduler whose message output has no reader turns a block's error into an
        exception on its worker thread. The reader is connected before the graph reaches the
        scheduler and is declared ahead of it.
*/
int main(int argc, char** argv) {
    const auto parsed = parseOptions(argc, argv);
    if (!parsed) {
        return parsed.error();
    }
    const Options& options = *parsed;

    gr::Graph flow;
    auto&     gps     = flow.emplaceBlock<timesource::GpsSource>({{"device_path", options.device}, {"baud_rate", *baudName(options.baud)}});
    auto&     printer = flow.emplaceBlock<StatusPrinter>();
    if (!flow.connect<"out", "in">(gps, printer).has_value()) {
        std::println(stderr, "gps_status: the graph could not be connected");
        return 1;
    }

    gr::MsgPortIn           events;
    gr::scheduler::Simple<> scheduler;
    if (!scheduler.msgOut.connect(events).has_value() || !scheduler.exchange(std::move(flow)).has_value()) {
        std::println(stderr, "gps_status: the scheduler refused the graph");
        return 1;
    }

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    std::println(stderr, "gps_status: reading {} at {} baud", options.device.empty() ? std::string("the detected receiver") : options.device, options.baud);

    std::atomic<bool> done{false};
    std::atomic<bool> failed{false};
    std::thread       runner([&scheduler, &done, &failed] {
        if (const auto result = scheduler.runAndWait(); !result.has_value()) {
            std::println(stderr, "gps_status: the graph stopped: {}", result.error().message);
            failed.store(true, std::memory_order_relaxed);
        }
        done.store(true, std::memory_order_relaxed);
    });

    const auto began = std::chrono::steady_clock::now();
    while (!done.load(std::memory_order_relaxed) && !gStop.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (options.seconds > 0.0 && std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count() >= options.seconds) {
            break;
        }
    }
    if (!done.load(std::memory_order_relaxed)) {
        scheduler.requestStop();
    }
    runner.join();

    std::println(stderr, "gps_status: {} lines from {}", printer.nLines, gps.device_name.value.empty() ? std::string("no device") : gps.device_name.value);
    std::fflush(stdout);
    return failed.load(std::memory_order_relaxed) ? 1 : 0;
}
