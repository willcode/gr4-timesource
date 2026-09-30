/* -*- c++ -*- */
/*
 * Copyright 2026 Jeff Long
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <expected>
#include <format>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <fcntl.h>
#include <sys/timex.h>
#include <time.h>
#include <unistd.h>

#if defined(TIMESOURCE_HAVE_TIMEPPS)
#include <sys/timepps.h>
#endif

namespace timesource {

/*| role: the clock a PpsSource marks the seconds of.
    contract: NTP is the host's system clock as NTP or another daemon disciplines it. PTP is a
        PTP hardware clock, /dev/ptpN. TAI is the kernel's TAI clock. HwPps is the assert edge
        of a pulse-per-second device, /dev/ppsN. Auto is the system clock, as in NTP mode. PTP
        and HwPps are taken by name alone.
*/
enum class ClockMode : std::uint8_t { NTP, PTP, TAI, HwPps, Auto };

[[nodiscard]] constexpr std::string_view clockModeName(ClockMode mode) noexcept {
    switch (mode) {
    case ClockMode::NTP: return "NTP";
    case ClockMode::PTP: return "PTP";
    case ClockMode::TAI: return "TAI";
    case ClockMode::HwPps: return "HwPps";
    case ClockMode::Auto: return "Auto";
    }
    return "Auto";
}

/*| role: the kernel's clock discipline as ntp_adjtime() reports it.
    frame: every duration is in nanoseconds and the frequency offset in parts per billion. The
        kernel reports the offset and the jitter in nanoseconds where STA_NANO is set and in
        microseconds otherwise, the error bounds always in microseconds, and the frequency in
        parts per million with a 16-bit fraction. taiUtcOffsetS is zero where the platform's
        timex has no TAI field. leapStatus is ntp_adjtime()'s return value, the clock state.
*/
struct KernelDiscipline {
    bool         synchronized  = false;
    std::int64_t offsetNs      = 0;
    std::int64_t estErrorNs    = 0;
    std::int64_t maxErrorNs    = 0;
    std::int64_t freqPpb       = 0;
    std::int64_t jitterNs      = 0;
    std::int32_t taiUtcOffsetS = 0;
    int          leapStatus    = TIME_ERROR;
};

/*| contract: the discipline a timex record and a clock state describe, in the units above.
        synchronized holds where STA_UNSYNC is clear and the state is no error.
    verified-by: pps.discipline-units
*/
[[nodiscard]] inline KernelDiscipline disciplineFrom(const struct timex& tx, int state) noexcept {
    KernelDiscipline d;
    d.leapStatus = state;
    d.synchronized = (tx.status & STA_UNSYNC) == 0 && state != TIME_ERROR;
#if defined(STA_NANO)
    const std::int64_t fineScale = (tx.status & STA_NANO) != 0 ? 1 : 1000;
#else
    const std::int64_t fineScale = 1000;
#endif
    d.offsetNs   = static_cast<std::int64_t>(tx.offset) * fineScale;
    d.jitterNs   = static_cast<std::int64_t>(tx.jitter) * fineScale;
    d.estErrorNs = static_cast<std::int64_t>(tx.esterror) * 1000;
    d.maxErrorNs = static_cast<std::int64_t>(tx.maxerror) * 1000;
    d.freqPpb    = static_cast<std::int64_t>(tx.freq) * 1000 / 65536;
#if defined(TIMESOURCE_HAVE_TIMEX_TAI)
    d.taiUtcOffsetS = static_cast<std::int32_t>(tx.tai);
#endif
    return d;
}

/*| contract: the kernel's discipline now, read with ntp_adjtime() and changing nothing.
*/
[[nodiscard]] inline KernelDiscipline queryKernelDiscipline() noexcept {
    struct timex tx{};
    tx.modes        = 0;
    const int state = ::ntp_adjtime(&tx);
    return disciplineFrom(tx, state);
}

[[nodiscard]] constexpr std::string_view leapStatusName(int state) noexcept {
    switch (state) {
    case TIME_OK: return "OK";
    case TIME_INS: return "insert_leap";
    case TIME_DEL: return "delete_leap";
    case TIME_OOP: return "leap_in_progress";
    case TIME_WAIT: return "leap_occurred";
    default: return "unsynchronised";
    }
}

[[nodiscard]] constexpr std::uint64_t toNs(const timespec& ts) noexcept { return static_cast<std::uint64_t>(ts.tv_sec) * 1'000'000'000ULL + static_cast<std::uint64_t>(ts.tv_nsec); }

inline constexpr std::uint64_t kSecondNs = 1'000'000'000ULL;

// The nanoseconds from a clock reading to its next whole second.
[[nodiscard]] constexpr std::uint64_t nsToNextSecond(std::uint64_t nowNs) noexcept { return 1'000'000'000ULL - nowNs % 1'000'000'000ULL; }

/*| role: one second a clock marked.
    frame: nominalNs is the whole second on the clock's own scale; wakeupOffsetNs is the reading
        taken at the mark less nominalNs, the pulse's own timestamp for HwPps. missed holds
        where a HwPps wait ended without a pulse; nominalNs is then the host's clock.
*/
struct Pulse {
    std::uint64_t nominalNs      = 0;
    std::int64_t  wakeupOffsetNs = 0;
    bool          missed         = false;
};

/*| contract: the second a PPS assert timestamp marks, the whole second nearest to it, and the
        timestamp's offset from that second, negative for an edge that came early.
    verified-by: pps.a-pulse-marks-its-nearest-second
*/
[[nodiscard]] constexpr Pulse pulseAt(std::int64_t seconds, std::int64_t nanoseconds) noexcept {
    const std::int64_t nearest = seconds + (nanoseconds >= 500'000'000 ? 1 : 0);
    return Pulse{.nominalNs = static_cast<std::uint64_t>(nearest) * 1'000'000'000ULL, .wakeupOffsetNs = seconds * 1'000'000'000LL + nanoseconds - nearest * 1'000'000'000LL};
}

/*| role: the time scale a PTP clock runs on.
    contract: TAI, the scale PTP defines, runs ahead of UTC by the TAI-UTC offset. UTC is the
        scale of the system clock.
*/
enum class TimeScale : std::uint8_t { TAI, UTC };

/*| contract: the TAI-UTC offset in seconds: the kernel's where it reports a positive one, and
        fallbackS where it reports 0.
    verified-by: pps.a-ptp-clock-on-tai-takes-the-kernel-offset-or-the-fallback
*/
[[nodiscard]] constexpr std::int32_t taiUtcOffsetS(const KernelDiscipline& d, std::int32_t fallbackS) noexcept { return d.taiUtcOffsetS > 0 ? d.taiUtcOffsetS : fallbackS; }

// The nanoseconds a clock on scale runs ahead of UTC, taiUtcS seconds on TAI.
[[nodiscard]] constexpr std::int64_t aheadOfUtcNs(TimeScale scale, std::int32_t taiUtcS) noexcept { return scale == TimeScale::TAI ? static_cast<std::int64_t>(taiUtcS) * 1'000'000'000LL : 0; }

/*| contract: a reading of a clock on scale, moved to UTC.
    verified-by: pps.a-ptp-clock-on-tai-takes-the-kernel-offset-or-the-fallback
*/
[[nodiscard]] constexpr std::uint64_t scaleToUtcNs(std::uint64_t readingNs, TimeScale scale, std::int32_t taiUtcS) noexcept { return static_cast<std::uint64_t>(static_cast<std::int64_t>(readingNs) - aheadOfUtcNs(scale, taiUtcS)); }

/*| contract: whether a second of a PTP clock on scale is locked. The kernel reports the system
        clock synchronized, and the PTP clock moved to UTC differs from the system clock by less
        than limitNs.
    frame: ptpLessSystemNs is the PTP clock's reading less the system clock's, both read at one
        moment. taiUtcS is the TAI-UTC offset taiUtcOffsetS() gives.
    verified-by: pps.ptp-locks-to-a-disciplined-system-clock
    verified-by: pps.a-ptp-clock-on-tai-takes-the-kernel-offset-or-the-fallback
*/
[[nodiscard]] constexpr bool ptpLocked(const KernelDiscipline& d, std::int64_t ptpLessSystemNs, TimeScale scale, std::int32_t taiUtcS, std::uint64_t limitNs) noexcept {
    const std::int64_t  offset    = ptpLessSystemNs - aheadOfUtcNs(scale, taiUtcS);
    const std::uint64_t magnitude = offset < 0 ? 0ULL - static_cast<std::uint64_t>(offset) : static_cast<std::uint64_t>(offset);
    return d.synchronized && magnitude < limitNs;
}

/*| role: the kernel calls a ClockSource makes, gathered so a case can answer them.
    contract: readNs reads a clock in nanoseconds, steadyNs reads the monotonic clock, and
        sleepNs sleeps for a duration on the monotonic clock. discipline reads the kernel's
        discipline. fetchPps waits at most timeoutNs for the assert events of a PPS source and
        answers 0, or the errno of the failure. Each default calls the kernel.
*/
struct ClockCalls {
    std::function<std::uint64_t(clockid_t)> readNs = [](clockid_t clock) {
        timespec now{};
        ::clock_gettime(clock, &now);
        return toNs(now);
    };
    std::function<std::uint64_t()>      steadyNs   = [] { return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()); };
    std::function<void(std::uint64_t)>  sleepNs    = [](std::uint64_t ns) { std::this_thread::sleep_for(std::chrono::nanoseconds{ns}); };
    std::function<KernelDiscipline()>   discipline = [] { return queryKernelDiscipline(); };
#if defined(TIMESOURCE_HAVE_TIMEPPS)
    std::function<int(pps_handle_t, pps_info_t&, std::uint64_t)> fetchPps = [](pps_handle_t pps, pps_info_t& info, std::uint64_t timeoutNs) {
        timespec wait{.tv_sec = static_cast<time_t>(timeoutNs / kSecondNs), .tv_nsec = static_cast<long>(timeoutNs % kSecondNs)};
        return ::time_pps_fetch(pps, PPS_TSFMT_TSPEC, &info, &wait) == 0 ? 0 : errno;
    };
#endif
};

/*| role: an open clock that marks each second.
    contract: open() resolves the mode asked for and opens what it needs, or answers why the
        mode is unavailable here. next() waits for the next whole second of the clock and
        answers it, or nothing where stop() turned true during the wait.
    contract: a HwPps wait answers a missed pulse at the host's time where 1.1 s pass without a
        pulse. A fetch error other than a timeout or an interrupt ends its 100 ms slice with a
        sleep. takeFault() answers the first error of each run of errors, once.
    frame: every wait sleeps on the monotonic clock in slices of at most 100 ms, tests stop()
        between slices and reads the marked clock after each. The mark is the next whole second
        of the clock, and never earlier than one second after the last second answered. Where a
        reading has passed the mark, next() answers the whole second of that reading. In NTP,
        TAI and PTP modes each second answered lies after the one before. A clock stepped back
        waits for the second after the last one answered.
    trap: open() in PTP or HwPps mode opens that device. NTP, TAI and Auto open nothing.
    verified-by: pps.ntp-emits-one-tag-per-second
    verified-by: pps.a-clock-step-keeps-the-seconds-in-order
    verified-by: pps.a-fetch-error-is-paced-and-reported-once
    verified-by: pps.ptp-without-a-device-is-unavailable
    verified-by: pps.hardware-pps-without-a-device-is-unavailable
*/
class ClockSource {
public:
    ClockSource() = default;
    ClockSource(const ClockSource&)            = delete;
    ClockSource& operator=(const ClockSource&) = delete;
    ClockSource(ClockSource&& other) noexcept { *this = std::move(other); }
    ClockSource& operator=(ClockSource&& other) noexcept {
        if (this != &other) {
            close();
            _mode         = other._mode;
            _clock        = other._clock;
            _fd           = std::exchange(other._fd, -1);
            _path         = std::move(other._path);
            _fault        = std::move(other._fault);
            _failing      = other._failing;
            _lastSecondNs = other._lastSecondNs;
            _calls        = std::move(other._calls);
#if defined(TIMESOURCE_HAVE_TIMEPPS)
            _pps          = other._pps;
            _hasPps       = std::exchange(other._hasPps, false);
            _lastSequence = other._lastSequence;
#endif
        }
        return *this;
    }
    ~ClockSource() { close(); }

    [[nodiscard]] static std::expected<ClockSource, std::string> open(ClockMode requested, std::uint8_t ptpIndex, std::uint8_t ppsIndex) {
        ClockSource source;
        switch (requested) {
        case ClockMode::NTP:
        case ClockMode::Auto: source.useSystemClock(); return source;
        case ClockMode::TAI:
#if defined(CLOCK_TAI)
            source._mode  = ClockMode::TAI;
            source._clock = CLOCK_TAI;
            return source;
#else
            return std::unexpected(std::string("clock mode TAI is unavailable: this platform has no CLOCK_TAI"));
#endif
        case ClockMode::PTP:
            if (auto opened = source.openPtp(ptpIndex); !opened) {
                return std::unexpected(std::format("clock mode PTP is unavailable: {}", opened.error()));
            }
            return source;
        case ClockMode::HwPps:
            if (auto opened = source.openPps(ppsIndex); !opened) {
                return std::unexpected(std::format("clock mode HwPps is unavailable: {}", opened.error()));
            }
            return source;
        }
        return std::unexpected(std::string("unknown clock mode"));
    }

    /*| contract: a source in mode that marks clock and makes every kernel call through calls. It
            opens no device; name stands for the device in its messages.
    */
    [[nodiscard]] static ClockSource through(ClockMode mode, clockid_t clock, ClockCalls calls, std::string name = {}) {
        ClockSource source;
        source._mode  = mode;
        source._clock = clock;
        source._calls = std::move(calls);
        source._path  = std::move(name);
        return source;
    }

    [[nodiscard]] ClockMode mode() const noexcept { return _mode; }

    template <typename Stop>
    [[nodiscard]] std::optional<Pulse> next(Stop&& stop) {
        if (_mode == ClockMode::HwPps) {
            return nextPps(stop);
        }
        const std::uint64_t start = read();
        const std::uint64_t mark  = std::max(start + nsToNextSecond(start), _lastSecondNs + kSecondNs);
        for (;;) {
            if (stop()) {
                return std::nullopt;
            }
            const std::uint64_t now = read();
            if (now >= mark) {
                _lastSecondNs = now - now % kSecondNs;
                return Pulse{.nominalNs = _lastSecondNs, .wakeupOffsetNs = static_cast<std::int64_t>(now - _lastSecondNs)};
            }
            _calls.sleepNs(std::min(mark - now, kSliceNs));
        }
    }

    /*| contract: the PTP clock's reading less the system clock's. The system clock is read
            before and after the PTP clock, and the two readings are averaged. Nothing outside
            PTP mode.
        verified-by: pps.ptp-offset-reads-both-clocks
    */
    [[nodiscard]] std::optional<std::int64_t> ptpLessSystemNs() const {
        if (_mode != ClockMode::PTP) {
            return std::nullopt;
        }
        const auto before = static_cast<std::int64_t>(_calls.readNs(CLOCK_REALTIME));
        const auto ptp    = static_cast<std::int64_t>(_calls.readNs(_clock));
        const auto after  = static_cast<std::int64_t>(_calls.readNs(CLOCK_REALTIME));
        return ptp - (before + (after - before) / 2);
    }

    // The kernel's discipline of the system clock now.
    [[nodiscard]] KernelDiscipline discipline() const { return _calls.discipline(); }

    // The first fetch error of a run of errors, answered once.
    [[nodiscard]] std::optional<std::string> takeFault() {
        if (_fault.empty()) {
            return std::nullopt;
        }
        return std::exchange(_fault, std::string{});
    }

private:
    static constexpr std::uint64_t kSliceNs = 100'000'000ULL;

    void useSystemClock() noexcept {
        _mode  = ClockMode::NTP;
        _clock = CLOCK_REALTIME;
    }

    [[nodiscard]] std::uint64_t read() const { return _calls.readNs(_clock); }

    [[nodiscard]] std::expected<void, std::string> openPtp([[maybe_unused]] std::uint8_t index) {
#if defined(__linux__)
        const std::string path = std::format("/dev/ptp{}", index);
        const int         fd   = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            return std::unexpected(std::format("cannot open {}: {}", path, std::strerror(errno)));
        }
        _fd    = fd;
        _mode  = ClockMode::PTP;
        _clock = static_cast<clockid_t>((~fd << 3) | 3); // the kernel's FD_TO_CLOCKID
        _path  = path;
        timespec probe{};
        if (::clock_gettime(_clock, &probe) != 0) {
            const std::string reason = std::format("{} is no clock: {}", path, std::strerror(errno));
            close();
            return std::unexpected(reason);
        }
        return {};
#else
        return std::unexpected(std::string("this platform has no PTP clock interface"));
#endif
    }

    [[nodiscard]] std::expected<void, std::string> openPps([[maybe_unused]] std::uint8_t index) {
#if defined(TIMESOURCE_HAVE_TIMEPPS)
        const std::string path = std::format("/dev/pps{}", index);
        int               fd   = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
        if (fd < 0 && errno == EACCES) {
            fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        }
        if (fd < 0) {
            return std::unexpected(std::format("cannot open {}: {}", path, std::strerror(errno)));
        }
        _fd = fd;
        if (::time_pps_create(fd, &_pps) < 0) {
            const std::string reason = std::format("{} is no PPS source: {}", path, std::strerror(errno));
            close();
            return std::unexpected(reason);
        }
        _hasPps = true;
        int capabilities = 0;
        pps_params_t params{};
        if (::time_pps_getcap(_pps, &capabilities) < 0 || (capabilities & PPS_CAPTUREASSERT) == 0 || ::time_pps_getparams(_pps, &params) < 0) {
            close();
            return std::unexpected(std::format("{} cannot capture the assert edge", path));
        }
        if ((params.mode & PPS_CAPTUREASSERT) == 0) {
            params.mode |= PPS_CAPTUREASSERT;
            if (::time_pps_setparams(_pps, &params) < 0) {
                const std::string reason = std::format("cannot enable assert capture on {}: {}", path, std::strerror(errno));
                close();
                return std::unexpected(reason);
            }
        }
        _mode  = ClockMode::HwPps;
        _clock = CLOCK_REALTIME;
        _path  = path;
        return {};
#else
        return std::unexpected(std::string("the build found no RFC 2783 sys/timepps.h"));
#endif
    }

    template <typename Stop>
    [[nodiscard]] std::optional<Pulse> nextPps([[maybe_unused]] Stop& stop) {
#if defined(TIMESOURCE_HAVE_TIMEPPS)
        const std::uint64_t deadline = _calls.steadyNs() + kSecondNs + kSliceNs;
        for (;;) {
            if (stop()) {
                return std::nullopt;
            }
            const std::uint64_t sliceEnd = _calls.steadyNs() + kSliceNs;
            pps_info_t          info{};
            if (const int error = _calls.fetchPps(_pps, info, kSliceNs); error == 0) {
                _failing = false;
                if (info.assert_sequence != _lastSequence) {
                    _lastSequence = info.assert_sequence;
                    return pulseAt(static_cast<std::int64_t>(info.assert_timestamp.tv_sec), static_cast<std::int64_t>(info.assert_timestamp.tv_nsec));
                }
            } else if (error != ETIMEDOUT && error != EINTR) {
                if (!_failing) {
                    _failing = true;
                    _fault   = std::format("fetching a pulse from {} failed: {}", _path, std::strerror(error));
                }
                if (const std::uint64_t now = _calls.steadyNs(); now < sliceEnd) {
                    _calls.sleepNs(sliceEnd - now);
                }
            }
            if (_calls.steadyNs() >= deadline) {
                return Pulse{.nominalNs = read(), .wakeupOffsetNs = 0, .missed = true};
            }
        }
#else
        return std::nullopt;
#endif
    }

    void close() noexcept {
#if defined(TIMESOURCE_HAVE_TIMEPPS)
        if (_hasPps) {
            ::time_pps_destroy(_pps);
            _hasPps = false;
        }
#endif
        if (_fd >= 0) {
            ::close(_fd);
            _fd = -1;
        }
    }

    ClockMode     _mode  = ClockMode::NTP;
    clockid_t     _clock = CLOCK_REALTIME;
    int           _fd    = -1;
    std::string   _path;
    std::string   _fault;
    bool          _failing      = false;
    std::uint64_t _lastSecondNs = 0;
    ClockCalls    _calls;
#if defined(TIMESOURCE_HAVE_TIMEPPS)
    pps_handle_t  _pps{};
    bool          _hasPps       = false;
    unsigned long _lastSequence = 0;
#endif
};

} // namespace timesource
