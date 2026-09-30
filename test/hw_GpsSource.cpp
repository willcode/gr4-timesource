/* -*- c++ -*- */
/*
 * Copyright 2026 Jeff Long
 * SPDX-License-Identifier: MIT
 */
#include <timesource/GpsSource.hpp>

#include "support/Cases.hpp"
#include "support/Graph.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

/*| role: the GPS source on a real receiver, the port named by GR4TIMESOURCE_GPS_DEVICE and read
        at the rate GR4TIMESOURCE_GPS_BAUD names (4800 where unset).
    contract: the case passes where a tag named trigger_name, locked or "(unlocked)", arrives
        within thirty seconds. It prints the sentences the parser took and the state of each
        tag's fix, and prints no position.
    frame: the port is opened for reading only; nothing is written to the receiver.
*/
int main(int argc, char** argv) {
    using namespace boost::ut;
    using namespace std::chrono_literals;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    timesource::test::Cases cases(argc, argv);

    cases("hw.gps-a-receiver-tags-a-second", [] {
        const char* device = std::getenv("GR4TIMESOURCE_GPS_DEVICE");
        expect(fatal(device != nullptr && *device != '\0')) << "GR4TIMESOURCE_GPS_DEVICE names the receiver's port";
        const char*      baudText = std::getenv("GR4TIMESOURCE_GPS_BAUD");
        const std::string baud     = baudText != nullptr && *baudText != '\0' ? std::string("Baud") + baudText : std::string("Baud4800");

        gr::Graph flow;
        auto&     gps  = flow.emplaceBlock<timesource::GpsSource>({{"device_path", std::string(device)}, {"baud_rate", baud}});
        auto&     sink = flow.emplaceBlock<timesource::test::RecordingSink>();
        expect(fatal(flow.connect<"out", "in">(gps, sink).has_value()));
        timesource::test::ScheduledGraph scheduled;
        expect(fatal(scheduled.adopt(std::move(flow)).has_value()));
        const auto began = std::chrono::steady_clock::now();
        scheduled.start();
        const bool arrived = sink.awaitSamples(3UZ, 30s) || sink.samples() > 0;
        const auto waited  = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - began);
        scheduled.stop();

        const auto& n = gps._parser.counts();
        std::printf("port: %s at %s\n", gps.device_name.value.c_str(), baud.c_str());
        std::printf("sentences: RMC %zu, GGA %zu, GSA %zu, VTG %zu, ZDA %zu, other %zu, rejected %zu\n", n.rmc, n.gga, n.gsa, n.vtg, n.zda, n.other, n.rejected);
        std::printf("samples %zu after %lld ms\n", sink.samples(), static_cast<long long>(waited.count()));
        for (const auto& t : sink.triggers()) {
            const auto meta = timesource::test::value<gr::property_map>(t.map, "trigger_meta_info").value_or(gr::property_map{});
            std::printf("tag at %zu: name %s, trigger_time %llu, fix_type %s, satellites %d, hdop %.2f\n", t.index, timesource::test::value<std::string>(t.map, "trigger_name").value_or("?").c_str(), static_cast<unsigned long long>(timesource::test::value<std::uint64_t>(t.map, "trigger_time").value_or(0)), timesource::test::value<std::string>(meta, "fix_type").value_or("?").c_str(), timesource::test::value<std::int32_t>(meta, "satellites").value_or(-1), static_cast<double>(timesource::test::value<float>(meta, "hdop").value_or(-1.f)));
        }
        expect(arrived) << "a tag within thirty seconds";
        bool named = false;
        for (const auto& t : sink.triggers()) {
            const auto name = timesource::test::value<std::string>(t.map, "trigger_name");
            named           = named || name == std::optional<std::string>{"GPS_PPS"} || name == std::optional<std::string>{"GPS_PPS(unlocked)"};
        }
        expect(named) << "named for a fix or marked unlocked";
    });

    const int rc = cases.finish();
    std::fflush(stdout);
    std::_Exit(rc);
}
