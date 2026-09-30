/* -*- c++ -*- */
/*
 * Copyright 2026 Jeff Long
 * SPDX-License-Identifier: MIT
 */
#include <timesource/GpsSource.hpp>

#include "support/Cases.hpp"
#include "support/Graph.hpp"
#include "support/FakeSerialPort.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <string>
#include <vector>

namespace {

using namespace std::chrono_literals;
using timesource::test::RecordingSink;
using timesource::test::ScheduledGraph;
using timesource::test::SeenTag;
using timesource::test::value;

constexpr std::chrono::sys_days kMarch11{std::chrono::year{2026} / 3 / 11};

std::uint64_t noonPlus(int second) { return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>((kMarch11 + std::chrono::hours{12} + std::chrono::seconds{second}).time_since_epoch()).count()); }

// One second of a receiver with a 3D fix at 12:00:ss on 2026-03-11, in the order a u-blox writes.
void fixedSecond(const timesource::test::FakePort& port, int second) {
    port.sentence(std::format("GPRMC,1200{:02d}.00,A,5001.1900,N,00840.6570,E,0.5,45.0,110326,,,A", second));
    port.sentence(std::format("GPGGA,1200{:02d}.00,5001.1900,N,00840.6570,E,1,10,0.8,136.0,M,47.0,M,,", second));
    port.sentence("GPGSA,A,3,04,05,09,12,,,,,,,,,1.8,1.0,1.5");
    port.sentence("GPGSV,1,1,04,04,40,083,46,05,17,308,41,09,59,224,45,12,65,109,47");
}

// One second of a receiver that knows the time and has no fix.
void unfixedSecond(const timesource::test::FakePort& port, int second) {
    port.sentence(std::format("GPRMC,1200{:02d}.00,V,,,,,,,110326,,,N", second));
    port.sentence(std::format("GPGGA,1200{:02d}.00,,,,,0,00,99.99,,,,,,", second));
    port.sentence("GPGSA,A,1,,,,,,,,,,,,,99.99,99.99,99.99");
}

/*| contract: run a GPS source on a fresh fake port into a recording sink, write the seconds
        feed() writes, wait until the sink holds want samples or five seconds pass, and stop.
        Answers the tags with a trigger name, whether the samples arrived, and the settings the
        port held once they arrived.
*/
struct GpsRun {
    std::vector<SeenTag>               triggers;
    std::size_t                        samples = 0;
    bool                               arrived = false;
    std::string                        path;
    timesource::test::FakePortSettings port;
};

template <typename Feed>
GpsRun runGps(gr::property_map settings, std::size_t want, Feed&& feed) {
    GpsRun run;
    auto   port = timesource::test::FakePort::create();
    if (!port) {
        return run;
    }
    run.path = port->path();
    settings.insert_or_assign(std::pmr::string("device_path"), port->path());
    gr::Graph flow;
    auto&     gps  = flow.emplaceBlock<timesource::GpsSource>(std::move(settings));
    auto&     sink = flow.emplaceBlock<RecordingSink>();
    if (!flow.connect<"out", "in">(gps, sink).has_value()) {
        return run;
    }
    ScheduledGraph scheduled;
    if (!scheduled.adopt(std::move(flow)).has_value()) {
        return run;
    }
    scheduled.start();
    feed(*port);
    run.arrived = sink.awaitSamples(want, 5s);
    run.port    = port->settings();
    scheduled.stop();
    run.triggers = sink.triggers();
    run.samples  = sink.samples();
    return run;
}

} // namespace

int main(int argc, char** argv) {
    using namespace boost::ut;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    timesource::test::Cases cases(argc, argv);

    cases("gps.emits-a-tag-on-each-new-second", [] {
        const auto run = runGps({}, 3UZ, [](const auto& port) {
            for (int s = 0; s < 4; ++s) {
                fixedSecond(port, s);
            }
        });
        expect(fatal(run.arrived)) << "four seconds of sentences close three";
        expect(fatal(eq(run.triggers.size(), 3UZ))) << "one tag per closed second";
        for (std::size_t i = 0; i < run.triggers.size(); ++i) {
            const auto& tag = run.triggers[i].map;
            expect(eq(run.triggers[i].index, i)) << "one sample per second, the tag on it";
            expect(value<std::string>(tag, "trigger_name") == std::optional<std::string>{"GPS_PPS"}) << "a locked second carries the plain name";
            expect(value<std::uint64_t>(tag, "trigger_time") == std::optional<std::uint64_t>{noonPlus(static_cast<int>(i) + 1)}) << "the UTC second the closing sentence names";
            expect(value<float>(tag, "trigger_offset") == std::optional<float>{0.f});
            const auto meta = value<gr::property_map>(tag, "trigger_meta_info");
            expect(fatal(meta.has_value())) << "the fix record rides along";
            expect(value<std::int32_t>(*meta, "satellites") == std::optional<std::int32_t>{10});
            expect(value<float>(*meta, "hdop") == std::optional<float>{1.0f}) << "the GSA's HDOP, the last one of the second";
            expect(value<std::string>(*meta, "fix_type") == std::optional<std::string>{"fix3D"});
            expect(std::fabs(value<float>(*meta, "speed_kmh").value_or(0.f) - 0.926f) < 1e-4f && value<float>(*meta, "heading_deg") == std::optional<float>{45.f});
            expect(value<std::uint64_t>(*meta, "local_time").value_or(0) > noonPlus(0)) << "the host clock at the arrival";
            expect(value<std::string>(*meta, "device_info") == std::optional<std::string>{run.path}) << "the port the block read";
            const auto geo = value<gr::property_map>(*meta, "geolocation");
            expect(fatal(geo.has_value()));
            expect(value<std::string>(*geo, "type") == std::optional<std::string>{"Point"}) << "a GeoJSON point";
            const auto it = geo->find("coordinates");
            expect(fatal(it != geo->end()));
            const auto* coordinates = it->second.get_if<gr::Tensor<float>>();
            expect(fatal(coordinates != nullptr && coordinates->size() == 3UZ)) << "longitude, latitude and altitude";
            expect(std::fabs(coordinates->data()[0] - 8.677617f) < 1e-5f && std::fabs(coordinates->data()[1] - 50.019833f) < 1e-5f && std::fabs(coordinates->data()[2] - 136.f) < 1e-4f) << "in GeoJSON order";
        }
    });

    cases("gps.marks-a-second-without-a-fix-unlocked", [] {
        const auto before = timesource::wallClockNs();
        const auto run    = runGps({}, 2UZ, [](const auto& port) {
            for (int s = 0; s < 3; ++s) {
                unfixedSecond(port, s);
            }
        });
        expect(fatal(run.arrived && eq(run.triggers.size(), 2UZ)));
        for (const auto& t : run.triggers) {
            expect(value<std::string>(t.map, "trigger_name") == std::optional<std::string>{"GPS_PPS(unlocked)"}) << "the name says the second is unlocked";
            const auto meta = value<gr::property_map>(t.map, "trigger_meta_info");
            expect(fatal(meta.has_value()));
            expect(value<std::uint64_t>(t.map, "trigger_time") == value<std::uint64_t>(*meta, "local_time")) << "at host time";
            expect(value<std::uint64_t>(t.map, "trigger_time").value_or(0) >= before) << "taken during the run";
            expect(value<std::string>(*meta, "fix_type") == std::optional<std::string>{"none"});
        }
    });

    cases("gps.meta-info-off-leaves-the-fix-out", [] {
        const auto run = runGps({{"emit_meta_info", false}}, 1UZ, [](const auto& port) {
            fixedSecond(port, 0);
            fixedSecond(port, 1);
        });
        expect(fatal(run.arrived && eq(run.triggers.size(), 1UZ)));
        expect(!run.triggers[0].map.contains("trigger_meta_info")) << "no meta map";
        expect(value<std::uint64_t>(run.triggers[0].map, "trigger_time") == std::optional<std::uint64_t>{noonPlus(1)}) << "and the time still";
    });

    cases("gps.clock-mode-writes-sample-rate-samples-per-second", [] {
        const auto run = runGps({{"emit_mode", std::string("clock")}, {"sample_rate", 100.f}}, 300UZ, [](const auto& port) {
            for (int s = 0; s < 4; ++s) {
                fixedSecond(port, s);
            }
        });
        expect(fatal(run.arrived)) << "three closed seconds of 100 samples";
        expect(eq(run.samples, 300UZ)) << "and not a sample more";
        expect(fatal(eq(run.triggers.size(), 3UZ)));
        for (std::size_t i = 0; i < run.triggers.size(); ++i) {
            expect(eq(run.triggers[i].index, 100UZ * i)) << "each tag on the first sample of its second";
        }
    });

    cases("gps.context-goes-on-the-tag", [] {
        const auto run = runGps({{"context", std::string("antenna-a")}, {"trigger_name", std::string("REF")}}, 1UZ, [](const auto& port) {
            fixedSecond(port, 0);
            fixedSecond(port, 1);
        });
        expect(fatal(run.arrived && !run.triggers.empty()));
        expect(value<std::string>(run.triggers[0].map, "context") == std::optional<std::string>{"antenna-a"});
        expect(value<std::string>(run.triggers[0].map, "trigger_name") == std::optional<std::string>{"REF"}) << "under the name the graph gave";
    });

    cases("gps.enumerations-are-set-by-name", [] {
        gr::Graph flow;
        auto&     gps = flow.emplaceBlock<timesource::GpsSource>({{"baud_rate", std::string("Baud9600")}, {"emit_mode", std::string("clock")}});
        expect(gps.baud_rate.value == timesource::BaudRate::Baud9600) << "a graph names the rate as Baud<bits per second>";
        expect(gps.emit_mode.value == timesource::EmitMode::clock);
    });

    cases("gps.reads-at-4800-baud-8n1-by-default", [] {
        gr::Graph flow;
        expect(flow.emplaceBlock<timesource::GpsSource>().baud_rate.value == timesource::BaudRate{}) << "the default rate is the zero value";
        const auto run = runGps({}, 1UZ, [](const auto& port) {
            fixedSecond(port, 0);
            fixedSecond(port, 1);
        });
        expect(fatal(run.arrived)) << "the block read the port";
        expect(run.port.open && eq(run.port.mode, static_cast<int>(SP_MODE_READ))) << "opened for reading only";
        expect(eq(run.port.baudrate, 4800)) << "at 4800 baud, with baud_rate left unset";
        expect(eq(run.port.bits, 8) && eq(run.port.parity, static_cast<int>(SP_PARITY_NONE)) && eq(run.port.stopbits, 1)) << "8 data bits, no parity, one stop bit";
        expect(eq(run.port.flowcontrol, static_cast<int>(SP_FLOWCONTROL_NONE))) << "and no flow control";
    });

    cases("gps.detection-passes-over-a-silent-port", [] {
        using timesource::test::FakePort;
        using timesource::test::FakeUsbId;
        const auto silent  = FakePort::create(FakeUsbId{.vendorId = 0x1546, .productId = 0x01a9, .product = {}}); // a u-blox M8, ranked first
        const auto talking = FakePort::create(FakeUsbId{.vendorId = 0x1a86, .productId = 0x7523, .product = {}}); // a CH340 bridge
        expect(fatal(silent.has_value() && talking.has_value()));
        silent->write("$ stray text on the port\r\n");
        silent->write("$GPZDA,,,,,,\r\n");
        silent->sentence("GPTXT,01,01,02,ANTSTATUS=OK");
        for (int s = 0; s < 3; ++s) {
            fixedSecond(*talking, s);
        }
        gr::Graph flow;
        auto&     gps  = flow.emplaceBlock<timesource::GpsSource>();
        gps._silentLimit = 300ms;
        auto&     sink = flow.emplaceBlock<RecordingSink>();
        expect(fatal(flow.connect<"out", "in">(gps, sink).has_value()));
        ScheduledGraph scheduled;
        expect(fatal(scheduled.adopt(std::move(flow)).has_value()));
        scheduled.start();
        const bool arrived    = sink.awaitSamples(2UZ, 5s);
        const auto silentPort = silent->settings();
        scheduled.stop();
        expect(fatal(arrived)) << "the block reached the talking port";
        expect(eq(silentPort.mode, static_cast<int>(SP_MODE_READ)) && !silentPort.open) << "the silent port was opened first, then closed";
        const std::string talkingName = timesource::describe(timesource::PortInfo{.path = talking->path(), .description = {}, .manufacturer = {}, .product = {}, .vendorId = 0x1a86, .productId = 0x7523, .usb = true});
        const auto        tags        = sink.triggers();
        expect(fatal(!tags.empty()));
        expect(value<std::uint64_t>(tags[0].map, "trigger_time") == std::optional<std::uint64_t>{noonPlus(1)}) << "the talking port's second";
        const auto meta = value<gr::property_map>(tags[0].map, "trigger_meta_info");
        expect(fatal(meta.has_value()));
        expect(value<std::string>(*meta, "device_info") == std::optional<std::string>{talkingName}) << "read from the talking port: " << value<std::string>(*meta, "device_info").value_or("?");
    });

    cases("gps.refuses-a-start-on-a-path-that-does-not-exist", [] {
        gr::Graph flow;
        auto&     gps  = flow.emplaceBlock<timesource::GpsSource>({{"device_path", std::string("/dev/timesource-no-such-port")}});
        auto&     sink = flow.emplaceBlock<RecordingSink>();
        expect(fatal(flow.connect<"out", "in">(gps, sink).has_value()));
        const auto end = timesource::test::runToEnd(std::move(flow));
        expect(end.returned) << "runAndWait() returns by itself";
        expect(end.failed) << "and fails";
        expect(end.error.contains("device '/dev/timesource-no-such-port' does not exist")) << "with the reason as its error: " << end.error;
    });

    const int rc = cases.finish();
    std::fflush(stdout);
    std::_Exit(rc);
}
