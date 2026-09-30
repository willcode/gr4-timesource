/* -*- c++ -*- */
/*
 * Copyright 2026 Jeff Long
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <timesource/KernelClock.hpp>
#include <timesource/NmeaParser.hpp>
#include <timesource/TickStream.hpp>

#include <gnuradio-4.0/Block.hpp>
#include <gnuradio-4.0/Tag.hpp>
#include <gnuradio-4.0/annotated.hpp>
#include <gnuradio-4.0/thread/thread_affinity.hpp>

#include <cstdint>
#include <cstdio>
#include <expected>
#include <format>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace timesource {

/*| role: one second the clock marked, handed from the io thread to the scheduler thread.
    frame: triggerNs is the second in UTC, and in PTP mode the PTP clock's own second, which
        makeTag() moves to UTC. The TAI clock's second is moved to UTC by the kernel's TAI
        offset. ptpLessSystemNs holds in PTP mode alone, read just after the mark.
*/
struct PpsTick {
    std::uint64_t               triggerNs      = 0;
    std::int64_t                wakeupOffsetNs = 0;
    std::uint64_t               sequence       = 0;
    KernelDiscipline            discipline;
    std::optional<std::int64_t> ptpLessSystemNs;
};

/*| role: the seconds of a kernel clock or a PPS device as a stream of tagged samples.
    contract: the tag of a second carries TRIGGER_NAME, "<trigger_name>_<mode>", with the
        suffix " (unlocked)" on an unlocked second; TRIGGER_TIME; TRIGGER_OFFSET; the
        discipline under TRIGGER_META_INFO where emit_meta_info holds; and CONTEXT where
        context is set.
    contract: TRIGGER_TIME is the UTC second in every mode. The TAI clock's second is moved
        back by the kernel's TAI offset. A PTP clock on ptp_time_scale TAI is moved back by the
        TAI offset ntp_adjtime() reports, or by tai_utc_offset_s where the kernel reports 0. A
        second is locked where ntp_adjtime() reports the system clock synchronized. In HwPps
        mode the pulse must also have arrived. In PTP mode the two clocks, on one scale, must
        also differ by less than ptp_offset_limit_ns, as ptpLocked() states.
    contract: start() refuses a clock mode the platform or the device cannot give, naming why.
        Auto is the system clock, as in NTP mode.
    frame: the io thread waits for each second and reads the discipline, and in PTP mode the
        PTP clock's offset from the system clock. The scheduler thread writes the samples and
        tags and applies the settings. clock_mode and the device indices take effect at the
        next start. ptp_time_scale, tai_utc_offset_s and ptp_offset_limit_ns take effect at
        the next tag.
    frame: start() opens the clock through _openClock, which a case replaces to answer the
        kernel's calls.
    invariant: _io is the last member, so the io thread ends before the clock it waits on.
    verified-by: pps.ntp-emits-one-tag-per-second
    verified-by: pps.a-ptp-block-tags-the-utc-second
*/
struct PpsSource : gr::Block<PpsSource> {
    using Description = gr::Doc<R"doc(@brief Emits a tagged sample at each second of a kernel clock or a PPS device.

The clock is the system clock as NTP disciplines it (NTP), a PTP hardware clock /dev/ptpN (PTP), the kernel's TAI clock (TAI), or the assert edge of a PPS device /dev/ppsN (HwPps). Auto is the system clock, as in NTP mode. PTP and HwPps are taken by name alone. At each whole second the block places a tag carrying trigger_name and the clock mode, the UTC second, and the kernel's discipline as ntp_adjtime reports it: synchronization, offset, error bounds, frequency, jitter, the TAI offset and the leap state. ptp_time_scale names the scale of the PTP clock, TAI by default. The block moves a TAI second to UTC by the TAI offset the kernel reports, or by tai_utc_offset_s where the kernel reports 0. A second is locked where the kernel reports the system clock synchronized. HwPps also needs the pulse. PTP also needs the PTP clock, moved to UTC, within ptp_offset_limit_ns of the system clock. An unlocked second's tag name ends in " (unlocked)". In ppsOnly mode the block writes one sample per second; in clock mode it writes sample_rate samples per second, the tag on the first.)doc">;

    gr::PortOut<std::uint8_t> out;

    gr::Annotated<ClockMode, "clock_mode", gr::Visible, gr::Doc<"clock marked, 'NTP', 'PTP', 'TAI', 'HwPps' or 'Auto'">>                                 clock_mode          = ClockMode::Auto;
    gr::Annotated<std::uint8_t, "ptp_device_index", gr::Visible, gr::Doc<"N of /dev/ptpN in PTP mode">>                                                  ptp_device_index    = 0;
    gr::Annotated<std::uint8_t, "pps_device_index", gr::Visible, gr::Doc<"N of /dev/ppsN in HwPps mode">>                                                pps_device_index    = 0;
    gr::Annotated<TimeScale, "ptp_time_scale", gr::Visible, gr::Doc<"time scale of the PTP clock, 'TAI' or 'UTC'">>                                     ptp_time_scale      = TimeScale::TAI;
    gr::Annotated<std::int32_t, "tai_utc_offset_s", gr::Unit<"s">, gr::Doc<"fallback TAI-UTC offset where the kernel reports 0">>                        tai_utc_offset_s    = 37;
    gr::Annotated<std::uint64_t, "ptp_offset_limit_ns", gr::Visible, gr::Unit<"ns">, gr::Doc<"bound on a locked PTP clock's offset from the system clock">> ptp_offset_limit_ns = 1'000'000ULL;
    gr::Annotated<std::string, "trigger_name", gr::Doc<"tag trigger name prefix">>                                                                       trigger_name        = std::string("PPS");
    gr::Annotated<std::string, "context", gr::Doc<"context tag value">>                                                                                  context;
    gr::Annotated<EmitMode, "emit_mode", gr::Visible, gr::Doc<"output shape, 'ppsOnly' or 'clock'">>                                                     emit_mode           = EmitMode::ppsOnly;
    gr::Annotated<float, "sample_rate", gr::Visible, gr::Unit<"Hz">, gr::Doc<"samples per second in clock mode">, gr::Limits<1.f, 100e6f>>               sample_rate         = 1.f;
    gr::Annotated<bool, "emit_meta_info", gr::Doc<"attach the kernel discipline as trigger_meta_info">>                                                  emit_meta_info      = true;

    GR_MAKE_REFLECTABLE(PpsSource, out, clock_mode, ptp_device_index, pps_device_index, ptp_time_scale, tai_utc_offset_s, ptp_offset_limit_ns, trigger_name, context, emit_mode, sample_rate, emit_meta_info);

    static constexpr std::string_view kTag = "timesource::PpsSource";

    std::function<std::expected<ClockSource, std::string>(ClockMode, std::uint8_t, std::uint8_t)> _openClock = &ClockSource::open;

    ClockSource         _clock;
    TickQueue<PpsTick>  _ticks;
    TickWriter<PpsTick> _writer;
    std::string         _modeName = std::string(clockModeName(ClockMode::NTP));
    IoThread            _io; // last: the io thread ends before the clock it waits on

    void start() {
        _io.stop();
        _ticks.clear();
        _writer.reset();
        auto opened = _openClock(clock_mode.value, ptp_device_index.value, pps_device_index.value);
        if (!opened) {
            throwStartFailure(kTag, opened.error());
        }
        _clock    = std::move(*opened);
        _modeName = std::string(clockModeName(_clock.mode()));
        _io.start([this] { ioLoop(); });
    }

    void stop() {
        _io.stop();
        _clock = ClockSource{};
    }

    gr::work::Result work(std::size_t requestedWork = std::numeric_limits<std::size_t>::max()) noexcept {
        if (!gr::lifecycle::isActive(this->state())) {
            return {requestedWork, 0UZ, gr::work::Status::DONE};
        }
        this->applyChangedSettings();
        const std::size_t written = _writer.write(*this, _ticks, samplesPerSecond(emit_mode.value, sample_rate.value), [this](PpsTick& tick) { return makeTag(tick); });
        return {requestedWork, written, gr::work::Status::OK};
    }

    [[nodiscard]] gr::property_map makeTag(const PpsTick& tick) {
        const KernelDiscipline& d         = tick.discipline;
        bool                    locked    = d.synchronized;
        std::uint64_t           triggerNs = tick.triggerNs;
        if (tick.ptpLessSystemNs) {
            const std::int32_t taiUtcS = taiUtcOffsetS(d, tai_utc_offset_s.value);
            locked                     = ptpLocked(d, *tick.ptpLessSystemNs, ptp_time_scale.value, taiUtcS, ptp_offset_limit_ns.value);
            triggerNs                  = scaleToUtcNs(tick.triggerNs, ptp_time_scale.value, taiUtcS);
        }
        auto tag = out.makeTagMap();
        gr::tag::put(tag, gr::tag::TRIGGER_NAME, locked ? std::format("{}_{}", trigger_name.value, _modeName) : std::format("{}_{} (unlocked)", trigger_name.value, _modeName));
        gr::tag::put(tag, gr::tag::TRIGGER_TIME, triggerNs);
        gr::tag::put(tag, gr::tag::TRIGGER_OFFSET, 0.f);
        if (emit_meta_info.value) {
            auto meta = out.makeTagMap();
            gr::tag::put(meta, "clock_mode", _modeName);
            gr::tag::put(meta, "wakeup_offset_ns", tick.wakeupOffsetNs);
            gr::tag::put(meta, "sequence", tick.sequence);
            gr::tag::put(meta, "synchronised", d.synchronized);
            gr::tag::put(meta, "kernel_offset_ns", d.offsetNs);
            gr::tag::put(meta, "est_error_ns", d.estErrorNs);
            gr::tag::put(meta, "max_error_ns", d.maxErrorNs);
            gr::tag::put(meta, "freq_ppb", d.freqPpb);
            gr::tag::put(meta, "jitter_ns", d.jitterNs);
            gr::tag::put(meta, "tai_utc_offset_s", d.taiUtcOffsetS);
            gr::tag::put(meta, "leap_status", std::string(leapStatusName(d.leapStatus)));
            gr::tag::put(tag, gr::tag::TRIGGER_META_INFO, std::move(meta));
        }
        if (!context.value.empty()) {
            gr::tag::put(tag, gr::tag::CONTEXT, context.value);
        }
        return tag;
    }

    void ioLoop() {
        gr::thread_pool::thread::setThreadName("timesource-pps");
        std::uint64_t sequence = 0;
        while (!_io.stopRequested()) {
            const auto pulse = _clock.next([this] { return _io.stopRequested(); });
            if (!pulse) {
                continue;
            }
            if (const auto fault = _clock.takeFault()) {
                std::fprintf(stderr, "[%.*s] %s; each second without a pulse is tagged unlocked\n", static_cast<int>(kTag.size()), kTag.data(), fault->c_str());
            }
            PpsTick tick{.triggerNs = pulse->nominalNs, .wakeupOffsetNs = pulse->wakeupOffsetNs, .sequence = sequence++, .discipline = _clock.discipline(), .ptpLessSystemNs = _clock.ptpLessSystemNs()};
            if (pulse->missed) {
                tick.discipline.synchronized = false;
            }
            if (_clock.mode() == ClockMode::TAI) {
                tick.triggerNs = scaleToUtcNs(tick.triggerNs, TimeScale::TAI, tick.discipline.taiUtcOffsetS);
            }
            _ticks.push(tick);
            this->progress->incrementAndGet();
            this->progress->notify_all();
        }
    }
};

} // namespace timesource
