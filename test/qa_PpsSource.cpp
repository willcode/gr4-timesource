/* -*- c++ -*- */
/*
 * Copyright 2026 Jeff Long
 * SPDX-License-Identifier: MIT
 */
#include <timesource/PpsSource.hpp>

#include "support/Cases.hpp"
#include "support/Graph.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <format>
#include <string>
#include <tuple>
#include <vector>

namespace {

using namespace std::chrono_literals;
using timesource::test::RecordingSink;
using timesource::test::ScheduledGraph;
using timesource::test::value;

constexpr std::uint64_t kS  = 1'000'000'000ULL;
constexpr std::uint64_t kMs = 1'000'000ULL;

// The clock id a case's PTP clock stands under; the case's calls answer it.
constexpr clockid_t kFakePtp = CLOCK_MONOTONIC;

timesource::KernelDiscipline synchronizedDiscipline(std::int32_t taiUtcS) {
    timesource::KernelDiscipline d;
    d.synchronized  = true;
    d.taiUtcOffsetS = taiUtcS;
    d.leapStatus    = TIME_OK;
    return d;
}

/*| role: the kernel calls of a ClockSource, answered on synthetic time.
    contract: each sleep advances the marked clock and the monotonic clock by its duration. The
        sleep numbered stepAtSleep, counted from 1, then steps the marked clock by stepNs. A PPS
        fetch answers fetchError, or else the assert edge at edge with sequence.
*/
struct FakeKernel {
    std::uint64_t              clockNs     = 0;
    std::uint64_t              steadyNs    = kS;
    std::vector<std::uint64_t> sleeps;
    std::size_t                stepAtSleep = 0;
    std::int64_t               stepNs      = 0;
    std::size_t                fetches     = 0;
    int                        fetchError  = 0;
    unsigned long              sequence    = 0;
    timespec                   edge{};

    timesource::ClockCalls calls() {
        timesource::ClockCalls calls;
        calls.readNs   = [this](clockid_t) { return clockNs; };
        calls.steadyNs = [this] { return steadyNs; };
        calls.sleepNs  = [this](std::uint64_t ns) {
            sleeps.push_back(ns);
            clockNs += ns;
            steadyNs += ns;
            if (sleeps.size() == stepAtSleep) {
                clockNs = static_cast<std::uint64_t>(static_cast<std::int64_t>(clockNs) + stepNs);
            }
        };
        calls.discipline = [] { return synchronizedDiscipline(0); };
#if defined(TIMESOURCE_HAVE_TIMEPPS)
        calls.fetchPps = [this](pps_handle_t, pps_info_t& info, std::uint64_t) {
            ++fetches;
            if (fetchError != 0) {
                return fetchError;
            }
            info.assert_sequence  = sequence;
            info.assert_timestamp = edge;
            return 0;
        };
#endif
        return calls;
    }
};

timesource::test::RunEnd runPps(timesource::ClockMode mode, const char* indexKey) {
    gr::Graph flow;
    auto&     pps  = flow.emplaceBlock<timesource::PpsSource>({{"clock_mode", std::string(timesource::clockModeName(mode))}, {indexKey, std::uint8_t{255}}});
    auto&     sink = flow.emplaceBlock<RecordingSink>();
    if (!flow.connect<"out", "in">(pps, sink).has_value()) {
        return {};
    }
    return timesource::test::runToEnd(std::move(flow));
}

} // namespace

int main(int argc, char** argv) {
    using namespace boost::ut;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    timesource::test::Cases cases(argc, argv);

    cases("pps.ntp-emits-one-tag-per-second", [] {
        gr::Graph flow;
        auto&     pps  = flow.emplaceBlock<timesource::PpsSource>({{"clock_mode", std::string("NTP")}});
        auto&     sink = flow.emplaceBlock<RecordingSink>();
        expect(fatal(flow.connect<"out", "in">(pps, sink).has_value()));
        ScheduledGraph scheduled;
        expect(fatal(scheduled.adopt(std::move(flow)).has_value()));
        scheduled.start();
        const bool arrived = sink.awaitSamples(2UZ, 3500ms);
        scheduled.stop();
        const auto host = timesource::wallClockNs();
        expect(fatal(arrived)) << "two seconds pass within the bound";
        const auto tags = sink.triggers();
        expect(fatal(tags.size() >= 2UZ));
        for (std::size_t i = 0; i < 2; ++i) {
            const auto& tag  = tags[i].map;
            const auto  time = value<std::uint64_t>(tag, "trigger_time").value_or(0);
            expect(eq(tags[i].index, i)) << "one sample per second, the tag on it";
            expect(time % 1'000'000'000ULL == 0ULL) << "the tag marks a whole second";
            expect(time <= host && host - time < 4'000'000'000ULL) << "a second of this run";
            const auto meta = value<gr::property_map>(tag, "trigger_meta_info");
            expect(fatal(meta.has_value())) << "the discipline rides along";
            const bool synchronized = value<bool>(*meta, "synchronised").value_or(false);
            expect(value<std::string>(tag, "trigger_name") == std::optional<std::string>{synchronized ? "PPS_NTP" : "PPS_NTP (unlocked)"}) << "the name follows the kernel's state";
            expect(value<std::string>(*meta, "clock_mode") == std::optional<std::string>{"NTP"});
            expect(value<std::uint64_t>(*meta, "sequence") == std::optional<std::uint64_t>{i}) << "the seconds are counted from the start";
            const auto wakeup = value<std::int64_t>(*meta, "wakeup_offset_ns");
            expect(wakeup.has_value() && *wakeup >= 0 && *wakeup < 1'000'000'000) << "the thread woke after the second and within it";
            for (const char* key : {"kernel_offset_ns", "est_error_ns", "max_error_ns", "freq_ppb", "jitter_ns"}) {
                expect(value<std::int64_t>(*meta, key).has_value()) << key << " is present";
            }
            expect(value<std::int32_t>(*meta, "tai_utc_offset_s").has_value());
            expect(value<std::string>(*meta, "leap_status").has_value());
        }
        const auto first  = value<std::uint64_t>(tags[0].map, "trigger_time").value_or(0);
        const auto second = value<std::uint64_t>(tags[1].map, "trigger_time").value_or(0);
        expect(eq(second - first, 1'000'000'000ULL)) << "consecutive seconds";
    });

    cases("pps.ptp-without-a-device-is-unavailable", [] {
        const auto end = runPps(timesource::ClockMode::PTP, "ptp_device_index");
        expect(end.returned && end.failed) << "the start is refused and the run ends";
#if defined(__linux__)
        expect(end.error.contains("clock mode PTP is unavailable: cannot open /dev/ptp255")) << "naming the clock it could not open: " << end.error;
#else
        expect(end.error.contains("clock mode PTP is unavailable")) << end.error;
#endif
    });

    cases("pps.hardware-pps-without-a-device-is-unavailable", [] {
        const auto end = runPps(timesource::ClockMode::HwPps, "pps_device_index");
        expect(end.returned && end.failed) << "the start is refused and the run ends";
#if defined(TIMESOURCE_HAVE_TIMEPPS)
        expect(end.error.contains("clock mode HwPps is unavailable: cannot open /dev/pps255")) << "naming the device it could not open: " << end.error;
#else
        expect(end.error.contains("clock mode HwPps is unavailable: the build found no RFC 2783 sys/timepps.h")) << end.error;
#endif
    });

    cases("pps.discipline-units", [] {
        struct timex tx{};
        tx.status   = 0;
        tx.offset   = -12;
        tx.jitter   = 7;
        tx.esterror = 5;
        tx.maxerror = 16000;
        tx.freq     = -3 * 65536;
#if defined(TIMESOURCE_HAVE_TIMEX_TAI)
        tx.tai = 37;
#endif
        const auto micro = timesource::disciplineFrom(tx, TIME_OK);
        expect(micro.synchronized) << "STA_UNSYNC clear and the state good";
        expect(eq(micro.offsetNs, -12'000LL) && eq(micro.jitterNs, 7'000LL)) << "offset and jitter in microseconds without STA_NANO";
        expect(eq(micro.estErrorNs, 5'000LL) && eq(micro.maxErrorNs, 16'000'000LL)) << "the error bounds are always microseconds";
        expect(eq(micro.freqPpb, -3'000LL)) << "parts per million with a 16-bit fraction";
#if defined(TIMESOURCE_HAVE_TIMEX_TAI)
        expect(eq(micro.taiUtcOffsetS, 37));
#endif
        tx.status       = STA_NANO | STA_UNSYNC;
        const auto nano = timesource::disciplineFrom(tx, TIME_OK);
        expect(eq(nano.offsetNs, -12LL) && eq(nano.jitterNs, 7LL)) << "nanoseconds with STA_NANO";
        expect(!nano.synchronized) << "STA_UNSYNC set";
        tx.status = 0;
        expect(!timesource::disciplineFrom(tx, TIME_ERROR).synchronized) << "the error state";
        const auto now = timesource::queryKernelDiscipline();
        expect(now.maxErrorNs >= 0 && now.estErrorNs >= 0) << "the kernel's own reading has sane bounds";
    });

    cases("pps.ptp-locks-to-a-disciplined-system-clock", [] {
        using timesource::TimeScale;
        struct timex tx{};
        tx.status                      = 0;
        const auto              synced = timesource::disciplineFrom(tx, TIME_OK);
        constexpr std::uint64_t limit  = 1'000'000ULL;
        expect(timesource::ptpLocked(synced, 400'000LL, TimeScale::UTC, 37, limit)) << "a PTP clock on UTC within the limit";
        expect(timesource::ptpLocked(synced, -999'999LL, TimeScale::UTC, 37, limit)) << "one nanosecond inside the limit";
        expect(!timesource::ptpLocked(synced, -1'000'000LL, TimeScale::UTC, 37, limit)) << "the limit itself is unlocked";
        expect(!timesource::ptpLocked(synced, -5'000'000'000'000LL, TimeScale::UTC, 37, limit)) << "a free-running PTP clock far from the system clock";
        tx.status = STA_UNSYNC;
        expect(!timesource::ptpLocked(timesource::disciplineFrom(tx, TIME_OK), 0LL, TimeScale::UTC, 37, limit)) << "an unsynchronized system clock locks no PTP second";
        tx.status = 0;
        expect(!timesource::ptpLocked(timesource::disciplineFrom(tx, TIME_ERROR), 0LL, TimeScale::UTC, 37, limit)) << "nor does the error state";
        gr::Graph flow;
        expect(eq(flow.emplaceBlock<timesource::PpsSource>().ptp_offset_limit_ns.value, 1'000'000ULL)) << "the default limit is one millisecond";
    });

    cases("pps.a-ptp-clock-on-tai-takes-the-kernel-offset-or-the-fallback", [] {
        using timesource::TimeScale;
        constexpr std::uint64_t limit    = 1'000'000ULL;
        constexpr std::uint64_t utc      = 1'700'000'000ULL * kS;
        const auto              kernel0  = synchronizedDiscipline(0);
        const auto              kernel37 = synchronizedDiscipline(37);

        expect(eq(timesource::taiUtcOffsetS(kernel0, 37), 37)) << "the fallback where the kernel reports 0";
        expect(timesource::ptpLocked(kernel0, 37'000'250'000LL, TimeScale::TAI, timesource::taiUtcOffsetS(kernel0, 37), limit)) << "a TAI clock locks on the fallback offset";
        expect(!timesource::ptpLocked(kernel0, 250'000LL, TimeScale::TAI, timesource::taiUtcOffsetS(kernel0, 37), limit)) << "a clock reading UTC is no TAI clock";
        expect(eq(timesource::scaleToUtcNs(utc + 37ULL * kS, TimeScale::TAI, timesource::taiUtcOffsetS(kernel0, 37)), utc)) << "its second moves back to UTC by the fallback";

        expect(eq(timesource::taiUtcOffsetS(kernel37, 10), 37)) << "the kernel's offset where it reports one";
        expect(timesource::ptpLocked(kernel37, 36'999'750'000LL, TimeScale::TAI, timesource::taiUtcOffsetS(kernel37, 10), limit)) << "a TAI clock locks on the kernel's offset";
        expect(!timesource::ptpLocked(kernel37, 10'000'000'000LL, TimeScale::TAI, timesource::taiUtcOffsetS(kernel37, 10), limit)) << "and the fallback goes unused";
        expect(eq(timesource::scaleToUtcNs(utc + 37ULL * kS, TimeScale::TAI, timesource::taiUtcOffsetS(kernel37, 10)), utc)) << "its second moves back by the kernel's offset";

        expect(timesource::ptpLocked(kernel37, 250'000LL, TimeScale::UTC, 37, limit)) << "a UTC clock locks against the system clock alone";
        expect(!timesource::ptpLocked(kernel37, 37'000'000'000LL, TimeScale::UTC, 37, limit)) << "a TAI reading is 37 s off on the UTC scale";
        expect(eq(timesource::scaleToUtcNs(utc, TimeScale::UTC, 37), utc)) << "a UTC second stays";

        gr::Graph flow;
        const auto& defaults = flow.emplaceBlock<timesource::PpsSource>();
        expect(defaults.ptp_time_scale.value == TimeScale::TAI && eq(defaults.tai_utc_offset_s.value, 37)) << "TAI and a 37 s fallback by default";
        expect(flow.emplaceBlock<timesource::PpsSource>({{"ptp_time_scale", std::string("UTC")}}).ptp_time_scale.value == TimeScale::UTC) << "the scale is set by name";
    });

    cases("pps.ptp-offset-reads-both-clocks", [] {
        std::size_t            systemReads = 0;
        timesource::ClockCalls calls;
        calls.readNs = [&systemReads](clockid_t clock) {
            if (clock == CLOCK_REALTIME) {
                return 1'000ULL * kS + (systemReads++ == 0 ? 0ULL : 10ULL);
            }
            return 1'037ULL * kS + 5ULL;
        };
        const auto ptp = timesource::ClockSource::through(timesource::ClockMode::PTP, kFakePtp, calls);
        expect(ptp.ptpLessSystemNs() == std::optional<std::int64_t>{37'000'000'000LL}) << "the PTP reading less the mean of the system readings around it";
        expect(eq(systemReads, 2UZ)) << "the system clock is read before and after";
        expect(!timesource::ClockSource::through(timesource::ClockMode::NTP, CLOCK_REALTIME, calls).ptpLessSystemNs().has_value()) << "nothing outside PTP mode";
#if defined(CLOCK_TAI) && defined(TIMESOURCE_HAVE_TIMEX_TAI)
        const auto tai    = timesource::ClockSource::through(timesource::ClockMode::PTP, CLOCK_TAI, timesource::ClockCalls{});
        const auto offset = tai.ptpLessSystemNs().value_or(0);
        const auto ahead  = static_cast<std::int64_t>(timesource::queryKernelDiscipline().taiUtcOffsetS) * 1'000'000'000LL;
        expect(offset - ahead > -50'000'000LL && offset - ahead < 50'000'000LL) << "the kernel's TAI clock reads its TAI offset ahead: " << offset;
#endif
    });

    cases("pps.a-ptp-block-tags-the-utc-second", [] {
        gr::Graph flow;
        auto&     pps = flow.emplaceBlock<timesource::PpsSource>({{"clock_mode", std::string("PTP")}});
        pps._openClock = [](timesource::ClockMode, std::uint8_t, std::uint8_t) -> std::expected<timesource::ClockSource, std::string> {
            timesource::ClockCalls calls;
            calls.readNs = [](clockid_t clock) {
                timespec now{};
                ::clock_gettime(CLOCK_REALTIME, &now);
                return timesource::toNs(now) + (clock == CLOCK_REALTIME ? 0ULL : 37ULL * kS + 200'000ULL);
            };
            calls.discipline = [] { return synchronizedDiscipline(0); };
            return timesource::ClockSource::through(timesource::ClockMode::PTP, kFakePtp, std::move(calls), "/dev/ptp-fake");
        };
        auto& sink = flow.emplaceBlock<RecordingSink>();
        expect(fatal(flow.connect<"out", "in">(pps, sink).has_value()));
        ScheduledGraph scheduled;
        expect(fatal(scheduled.adopt(std::move(flow)).has_value()));
        scheduled.start();
        const bool arrived = sink.awaitSamples(2UZ, 3500ms);
        scheduled.stop();
        const auto host = timesource::wallClockNs();
        expect(fatal(arrived)) << "two seconds pass within the bound";
        const auto tags = sink.triggers();
        expect(fatal(tags.size() >= 2UZ));
        for (std::size_t i = 0; i < 2; ++i) {
            const auto time = value<std::uint64_t>(tags[i].map, "trigger_time").value_or(0);
            expect(value<std::string>(tags[i].map, "trigger_name") == std::optional<std::string>{"PPS_PTP"}) << "a TAI clock 37 s and 0.2 ms ahead locks on the fallback offset";
            expect(time % kS == 0ULL) << "the tag marks a whole second";
            expect(time <= host && host - time < 4ULL * kS) << "the UTC second, not the PTP clock's TAI second";
        }
        expect(eq(value<std::uint64_t>(tags[1].map, "trigger_time").value_or(0) - value<std::uint64_t>(tags[0].map, "trigger_time").value_or(0), kS)) << "consecutive seconds";
    });

    cases("pps.a-clock-step-keeps-the-seconds-in-order", [] {
        FakeKernel k;
        k.clockNs            = 100ULL * kS + 300ULL * kMs;
        k.stepAtSleep        = 2;
        k.stepNs             = -2'500'000'000LL;
        auto       source    = timesource::ClockSource::through(timesource::ClockMode::NTP, CLOCK_REALTIME, k.calls());
        const auto never     = [] { return false; };
        const auto stepped   = source.next(never);
        expect(fatal(stepped.has_value()));
        expect(eq(stepped->nominalNs, 101ULL * kS)) << "a step back during the wait keeps the mark";
        k.clockNs            = 100ULL * kS + 200ULL * kMs;
        const auto afterBack = source.next(never);
        expect(fatal(afterBack.has_value()));
        expect(eq(afterBack->nominalNs, 102ULL * kS)) << "a step back between seconds answers the second after the last one";
        k.stepAtSleep        = k.sleeps.size() + 1;
        k.stepNs             = 5'000'000'000LL;
        const auto forward   = source.next(never);
        expect(fatal(forward.has_value()));
        expect(eq(forward->nominalNs, 107ULL * kS) && eq(forward->wakeupOffsetNs, static_cast<std::int64_t>(100ULL * kMs))) << "a step forward answers the second of the reading";
        expect(std::ranges::all_of(k.sleeps, [](std::uint64_t ns) { return ns > 0 && ns <= 100ULL * kMs; })) << "every sleep is a slice of at most 100 ms";
        k.clockNs            = 50ULL * kS;
        int        polls     = 0;
        const auto farBack   = source.next([&polls] { return ++polls > 3; });
        expect(!farBack.has_value()) << "a stop ends the wait of a clock stepped far back";
    });

    cases("pps.a-fetch-error-is-paced-and-reported-once", [] {
#if defined(TIMESOURCE_HAVE_TIMEPPS)
        FakeKernel k;
        k.clockNs            = 200ULL * kS + 400ULL * kMs;
        k.fetchError         = EIO;
        auto       source    = timesource::ClockSource::through(timesource::ClockMode::HwPps, CLOCK_REALTIME, k.calls(), "/dev/pps-fake");
        const auto never     = [] { return false; };
        const auto missed    = source.next(never);
        expect(fatal(missed.has_value()));
        expect(missed->missed) << "the deadline answers a missed pulse";
        expect(eq(k.fetches, 11UZ)) << "one fetch per 100 ms slice until 1.1 s pass";
        expect(eq(k.sleeps.size(), 11UZ) && std::ranges::all_of(k.sleeps, [](std::uint64_t ns) { return ns == 100ULL * kMs; })) << "each error sleeps out its slice";
        expect(eq(missed->nominalNs, 201ULL * kS + 500ULL * kMs)) << "at the host's time";
        expect(source.takeFault() == std::optional<std::string>{std::format("fetching a pulse from /dev/pps-fake failed: {}", std::strerror(EIO))}) << "the first error, named";
        expect(!source.takeFault().has_value()) << "answered once";
        std::ignore = source.next(never);
        expect(!source.takeFault().has_value()) << "the same run of errors is not reported again";
        k.fetchError         = 0;
        k.sequence           = 1;
        k.edge               = timespec{.tv_sec = 201, .tv_nsec = 999'999'000};
        const auto pulse     = source.next(never);
        expect(fatal(pulse.has_value()));
        expect(!pulse->missed && eq(pulse->nominalNs, 202ULL * kS) && eq(pulse->wakeupOffsetNs, -1'000LL)) << "a pulse ends the run";
        k.fetchError         = EIO;
        std::ignore          = source.next(never);
        expect(source.takeFault().has_value()) << "the next error starts a new run and is reported";
#endif
    });

    cases("pps.auto-is-the-system-clock", [] {
        const auto opened = timesource::ClockSource::open(timesource::ClockMode::Auto, 255, 255);
        expect(fatal(opened.has_value())) << "Auto always starts";
        expect(opened->mode() == timesource::ClockMode::NTP) << "the system clock";
        expect(!opened->ptpLessSystemNs().has_value()) << "no PTP offset outside PTP mode";
        gr::Graph flow;
        expect(flow.emplaceBlock<timesource::PpsSource>().clock_mode.value == timesource::ClockMode::Auto) << "Auto is the default";
    });

    cases("pps.names-modes-and-leap-states", [] {
        using timesource::ClockMode;
        expect(timesource::clockModeName(ClockMode::NTP) == "NTP" && timesource::clockModeName(ClockMode::PTP) == "PTP" && timesource::clockModeName(ClockMode::TAI) == "TAI" && timesource::clockModeName(ClockMode::HwPps) == "HwPps" && timesource::clockModeName(ClockMode::Auto) == "Auto");
        expect(timesource::leapStatusName(TIME_OK) == "OK" && timesource::leapStatusName(TIME_INS) == "insert_leap" && timesource::leapStatusName(TIME_DEL) == "delete_leap" && timesource::leapStatusName(TIME_OOP) == "leap_in_progress" && timesource::leapStatusName(TIME_WAIT) == "leap_occurred" && timesource::leapStatusName(TIME_ERROR) == "unsynchronised");
    });

    cases("pps.a-pulse-marks-its-nearest-second", [] {
        const auto late = timesource::pulseAt(100, 250);
        expect(eq(late.nominalNs, 100'000'000'000ULL) && eq(late.wakeupOffsetNs, 250LL)) << "an edge just after the second";
        const auto early = timesource::pulseAt(100, 999'999'000);
        expect(eq(early.nominalNs, 101'000'000'000ULL) && eq(early.wakeupOffsetNs, -1'000LL)) << "an edge just before the next second";
    });

    cases("pps.time-to-the-next-second", [] {
        expect(eq(timesource::nsToNextSecond(5'000'000'000ULL), 1'000'000'000ULL)) << "on a second, the next whole one";
        expect(eq(timesource::nsToNextSecond(5'999'999'999ULL), 1ULL));
        expect(eq(timesource::nsToNextSecond(5'250'000'000ULL), 750'000'000ULL));
    });

    cases("pps.samples-per-second-follow-the-mode", [] {
        using timesource::EmitMode;
        expect(eq(timesource::samplesPerSecond(EmitMode::ppsOnly, 1000.f), 1UZ)) << "ppsOnly ignores the rate";
        expect(eq(timesource::samplesPerSecond(EmitMode::clock, 1000.f), 1000UZ));
        expect(eq(timesource::samplesPerSecond(EmitMode::clock, 99.6f), 100UZ)) << "rounded to whole samples";
        expect(eq(timesource::samplesPerSecond(EmitMode::clock, 0.f), 1UZ)) << "one at the least";
    });

    const int rc = cases.finish();
    std::fflush(stdout);
    std::_Exit(rc);
}
