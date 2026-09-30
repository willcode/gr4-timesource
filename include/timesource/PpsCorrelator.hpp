/* -*- c++ -*- */
/*
 * Copyright 2026 Jeff Long
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <gnuradio-4.0/Block.hpp>
#include <gnuradio-4.0/DataSet.hpp>
#include <gnuradio-4.0/Port.hpp>
#include <gnuradio-4.0/Tag.hpp>
#include <gnuradio-4.0/annotated.hpp>

#include <gnuradio-4.0/algorithm/MeasurementRecord.hpp>
#include <gnuradio-4.0/algorithm/MeasurementSlot.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <concepts>
#include <cstdint>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace timesource {

/*| role: where PpsCorrelator finds an edge: a pulse in the samples, or a trigger tag.
*/
enum class PpsEdgeSource : std::uint8_t { Threshold = 0, TriggerTag };

namespace detail {

[[nodiscard]] inline PpsEdgeSource parseEdgeSource(std::string_view name) {
    if (name == "threshold") {
        return PpsEdgeSource::Threshold;
    }
    if (name == "trigger_tag") {
        return PpsEdgeSource::TriggerTag;
    }
    throw gr::exception(std::format("PpsCorrelator: 'edge_source' must be 'threshold' or 'trigger_tag', got '{}'", name));
}

} // namespace detail

/*| role: measures the error of a stream's sample clock against a reference that marks each
        second.
    contract: the block finds one edge for each reference pulse. N_k is the distance in samples
        between two consecutive edges. The nominal interval is sample_rate * pps_interval. The
        rate error of one interval is

            rate_error_ppm = (N_k / nominal_interval - 1) * 1e6

        A positive error means that the stream holds more samples per second than sample_rate
        states.
    contract: with edge_source threshold, the block reads a pulse in the samples. It takes the
        magnitude m of each sample. An edge is the first sample k at or above threshold while the
        detector is armed. The edge position is the linear interpolation between the two samples
        around the crossing:

            position = (k - 1) + (threshold - m[k-1]) / (m[k] - m[k-1])

        The detector arms while the magnitude is below threshold. The arming gives
        m[k-1] < threshold <= m[k], and the denominator is positive. A stream whose first sample
        is at or above threshold starts disarmed and gives no edge there. The interpolation is
        exact for a linear edge.
    contract: with edge_source trigger_tag, the block reads the tags that timesource::PpsSource
        writes. PpsSource puts trigger_name, trigger_time and trigger_offset on the first sample
        of each second. A tag with a uint64 trigger_time is an edge at its sample index.
        trigger_offset is a delay in seconds. The block adds trigger_offset * sample_rate samples
        to the edge position. A non-empty trigger_filter admits a tag only where its trigger_name
        starts with trigger_filter.
    contract: an interval whose error exceeds tolerance_ppm is left out of the average. A long
        interval of that kind counts round(N_k / nominal_interval) - 1 missed edges, and at least
        one. A short interval of that kind counts one extra edge. rateErrorPpm() is the mean
        error of the last n_intervals accepted intervals.
    contract: the block writes one DataSet record for each closed interval to the optional
        records port. A record's sample_start is the whole sample on which its interval opened.
        An interval still open when the stream ends gives no record. The block publishes the
        errors and the counts together after each edge. Each reader returns a value from one
        update and may run on any thread.
    trap: sample_rate is the rate of the stream the block reads. A PpsSource stream in ppsOnly
        mode has one sample per second, and its sample_rate is 1. A device rate given for such a
        stream makes every interval about -1e6 ppm. The block refuses each one as an extra edge,
        and rateErrorPpm() stays 0.
    why: sample_rate is a double. A float rate above 16.8 MS/s carries a rounding error of up to
        0.06 ppm. A float sample_rate tag still sets the double setting.
    verified-by: correlator.measures-20-ppm-and-zero-after-a-correction
    verified-by: correlator.a-deleted-pulse-is-one-missed-edge
    verified-by: correlator.pps-source-tags-are-an-edge-stream
    verified-by: correlator.records-state-their-interval
*/
template <typename T>
requires(std::same_as<T, float> || std::same_as<T, std::complex<float>> || std::same_as<T, std::uint8_t>)
struct PpsCorrelator : gr::Block<PpsCorrelator<T>> {
    using Description = gr::Doc<R""(
@brief Measures the sampling clock's error in ppm from the interval between PPS edges.

`rate_error_ppm = (N_k / (sample_rate * pps_interval) - 1) * 1e6` per interval, from a threshold crossing with
two-point linear sub-sample interpolation, or from the trigger tags of `timesource::PpsSource`. An interval outside
`tolerance_ppm` counts as a missed or extra edge and stays out of the average. One record per interval.
)"">;

    gr::PortIn<T> in;
    // One record per closed interval. An unconnected port drops the records.
    gr::PortOut<gr::DataSet<float>, gr::Async, gr::Optional> records;

    gr::Annotated<double, "sample_rate", gr::Visible, gr::Unit<"Hz">, gr::Doc<"nominal rate of the stream being read">>          sample_rate    = 1.0;
    gr::Annotated<double, "pps_interval", gr::Visible, gr::Unit<"s">, gr::Doc<"nominal seconds between reference edges">>        pps_interval   = 1.0;
    gr::Annotated<std::string, "edge_source", gr::Visible, gr::Doc<"edge source, 'threshold' or 'trigger_tag'">>                 edge_source    = std::string("threshold");
    gr::Annotated<double, "threshold", gr::Visible, gr::Doc<"magnitude a rising edge crosses in 'threshold' mode">>              threshold      = 0.5;
    gr::Annotated<std::string, "trigger_filter", gr::Doc<"prefix of the trigger names counted; empty counts every trigger_time">> trigger_filter = std::string("");
    gr::Annotated<double, "tolerance_ppm", gr::Visible, gr::Doc<"largest interval error accepted as a measurement, in ppm">>      tolerance_ppm  = 10'000.0;
    gr::Annotated<gr::Size_t, "n_intervals", gr::Visible, gr::Doc<"accepted intervals in the reported mean">>                    n_intervals    = 16U;

    GR_MAKE_REFLECTABLE(PpsCorrelator, in, records, sample_rate, pps_interval, edge_source, threshold, trigger_filter, tolerance_ppm, n_intervals);

    // The published values: the mean error, the last interval's error, and the four counts.
    static constexpr std::size_t kSmoothedAt = 0UZ;
    static constexpr std::size_t kLastAt     = 1UZ;
    static constexpr std::size_t kEdgesAt    = 2UZ;
    static constexpr std::size_t kMissedAt   = 3UZ;
    static constexpr std::size_t kExtraAt    = 4UZ;
    static constexpr std::size_t kAcceptedAt = 5UZ;
    static constexpr std::size_t kSlotValues = 6UZ;

    gr::measurement::MeasurementSlot<kSlotValues> _slot{};

    PpsEdgeSource                   _source{PpsEdgeSource::Threshold};
    double                          _nominalInterval{1.};
    std::array<double, kSlotValues> _values{};
    std::uint64_t                   _intervals{0ULL};

    // The ring holds n_intervals values from construction on. A batch that changes no setting
    // never calls settingsChanged().
    std::vector<double> _window = std::vector<double>(static_cast<std::size_t>(n_intervals.value), 0.);

    std::size_t _windowAt{0UZ};   // where the next accepted error lands
    std::size_t _windowFill{0UZ}; // how much of the ring is written

    double        _previousEdge{0.}; // position of the edge that opened the current interval
    bool          _haveEdge{false};
    double        _lastMagnitude{0.};
    bool          _havePrevious{false}; // a sample before this call's first was read
    bool          _armed{false};
    std::uint64_t _tagCursor{0ULL}; // absolute index past the last tag read

    std::vector<gr::DataSet<float>> _pending{}; // records made and not yet published

    void settingsChanged(const gr::property_map& /*oldSettings*/, const gr::property_map& /*newSettings*/) {
        if (!std::isfinite(sample_rate) || sample_rate <= 0.0) {
            throw gr::exception(std::format("PpsCorrelator: 'sample_rate' must be positive and finite, got {}", sample_rate.value));
        }
        if (!std::isfinite(pps_interval) || pps_interval <= 0.0) {
            throw gr::exception(std::format("PpsCorrelator: 'pps_interval' must be positive and finite, got {}", pps_interval.value));
        }
        if (!std::isfinite(threshold) || threshold <= 0.0) {
            throw gr::exception(std::format("PpsCorrelator: 'threshold' must be positive and finite, got {}", threshold.value));
        }
        if (!std::isfinite(tolerance_ppm) || tolerance_ppm <= 0.0) {
            throw gr::exception(std::format("PpsCorrelator: 'tolerance_ppm' must be positive and finite, got {}", tolerance_ppm.value));
        }
        if (n_intervals < 1U) {
            throw gr::exception("PpsCorrelator: 'n_intervals' must be at least one");
        }
        _source          = detail::parseEdgeSource(edge_source);
        _nominalInterval = sample_rate * pps_interval;
        reset();
    }

    void start() { reset(); }

    // Clears every edge and every count. The owning thread calls it between stop() and start().
    void reset() {
        _values.fill(0.);
        _intervals  = 0ULL;
        _windowAt   = 0UZ;
        _windowFill = 0UZ;
        _window.assign(static_cast<std::size_t>(n_intervals.value), 0.);
        _previousEdge  = 0.;
        _haveEdge      = false;
        _lastMagnitude = 0.;
        _havePrevious  = false;
        _armed         = false;
        _tagCursor     = 0ULL;
        _pending.clear();
        publish();
    }

    // The clock error in parts per million, the mean of the last n_intervals accepted intervals. Any thread.
    [[nodiscard]] double rateErrorPpm() const noexcept { return _slot.read().first[kSmoothedAt]; }

    // The error of the last accepted interval alone. Any thread.
    [[nodiscard]] double lastRateErrorPpm() const noexcept { return _slot.read().first[kLastAt]; }

    // Edges found. Any thread.
    [[nodiscard]] std::uint64_t nEdges() const noexcept { return static_cast<std::uint64_t>(_slot.read().first[kEdgesAt]); }

    // Reference edges the long intervals count as missed. Any thread.
    [[nodiscard]] std::uint64_t nMissedEdges() const noexcept { return static_cast<std::uint64_t>(_slot.read().first[kMissedAt]); }

    // Intervals too short for one period, one extra edge each. Any thread.
    [[nodiscard]] std::uint64_t nExtraEdges() const noexcept { return static_cast<std::uint64_t>(_slot.read().first[kExtraAt]); }

    // Intervals closed. nAcceptedIntervals() of them were inside the tolerance. Any thread.
    [[nodiscard]] std::uint64_t nIntervals() const noexcept { return _slot.read().second; }

    [[nodiscard]] std::uint64_t nAcceptedIntervals() const noexcept { return static_cast<std::uint64_t>(_slot.read().first[kAcceptedAt]); }

    [[nodiscard]] gr::work::Status processBulk(gr::InputSpanLike auto& inSpan, gr::OutputSpanLike auto& outSpan) {
        const std::span<const T> input = std::span<const T>(inSpan);
        const std::uint64_t      base  = static_cast<std::uint64_t>(inSpan.streamIndex);

        if (_source == PpsEdgeSource::Threshold) {
            scanThreshold(input, base);
        } else {
            scanTags(inSpan, base + static_cast<std::uint64_t>(input.size()));
        }
        std::size_t made = 0UZ;
        if (outSpan.isConnected) {
            const std::size_t take = std::min(_pending.size(), outSpan.size());
            for (; made < take; ++made) {
                outSpan[made] = std::move(_pending[made]);
            }
            _pending.erase(_pending.begin(), _pending.begin() + static_cast<std::ptrdiff_t>(made));
        } else {
            _pending.clear(); // an unconnected port keeps no backlog
        }
        outSpan.publish(made);
        std::ignore = inSpan.consume(input.size());
        return gr::work::Status::OK;
    }

private:
    // The magnitude: a bipolar pulse gives one edge, and a complex pulse needs no phase.
    [[nodiscard]] static double magnitude(T sample) noexcept {
        if constexpr (std::same_as<T, std::complex<float>>) {
            return std::sqrt(static_cast<double>(sample.real()) * static_cast<double>(sample.real()) + static_cast<double>(sample.imag()) * static_cast<double>(sample.imag()));
        } else if constexpr (std::same_as<T, float>) {
            return std::abs(static_cast<double>(sample));
        } else {
            return static_cast<double>(sample);
        }
    }

    void scanThreshold(std::span<const T> input, std::uint64_t base) {
        const double level    = threshold;
        double       previous = _lastMagnitude;
        bool         armed    = _armed;
        bool         have     = _havePrevious;

        for (std::size_t k = 0UZ; k < input.size(); ++k) {
            const double m = magnitude(input[k]);
            if (!have) {
                // the stream's first sample sets the detector's starting state
                armed = m < level;
                have  = true;
            } else if (armed && m >= level) {
                // previous < level <= m by the arming: the denominator is positive
                const double fraction = (level - previous) / (m - previous);
                closeEdge(static_cast<double>(base + static_cast<std::uint64_t>(k)) - 1. + fraction);
                armed = false;
            } else if (!armed && m < level) {
                armed = true;
            }
            previous = m;
        }

        _lastMagnitude = previous;
        _armed         = armed;
        _havePrevious  = have;
    }

    void scanTags(const auto& inSpan, std::uint64_t end) {
        for (const gr::Tag& tag : inSpan.rawTags) {
            const std::uint64_t at = static_cast<std::uint64_t>(tag.index);
            if (at < _tagCursor || at >= end) {
                continue; // a tag the framework did not retire returns in the next window; the cursor counts it once
            }
            const auto found = tag.map.find(gr::tag::TRIGGER_TIME.shortKey());
            if (found == tag.map.end() || found->second.template get_if<std::uint64_t>() == nullptr) {
                continue;
            }
            if (!trigger_filter.value.empty()) {
                const auto named = tag.map.find(gr::tag::TRIGGER_NAME.shortKey());
                if (named == tag.map.end()) {
                    continue;
                }
                const std::pmr::string* name = named->second.template get_if<std::pmr::string>();
                if (name == nullptr || !std::string_view(name->data(), name->size()).starts_with(std::string_view(trigger_filter.value))) {
                    continue;
                }
            }
            // trigger_offset is a delay in seconds after the tagged sample
            double offset = 0.;
            if (const auto delay = tag.map.find(gr::tag::TRIGGER_OFFSET.shortKey()); delay != tag.map.end()) {
                if (const float* seconds = delay->second.template get_if<float>(); seconds != nullptr) {
                    offset = static_cast<double>(*seconds) * sample_rate;
                }
            }
            closeEdge(static_cast<double>(at) + offset);
        }
        _tagCursor = std::max(_tagCursor, end);
    }

    // Counts one edge. An edge after the first closes an interval, which the block measures and records.
    void closeEdge(double position) {
        _values[kEdgesAt] += 1.;
        if (!_haveEdge) {
            _previousEdge = position;
            _haveEdge     = true;
            publish();
            return;
        }

        const double span     = position - _previousEdge;
        const double ppm      = (span / _nominalInterval - 1.) * 1e6;
        const bool   accepted = std::abs(ppm) <= tolerance_ppm;

        std::uint64_t missed = 0ULL;
        if (!accepted) {
            if (span > _nominalInterval) {
                // a long interval spans whole periods, and a refused one lost at least one edge
                const std::int64_t periods = std::max<std::int64_t>(2, std::llround(span / _nominalInterval));
                missed                     = static_cast<std::uint64_t>(periods) - 1ULL;
                _values[kMissedAt] += static_cast<double>(missed);
            } else {
                _values[kExtraAt] += 1.;
            }
        } else {
            _window[_windowAt] = ppm;
            _windowAt          = (_windowAt + 1UZ) % _window.size();
            _windowFill        = std::min(_windowFill + 1UZ, _window.size());
            double mean        = 0.;
            for (std::size_t i = 0UZ; i < _windowFill; ++i) { // a fixed order makes the mean a function of the input alone
                mean += _window[i];
            }
            _values[kSmoothedAt] = mean / static_cast<double>(_windowFill);
            _values[kLastAt]     = ppm;
            _values[kAcceptedAt] += 1.;
        }

        emitRecord(_intervals, position, span, ppm, accepted, missed);
        ++_intervals;
        _previousEdge = position;
        publish();
    }

    void emitRecord(std::uint64_t index, double position, double span, double ppm, bool accepted, std::uint64_t missed) {
        const double                                          whole = std::floor(position);
        const std::array<gr::measurement::ScalarChannel, 3UZ> channels{{
            {"rate_error_ppm", "Frequency error", "ppm", static_cast<float>(ppm)},
            {"edge_fraction", "Position", "1", static_cast<float>(position - whole)},
            {"accepted", "Flag", "1", accepted ? 1.f : 0.f},
        }};
        // the absolute sample index and the interval length are exact integers a float channel
        // would round, and they travel as metadata
        gr::property_map extra{
            {gr::property_map::key_type("interval_index"), gr::pmt::Value(index)},
            {gr::property_map::key_type("edge_index"), gr::pmt::Value(static_cast<std::uint64_t>(whole))},
            {gr::property_map::key_type("interval_samples"), gr::pmt::Value(span)},
            {gr::property_map::key_type("n_missed_edges"), gr::pmt::Value(missed)},
        };
        _pending.push_back(gr::measurement::makeScalarRecord(std::span<const gr::measurement::ScalarChannel>(channels), static_cast<float>(sample_rate), static_cast<std::uint64_t>(std::floor(_previousEdge)), std::move(extra)));
    }

    void publish() noexcept { _slot.publish(_values, _intervals); }
};

} // namespace timesource
