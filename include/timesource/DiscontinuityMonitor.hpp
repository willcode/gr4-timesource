/* -*- c++ -*- */
/*
 * Copyright 2026 Jeff Long
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <gnuradio-4.0/Block.hpp>
#include <gnuradio-4.0/Port.hpp>
#include <gnuradio-4.0/Tag.hpp>
#include <gnuradio-4.0/annotated.hpp>

#include <gnuradio-4.0/algorithm/MeasurementSlot.hpp>
#include <gnuradio-4.0/algorithm/timing/SampleClock.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <concepts>
#include <cstdint>
#include <format>
#include <limits>
#include <span>
#include <string_view>
#include <utility>

namespace timesource {

namespace detail {

/*| role: the denominator of a stated rate, one microhertz. A rate a device states is a whole
        number of hertz on this grid, and a clock trim of one part per million is exact on it:
        44100 Hz trimmed by 1 ppm is 44100044100/1000000.
*/
inline constexpr std::uint64_t kRateDenominator = 1'000'000ULL;

/*| role: the largest rate the grid carries inside the exact domain of gr::timing::SampleClock.
*/
inline constexpr double kMaxRateHz = static_cast<double>(gr::timing::SampleClock::kMaxRateNum / kRateDenominator);

/*| contract: the rate as the exact rational rate_num/rate_den that gr::timing::SampleClock takes,
        rounded to the nearest microhertz. The rounding error is at most half a microhertz,
        1.1e-14 of 44.1 kHz. A rate that is not positive and finite, below one microhertz or
        above kMaxRateHz throws, naming the block and the setting.
*/
[[nodiscard]] inline std::pair<std::uint64_t, std::uint64_t> rationalRate(double rateHz, std::string_view block, std::string_view setting) {
    if (!std::isfinite(rateHz) || rateHz <= 0.) {
        throw gr::exception(std::format("{}: '{}' must be a positive finite rate, got {}", block, setting, rateHz));
    }
    if (rateHz < 1. / static_cast<double>(kRateDenominator) || rateHz > kMaxRateHz) {
        throw gr::exception(std::format("{}: '{}' is {} Hz, outside the exact rational domain [{:g}, {:g}] Hz the microhertz grid and gr::timing::SampleClock allow", block, setting, rateHz, 1. / static_cast<double>(kRateDenominator), kMaxRateHz));
    }
    return {static_cast<std::uint64_t>(std::llround(rateHz * static_cast<double>(kRateDenominator))), kRateDenominator};
}

} // namespace detail

/*| role: the causes a `discontinuity` tag names. Other counts a name outside the known set.
*/
enum class DiscontinuityCause : std::uint8_t { SampleRate = 0, SignalName, SignalQuantity, SignalUnit, SignalRange, Gap, Other };

inline constexpr std::size_t kDiscontinuityCauses = 7UZ;

inline constexpr std::array<std::string_view, kDiscontinuityCauses> kDiscontinuityCauseNames{"sample_rate", "signal_name", "signal_quantity", "signal_unit", "signal_range", "gap", "other"};

[[nodiscard]] inline constexpr DiscontinuityCause discontinuityCauseFrom(std::string_view name) noexcept {
    for (std::size_t i = 0UZ; i + 1UZ < kDiscontinuityCauses; ++i) {
        if (kDiscontinuityCauseNames[i] == name) {
            return static_cast<DiscontinuityCause>(i);
        }
    }
    return DiscontinuityCause::Other;
}

/*| role: copies its input to its output and counts the gaps that the stream's tags report.
    contract: the block reads three tag keys. n_dropped_samples holds a number of lost samples,
        and the block adds that number to a running total. discontinuity holds a comma-separated
        list of cause names. The known names are sample_rate, signal_name, signal_quantity,
        signal_unit, signal_range and gap. Each name in the list adds one to the count of its
        cause. A name outside that set adds one to the count of other. Each discontinuity tag
        also adds one to the event count. sample_rate holds the stream's rate as a float.
    contract: the block publishes all its counts together after each call that changes them.
        account() returns every count from one read of the published set. The count readers may
        run on any thread.
    contract: with a positive nominal_rate, the block refuses a stated rate that differs from
        nominal_rate by more than rate_tolerance times nominal_rate. It also refuses a zero,
        negative or non-finite rate. A refused rate adds one to the mismatch count, and
        lastRefusedRate() returns it. The clock keeps its rate. A nominal_rate of zero skips the
        comparison.
    contract: the clock takes a rate from one microhertz to detail::kMaxRateHz, the domain of
        detail::rationalRate(). A nominal_rate outside it throws when the settings are applied.
        An accepted sample_rate tag outside it throws from processBulk().
    frame: the clock is a gr::timing::SampleClock. It maps a sample index to nanoseconds in exact
        integer arithmetic. anchor_index and anchor_ns give the time of one sample. The first
        rate, a positive nominal_rate or the first accepted sample_rate tag, starts the clock at
        anchor_index and anchor_ns. A later accepted rate re-anchors the clock at the tag's
        sample index. That sample reads the same time before and after the change. A tag that
        restates the current rate changes nothing.
    frame: clock(), hasClock() and lastRefusedRate() return state that the block's own thread
        writes. Only that thread may call them.
    invariant: the block declares gr::UnfilteredTagPropagation. The framework forwards every key
        of every tag to the output at the sample index the tag arrived at. The discontinuity key
        is one of the keys this forwarding keeps.
    verified-by: discontinuity.totals-dropped-samples-and-counts-every-cause
    verified-by: discontinuity.a-rate-change-re-anchors-and-both-epochs-convert-exactly
    verified-by: discontinuity.refuses-counts-and-names-a-rate-outside-the-tolerance
    verified-by: discontinuity.forwards-a-key-the-default-filter-drops
*/
template <typename T>
requires(std::same_as<T, float> || std::same_as<T, std::complex<float>> || std::same_as<T, std::uint8_t>)
struct DiscontinuityMonitor : gr::Block<DiscontinuityMonitor<T>, gr::UnfilteredTagPropagation> {
    using Description = gr::Doc<R""(
@brief Counts the gaps a stream's tags report and keeps a sample-to-time clock.

The block passes samples and tags through unchanged. It totals `n_dropped_samples`, counts each cause a `discontinuity`
tag names, and re-anchors a `SampleClock` at each accepted `sample_rate` tag. With a positive `nominal_rate` it
refuses a stated rate that differs by more than `rate_tolerance` and counts the refusal.
)"">;

    gr::PortIn<T>  in;
    gr::PortOut<T> out;

    gr::Annotated<double, "nominal_rate", gr::Visible, gr::Unit<"Hz">, gr::Doc<"expected stated rate; 0 accepts any stated rate">>           nominal_rate   = 0.0;
    gr::Annotated<double, "rate_tolerance", gr::Visible, gr::Doc<"largest accepted relative difference from nominal_rate">>                 rate_tolerance = 1e-6;
    gr::Annotated<std::uint64_t, "anchor_index", gr::Doc<"sample index of anchor_ns">>                                                      anchor_index   = 0ULL;
    gr::Annotated<std::int64_t, "anchor_ns", gr::Unit<"ns">, gr::Doc<"nanoseconds since the Unix epoch at anchor_index; 0 gives elapsed time">> anchor_ns = 0LL;

    GR_MAKE_REFLECTABLE(DiscontinuityMonitor, in, out, nominal_rate, rate_tolerance, anchor_index, anchor_ns);

    // The published account: the per-cause counts, then events, rate changes and mismatches. The
    // slot's integer field carries the dropped-sample total, which can pass 2^53 on a long run.
    static constexpr std::size_t kAccountValues = kDiscontinuityCauses + 3UZ;
    static constexpr std::size_t kEventsAt      = kDiscontinuityCauses;
    static constexpr std::size_t kRateChangesAt = kDiscontinuityCauses + 1UZ;
    static constexpr std::size_t kMismatchesAt  = kDiscontinuityCauses + 2UZ;

    gr::measurement::MeasurementSlot<kAccountValues> _slot{};

    std::array<double, kAccountValues> _account{};
    std::uint64_t                      _dropped{0ULL};
    std::uint64_t                      _tagCursor{0ULL}; // absolute index past the last tag counted
    gr::timing::SampleClock            _clock{};
    bool                               _haveRate{false};
    double                             _rateHz{0.}; // the stated rate the clock runs at
    double                             _refusedRate{std::numeric_limits<double>::quiet_NaN()};

    void settingsChanged(const gr::property_map& /*oldSettings*/, const gr::property_map& newSettings) {
        if (!std::isfinite(nominal_rate) || nominal_rate < 0.0) {
            throw gr::exception(std::format("DiscontinuityMonitor: 'nominal_rate' must be zero or a positive finite rate, got {}", nominal_rate.value));
        }
        if (!std::isfinite(rate_tolerance) || rate_tolerance < 0.0) {
            throw gr::exception(std::format("DiscontinuityMonitor: 'rate_tolerance' must be zero or a positive finite fraction, got {}", rate_tolerance.value));
        }
        if (nominal_rate > 0.0) { // an unrepresentable nominal rate throws when the settings are applied
            std::ignore = detail::rationalRate(nominal_rate, "DiscontinuityMonitor", "nominal_rate");
        }
        if (newSettings.contains("nominal_rate") || newSettings.contains("anchor_index") || newSettings.contains("anchor_ns")) {
            anchorClock(nominal_rate > 0.0 ? nominal_rate.value : 0.0);
        }
    }

    void start() { reset(); }

    // Zeroes the account and re-anchors the clock. The owning thread calls it between stop() and start().
    void reset() {
        _account.fill(0.);
        _dropped     = 0ULL;
        _tagCursor   = 0ULL;
        _refusedRate = std::numeric_limits<double>::quiet_NaN();
        anchorClock(nominal_rate > 0.0 ? nominal_rate.value : 0.0);
        publish();
    }

    // Total samples the stream reported dropped. Any thread.
    [[nodiscard]] std::uint64_t nDroppedSamples() const noexcept { return _slot.read().second; }

    // Discontinuity events, one per `discontinuity` tag whatever the number of causes it names. Any thread.
    [[nodiscard]] std::uint64_t nEvents() const noexcept { return static_cast<std::uint64_t>(_slot.read().first[kEventsAt]); }

    // Count of one cause, one per name in each event's list. Any thread.
    [[nodiscard]] std::uint64_t nEvents(DiscontinuityCause cause) const noexcept { return static_cast<std::uint64_t>(_slot.read().first[static_cast<std::size_t>(cause)]); }

    // The whole account from one read: the per-cause counts, then events, rate changes and mismatches. Any thread.
    [[nodiscard]] std::array<std::uint64_t, kAccountValues> account() const noexcept {
        const auto                                values = _slot.read().first;
        std::array<std::uint64_t, kAccountValues> counts{};
        for (std::size_t i = 0UZ; i < kAccountValues; ++i) {
            counts[i] = static_cast<std::uint64_t>(values[i]);
        }
        return counts;
    }

    // Rate changes the clock was re-anchored at. Any thread.
    [[nodiscard]] std::uint64_t nRateChanges() const noexcept { return static_cast<std::uint64_t>(_slot.read().first[kRateChangesAt]); }

    // Stated rates refused against nominal_rate and rate_tolerance. Any thread.
    [[nodiscard]] std::uint64_t nRateMismatches() const noexcept { return static_cast<std::uint64_t>(_slot.read().first[kMismatchesAt]); }

    // The last refused rate, or NaN before the first refusal. Owning thread.
    [[nodiscard]] double lastRefusedRate() const noexcept { return _refusedRate; }

    // Whether the clock has a rate, from nominal_rate or from an accepted tag. Owning thread.
    [[nodiscard]] bool hasClock() const noexcept { return _haveRate; }

    // The sample-to-time map, re-anchored at the last accepted rate change. Owning thread.
    [[nodiscard]] const gr::timing::SampleClock& clock() const noexcept { return _clock; }

    [[nodiscard]] gr::work::Status processBulk(gr::InputSpanLike auto& inSpan, gr::OutputSpanLike auto& outSpan) {
        const std::span<const T> input    = std::span<const T>(inSpan);
        const std::size_t        nSamples = std::min(input.size(), outSpan.size());
        const std::uint64_t      base     = static_cast<std::uint64_t>(inSpan.streamIndex);
        const std::uint64_t      end      = base + static_cast<std::uint64_t>(nSamples);

        bool changed = false;
        for (const gr::Tag& tag : inSpan.rawTags) {
            // a tag the framework did not retire with its chunk returns in the next window; the
            // cursor counts each tag once
            const std::uint64_t at = static_cast<std::uint64_t>(tag.index);
            if (at < _tagCursor || at >= end) {
                continue;
            }
            changed = account(tag.map, at) || changed;
        }
        _tagCursor = std::max(_tagCursor, end);

        std::ranges::copy(input.first(nSamples), outSpan.begin());

        if (changed) {
            publish();
        }
        std::ignore = inSpan.consume(nSamples);
        outSpan.publish(nSamples);
        return gr::work::Status::OK;
    }

private:
    void anchorClock(double rateHz) {
        _haveRate = rateHz > 0.0;
        _rateHz   = _haveRate ? rateHz : 0.0;
        if (_haveRate) {
            const auto [num, den] = detail::rationalRate(rateHz, "DiscontinuityMonitor", "sample_rate");
            _clock                = gr::timing::SampleClock(anchor_index.value, anchor_ns.value, num, den);
        } else {
            _clock = gr::timing::SampleClock{};
        }
    }

    // Takes one tag's keys into the account. Returns true when a published value changed.
    [[nodiscard]] bool account(const gr::property_map& map, std::uint64_t at) {
        // the map's lookup is transparent, so a lookup by the key's characters copies nothing
        bool changed = false;
        if (const auto it = map.find(gr::tag::N_DROPPED_SAMPLES.shortKey()); it != map.end()) {
            if (const gr::Size_t* dropped = it->second.template get_if<gr::Size_t>(); dropped != nullptr) {
                _dropped += static_cast<std::uint64_t>(*dropped);
                changed = true;
            }
        }
        if (const auto it = map.find(std::string_view("discontinuity")); it != map.end()) {
            if (const std::pmr::string* causes = it->second.template get_if<std::pmr::string>(); causes != nullptr) {
                countCauses(std::string_view(causes->data(), causes->size()));
                _account[kEventsAt] += 1.;
                changed = true;
            }
        }
        if (const auto it = map.find(gr::tag::SAMPLE_RATE.shortKey()); it != map.end()) {
            if (const float* rate = it->second.template get_if<float>(); rate != nullptr) {
                changed = adoptRate(static_cast<double>(*rate), at) || changed;
            }
        }
        return changed;
    }

    void countCauses(std::string_view causes) noexcept {
        while (!causes.empty()) {
            const std::size_t      comma = causes.find(',');
            const std::string_view name  = causes.substr(0UZ, comma);
            if (!name.empty()) {
                _account[static_cast<std::size_t>(discontinuityCauseFrom(name))] += 1.;
            }
            if (comma == std::string_view::npos) {
                return;
            }
            causes.remove_prefix(comma + 1UZ);
        }
    }

    // Adopts a stated rate as a new epoch, or refuses it against nominal_rate and records the refusal.
    [[nodiscard]] bool adoptRate(double rateHz, std::uint64_t at) {
        if (!std::isfinite(rateHz) || rateHz <= 0.0 || (nominal_rate > 0.0 && std::abs(rateHz - nominal_rate) > rate_tolerance * nominal_rate)) {
            _refusedRate = rateHz;
            _account[kMismatchesAt] += 1.;
            return true;
        }
        if (_haveRate && rateHz == _rateHz) {
            return false; // a restated current rate changes nothing
        }
        const auto [num, den] = detail::rationalRate(rateHz, "DiscontinuityMonitor", "sample_rate");
        // the first rate builds the clock at the configured anchor; a later one re-anchors at the tag's sample
        _clock    = _haveRate ? _clock.withRate(num, den, at) : gr::timing::SampleClock(anchor_index.value, anchor_ns.value, num, den);
        _rateHz   = rateHz;
        _haveRate = true;
        _account[kRateChangesAt] += 1.;
        return true;
    }

    void publish() noexcept { _slot.publish(_account, _dropped); }
};

} // namespace timesource
