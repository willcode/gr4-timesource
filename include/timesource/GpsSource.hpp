/* -*- c++ -*- */
/*
 * Copyright 2026 Jeff Long
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <timesource/NmeaParser.hpp>
#include <timesource/SerialPort.hpp>
#include <timesource/TickStream.hpp>

#include <gnuradio-4.0/Block.hpp>
#include <gnuradio-4.0/Tag.hpp>
#include <gnuradio-4.0/annotated.hpp>
#include <gnuradio-4.0/thread/thread_affinity.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace timesource {

[[nodiscard]] constexpr std::string_view fixTypeName(FixType type) noexcept {
    switch (type) {
    case FixType::none: return "none";
    case FixType::fix2D: return "fix2D";
    case FixType::fix3D: return "fix3D";
    }
    return "none";
}

/*| role: one second the receiver reported, handed from the io thread to the scheduler thread.
*/
struct GpsTick {
    SecondEdge  edge;
    std::string device;
};

/*| role: a GPS receiver on a serial port as a stream of tagged samples, one tag per UTC second.
    contract: the tag of a second carries TRIGGER_NAME, TRIGGER_TIME and TRIGGER_OFFSET, the
        record of the second before it in TRIGGER_META_INFO where emit_meta_info holds, and
        CONTEXT where context is set. A second is locked where its UTC time is known and the
        closed record holds a valid fix. TRIGGER_TIME is then the UTC second, and the name is
        trigger_name. Otherwise TRIGGER_TIME is the host's clock at the arrival of the second's
        first sentence, and "(unlocked)" follows the name. TRIGGER_OFFSET is 0, and 1 s for a
        locked leap second, whose TRIGGER_TIME is 23:59:59.
    contract: start() refuses a device_path that does not open. With device_path empty, the io
        thread opens the best-ranked receiver the system lists, and retries each second until
        one opens. A detected port that gives no checked sentence of a read kind within
        _silentLimit, kSilentLimit unless a case shortens it, is closed and the next ranked port
        is opened; after the last, the best-ranked one again. A port that fails while running
        is reopened the same way.
    frame: the io thread reads the port and runs the parser; the scheduler thread writes the
        samples and the tags and applies the settings. device_path, baud_rate and
        update_rate_ms take effect at the next start.
    invariant: _io is the last member, so the io thread ends before the port, the parser and
        the queue it uses are destroyed.
    verified-by: gps.emits-a-tag-on-each-new-second
    verified-by: gps.marks-a-second-without-a-fix-unlocked
    verified-by: gps.refuses-a-start-on-a-path-that-does-not-exist
    verified-by: gps.detection-passes-over-a-silent-port
*/
struct GpsSource : gr::Block<GpsSource> {
    using Description = gr::Doc<R"doc(@brief Emits a tagged sample at each UTC second a GPS receiver reports over a serial port.

The block reads the receiver's NMEA 0183 sentences (GGA, RMC, ZDA, GSA, VTG) from device_path, or from the receiver it detects by its USB identity when device_path is empty. The first sentence of each new second places a tag carrying trigger_name, the UTC time of that second and the fix of the second before it: position, altitude, satellites, HDOP, fix type, speed and heading. The receiver writes its sentences after the pulse that starts the second. The tag therefore follows the pulse by the receiver's output delay and the serial latency. A receiver without a fix gives tags named with the suffix "(unlocked)" at host time. In ppsOnly mode the block writes one sample per second; in clock mode it writes sample_rate samples per second, the tag on the first. The block never writes to the receiver.)doc">;

    gr::PortOut<std::uint8_t> out;

    gr::Annotated<std::string, "device_path", gr::Doc<"serial port path, or empty to detect a receiver">>                                  device_path;
    gr::Annotated<std::string, "device_name", gr::Visible, gr::Doc<"the port in use, set at open">>                                        device_name;
    gr::Annotated<std::string, "trigger_name", gr::Doc<"tag trigger name">>                                                                trigger_name     = std::string("GPS_PPS");
    gr::Annotated<std::string, "context", gr::Doc<"context tag value">>                                                                    context;
    gr::Annotated<EmitMode, "emit_mode", gr::Visible, gr::Doc<"output shape, 'ppsOnly' or 'clock'">>                                       emit_mode        = EmitMode::ppsOnly;
    gr::Annotated<float, "sample_rate", gr::Visible, gr::Unit<"Hz">, gr::Doc<"samples per second in clock mode">, gr::Limits<1.f, 100e6f>> sample_rate      = 1.f;
    gr::Annotated<bool, "emit_meta_info", gr::Doc<"attach the fix record as trigger_meta_info">>                                           emit_meta_info   = true;
    gr::Annotated<bool, "emit_device_info", gr::Doc<"add the port name to trigger_meta_info">>                                             emit_device_info = true;
    gr::Annotated<BaudRate, "baud_rate", gr::Doc<"serial rate, 'Baud4800' (default, zero value) to 'Baud230400'">>                         baud_rate        = BaudRate::Baud4800;
    gr::Annotated<std::uint32_t, "update_rate_ms", gr::Unit<"ms">, gr::Doc<"longest wait for serial data per read">>                       update_rate_ms   = 100U;

    GR_MAKE_REFLECTABLE(GpsSource, out, device_path, device_name, trigger_name, context, emit_mode, sample_rate, emit_meta_info, emit_device_info, baud_rate, update_rate_ms);

    static constexpr std::string_view kTag         = "timesource::GpsSource";
    static constexpr std::size_t      kMaxLine     = 4096;
    static constexpr auto             kRetryWait   = std::chrono::seconds{1};
    static constexpr auto             kSilentLimit = std::chrono::seconds{5};

    std::chrono::steady_clock::duration _silentLimit = kSilentLimit;

    SerialPort          _port;
    NmeaParser          _parser;
    TickQueue<GpsTick>  _ticks;
    TickWriter<GpsTick> _writer;
    IoThread            _io; // last: the io thread ends before the members it reads

    void start() {
        _io.stop();
        _parser = NmeaParser{};
        _ticks.clear();
        _writer.reset();
        const std::string path = device_path.value;
        const BaudRate    rate = baud_rate.value;
        const auto        wait = std::chrono::milliseconds{std::max<std::uint32_t>(update_rate_ms.value, 1U)};
        if (!path.empty()) {
            auto opened = SerialPort::open(path, rate);
            if (!opened) {
                throwStartFailure(kTag, opened.error());
            }
            _port             = std::move(*opened);
            device_name.value = shownName(path, _port.info());
        }
        _io.start([this, path, rate, wait] { ioLoop(path, rate, wait); });
    }

    void stop() {
        _io.stop();
        _port.close();
    }

    gr::work::Result work(std::size_t requestedWork = std::numeric_limits<std::size_t>::max()) noexcept {
        if (!gr::lifecycle::isActive(this->state())) {
            return {requestedWork, 0UZ, gr::work::Status::DONE};
        }
        this->applyChangedSettings();
        const std::size_t written = _writer.write(*this, _ticks, samplesPerSecond(emit_mode.value, sample_rate.value), [this](GpsTick& tick) { return makeTag(tick); });
        return {requestedWork, written, gr::work::Status::OK};
    }

    // The name device_name shows for a port opened by the path the caller gave.
    [[nodiscard]] static std::string shownName(std::string_view path, PortInfo info) {
        info.path = std::string(path);
        return describe(info);
    }

    [[nodiscard]] gr::property_map makeTag(GpsTick& tick) {
        const GpsFix& fix    = tick.edge.previous;
        const bool    locked = tick.edge.hasTime && fix.valid;
        if (!tick.device.empty() && tick.device != device_name.value) {
            device_name.value = tick.device;
        }
        auto tag = out.makeTagMap();
        gr::tag::put(tag, gr::tag::TRIGGER_NAME, locked ? trigger_name.value : trigger_name.value + "(unlocked)");
        gr::tag::put(tag, gr::tag::TRIGGER_TIME, locked ? tick.edge.utcNs : fix.localTimeNs);
        gr::tag::put(tag, gr::tag::TRIGGER_OFFSET, locked && tick.edge.leapSecond ? 1.f : 0.f);
        if (emit_meta_info.value) {
            auto geolocation = out.makeTagMap();
            gr::tag::put(geolocation, "type", std::string("Point"));
            gr::tag::put(geolocation, "coordinates", std::vector<float>{static_cast<float>(fix.longitude), static_cast<float>(fix.latitude), fix.altitude});
            auto meta = out.makeTagMap();
            gr::tag::put(meta, "geolocation", std::move(geolocation));
            gr::tag::put(meta, "local_time", fix.localTimeNs);
            gr::tag::put(meta, "satellites", fix.satellites);
            gr::tag::put(meta, "hdop", fix.hdop);
            gr::tag::put(meta, "fix_type", std::string(fixTypeName(fix.fixType)));
            gr::tag::put(meta, "speed_kmh", fix.speedKmh);
            gr::tag::put(meta, "heading_deg", fix.headingDeg);
            if (emit_device_info.value) {
                gr::tag::put(meta, "device_info", device_name.value);
            }
            gr::tag::put(tag, gr::tag::TRIGGER_META_INFO, std::move(meta));
        }
        if (!context.value.empty()) {
            gr::tag::put(tag, gr::tag::CONTEXT, context.value);
        }
        return tag;
    }

    /*| contract: runs on the io thread until the stop request. It reads the port, splits the
            text into lines and queues each second the parser closes, then moves the graph's
            progress counter so the scheduler calls work().
        frame: every line of one read takes the time the read returned as its arrival time.
            The wait ends at the first byte, so the first line of a second is stamped when its
            bytes reach the host.
    */
    void ioLoop(std::string path, BaudRate rate, std::chrono::milliseconds wait) {
        gr::thread_pool::thread::setThreadName("timesource-gps");
        std::string              lines;
        std::array<char, 512UZ>  chunk{};
        std::string              device   = path.empty() ? std::string{} : shownName(path, _port.info());
        bool                     reported = false;
        std::vector<std::string> passedOver;
        bool                     heard           = false;
        bool                     silenceReported = false;
        auto                     openedAt        = std::chrono::steady_clock::now();
        std::size_t              checkedAtOpen   = 0;
        while (!_io.stopRequested()) {
            if (!_port.isOpen()) {
                auto opened = path.empty() ? openDetectedReceiver(rate, passedOver) : SerialPort::open(path, rate);
                if (!opened) {
                    if (!reported) {
                        std::fprintf(stderr, "[%.*s] %s; retrying each second\n", static_cast<int>(kTag.size()), kTag.data(), opened.error().c_str());
                        reported = true;
                    }
                    _io.waitFor(kRetryWait);
                    continue;
                }
                _port    = std::move(*opened);
                device   = path.empty() ? describe(_port.info()) : shownName(path, _port.info());
                reported = false;
                lines.clear();
                if (std::ranges::contains(passedOver, _port.info().path)) {
                    passedOver.clear(); // every ranked port was passed over
                }
                heard         = false;
                openedAt      = std::chrono::steady_clock::now();
                checkedAtOpen = _parser.counts().checked;
            }
            if (path.empty() && !heard) {
                if (_parser.counts().checked > checkedAtOpen) {
                    heard           = true;
                    silenceReported = false;
                } else if (std::chrono::steady_clock::now() - openedAt >= _silentLimit) {
                    if (!silenceReported) {
                        std::fprintf(stderr, "[%.*s] %s gave no checked NMEA sentence in %lld ms; trying the next receiver\n", static_cast<int>(kTag.size()), kTag.data(), _port.info().path.c_str(), static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(_silentLimit).count()));
                        silenceReported = true;
                    }
                    passedOver.push_back(_port.info().path);
                    _port.close();
                    continue;
                }
            }
            const auto got = _port.read(chunk, wait);
            if (!got) {
                std::fprintf(stderr, "[%.*s] %s; reopening\n", static_cast<int>(kTag.size()), kTag.data(), got.error().c_str());
                _port.close();
                continue;
            }
            if (*got == 0) {
                continue;
            }
            const std::uint64_t arrived = wallClockNs();
            lines.append(chunk.data(), *got);
            std::size_t begin = 0;
            for (std::size_t end = lines.find('\n'); end != std::string::npos; end = lines.find('\n', begin)) {
                if (auto edge = _parser.parseLine(std::string_view(lines).substr(begin, end - begin), arrived)) {
                    _ticks.push(GpsTick{.edge = *edge, .device = device});
                    this->progress->incrementAndGet();
                    this->progress->notify_all();
                }
                begin = end + 1;
            }
            lines.erase(0, begin);
            if (lines.size() > kMaxLine) {
                lines.clear();
            }
        }
    }
};

} // namespace timesource
