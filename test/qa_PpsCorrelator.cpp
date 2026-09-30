/* -*- c++ -*- */
/*
 * Copyright 2026 Jeff Long
 * SPDX-License-Identifier: MIT
 */
#include <timesource/PpsCorrelator.hpp>
#include <timesource/PpsSource.hpp>

#include "support/Cases.hpp"
#include "support/Graph.hpp"
#include "support/Spans.hpp"

#include <gnuradio-4.0/algorithm/filter/FilterDesign.hpp>
#include <gnuradio-4.0/filter/ArbitraryRateResampler.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using gr::blocks::filter::ArbitraryRateResampler;
using timesource::PpsCorrelator;
namespace test = timesource::test;

/*| contract: stages settings on a block built in place with empty braces, as the block's
        constructor stages them, then applies them and starts the block. The block holds atomics
        in its published slot and can be neither copied nor moved.
*/
template <typename TBlock>
void init(TBlock& block, const gr::property_map& initial) {
    block.settings().setInitBlockParameters(initial);
    block.settings().init();
    std::ignore = block.settings().applyStagedParameters();
    block.start();
}

constexpr double      kSampleRate = 48'000.0;
constexpr double      kPpm        = 20.0;
constexpr double      kSpacing    = kSampleRate * (1.0 + kPpm * 1e-6); // 48000.96 samples between pulses
constexpr std::size_t kRamp       = 128UZ;                             // samples the pulse takes to rise
constexpr std::size_t kHold       = 128UZ;
constexpr std::size_t kLead       = 512UZ;

/*| contract: a trapezoidal pulse train, a linear rise over kRamp samples, a flat top, a linear
        fall and zero between pulses, with pulse omit left out. The rise is linear where the
        threshold crosses it, and the block's two-point interpolation is exact there. A
        threshold at half the amplitude crosses at start + kRamp/2, and the interval between two
        crossings is the spacing.
*/
[[nodiscard]] std::vector<float> pulseTrain(std::size_t nPulses, std::optional<std::size_t> omit = {}) {
    const std::size_t  length = static_cast<std::size_t>(std::ceil(static_cast<double>(kLead) + static_cast<double>(nPulses) * kSpacing)) + 4UZ * kRamp;
    std::vector<float> out(length, 0.f);
    for (std::size_t p = 0UZ; p < nPulses; ++p) {
        if (omit.has_value() && *omit == p) {
            continue;
        }
        // the waveform is sampled on the integer grid, and the ramp through the threshold is
        // linear there wherever the pulse starts between samples
        const double      start = static_cast<double>(kLead) + static_cast<double>(p) * kSpacing;
        const std::size_t begin = static_cast<std::size_t>(std::ceil(start));
        for (std::size_t n = begin; n < begin + 2UZ * kRamp + kHold && n < out.size(); ++n) {
            const double along = static_cast<double>(n) - start;
            double       value = 0.;
            if (along < static_cast<double>(kRamp)) {
                value = along / static_cast<double>(kRamp);
            } else if (along < static_cast<double>(kRamp + kHold)) {
                value = 1.;
            } else {
                value = 1. - (along - static_cast<double>(kRamp + kHold)) / static_cast<double>(kRamp);
            }
            out[n] = static_cast<float>(std::max(0., value));
        }
    }
    return out;
}

[[nodiscard]] gr::property_map settings(double rate, gr::property_map extra = {}) {
    gr::property_map map{{"sample_rate", rate}, {"pps_interval", 1.0}, {"edge_source", std::string("threshold")}, {"threshold", 0.5}, {"n_intervals", gr::Size_t(16)}};
    for (auto& [key, value] : extra) {
        map.insert_or_assign(key, value);
    }
    return map;
}

// The prototype filter of a resampler bank of the given size, a 60 dB Kaiser lowpass.
[[nodiscard]] std::vector<float> prototypeFor(std::size_t bank) {
    const double      stopEdge = 0.5 / static_cast<double>(bank);
    const double      passEdge = 0.8 * stopEdge;
    const std::size_t length   = (2UZ * gr::filter::fir::design::kaiserLength(60.0, stopEdge - passEdge)) | 1UZ;

    std::vector<float> taps = gr::filter::fir::design::kaiserLowpass(length, 0.5 * (passEdge + stopEdge), 60.0);
    for (float& v : taps) {
        v *= static_cast<float>(bank);
    }
    return taps;
}

// The stream resampled at rate 1 + ppm * 1e-6, a clock correction of ppm.
[[nodiscard]] std::vector<float> resampled(std::span<const float> input, double ppm) {
    constexpr std::size_t         kBank = 32UZ;
    ArbitraryRateResampler<float> block{};
    init(block, {{"rate", 1.0 + ppm * 1e-6}, {"bank_size", gr::Size_t(kBank)}, {"taps", prototypeFor(kBank)}});
    return test::runVariable<float>(block, input, 65536UZ, 65536UZ).samples;
}

[[nodiscard]] gr::Tag triggerTag(std::size_t at, std::uint64_t timeNs, float offsetSeconds = 0.f, std::string name = "PPS_NTP") {
    gr::property_map map;
    map.insert_or_assign(gr::property_map::key_type{gr::tag::TRIGGER_NAME.shortKey()}, gr::pmt::Value(std::move(name)));
    map.insert_or_assign(gr::property_map::key_type{gr::tag::TRIGGER_TIME.shortKey()}, gr::pmt::Value(timeNs));
    map.insert_or_assign(gr::property_map::key_type{gr::tag::TRIGGER_OFFSET.shortKey()}, gr::pmt::Value(offsetSeconds));
    return gr::Tag{at, std::move(map)};
}

// The tag timesource::PpsSource writes for the second at timeNs, on a synchronized system clock.
[[nodiscard]] gr::Tag ppsSourceTag(timesource::PpsSource& source, std::size_t at, std::uint64_t timeNs) {
    timesource::KernelDiscipline discipline{};
    discipline.synchronized = true;
    const timesource::PpsTick tick{.triggerNs = timeNs, .wakeupOffsetNs = 0, .sequence = 0, .discipline = discipline, .ptpLessSystemNs = std::nullopt};
    return gr::Tag{at, source.makeTag(tick)};
}

[[nodiscard]] std::optional<double> metaDouble(const gr::DataSet<float>& record, std::string_view key) {
    if (record.meta_information.empty()) {
        return std::nullopt;
    }
    const auto found = record.meta_information.front().find(key);
    if (found == record.meta_information.front().end()) {
        return std::nullopt;
    }
    if (const double* value = found->second.get_if<double>(); value != nullptr) {
        return *value;
    }
    if (const std::uint64_t* value = found->second.get_if<std::uint64_t>(); value != nullptr) {
        return static_cast<double>(*value);
    }
    if (const float* value = found->second.get_if<float>(); value != nullptr) {
        return static_cast<double>(*value);
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<float> channel(const gr::DataSet<float>& record, std::string_view name) {
    for (std::size_t i = 0UZ; i < record.signal_names.size(); ++i) {
        if (record.signal_names[i] == name) {
            return record.signal_values[i];
        }
    }
    return std::nullopt;
}

} // namespace

int main(int argc, char** argv) {
    using namespace boost::ut;
    using CF = std::complex<float>;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    timesource::test::Cases cases(argc, argv);

    cases("correlator.measures-20-ppm-and-zero-after-a-correction", [] {
        const std::vector<float> train = pulseTrain(6UZ);

        PpsCorrelator<float> open{};
        init(open, settings(kSampleRate));
        std::ignore = test::run<gr::DataSet<float>>(open, std::span<const float>(train), 0UZ, {}, 16UZ);

        expect(eq(open.nEdges(), 6ULL));
        expect(eq(open.nIntervals(), 5ULL));
        expect(eq(open.nAcceptedIntervals(), 5ULL));
        expect(eq(open.nMissedEdges(), 0ULL));

        // one ppm of the interval is 0.048 samples here; the bound states where the two-point
        // interpolation puts a linear edge
        const double measured = open.rateErrorPpm();
        const double bound    = std::abs(measured - kPpm);
        std::println("[pps] open loop: {:.6f} ppm against a stated 20, error {:.2e} ppm = {:.2e} samples of interval on a linear edge", measured, bound, bound * 1e-6 * kSpacing);
        expect(lt(bound, 1e-3)) << std::format("measured {} ppm", measured);

        // a correction of -ppm through ArbitraryRateResampler at rate 1 + ppm * 1e-6
        const std::vector<float> corrected = resampled(std::span<const float>(train), -kPpm);
        PpsCorrelator<float>     closed{};
        init(closed, settings(kSampleRate));
        std::ignore = test::run<gr::DataSet<float>>(closed, std::span<const float>(corrected), 0UZ, {}, 16UZ);

        expect(eq(closed.nEdges(), 6ULL));
        expect(eq(closed.nAcceptedIntervals(), 5ULL));
        // Two residuals remain. A correction by -ppm in place of 1/(1 + ppm * 1e-6) leaves the
        // second-order term, 20e-6 * 20e-6 = 4e-10, or -4e-4 ppm. The resampler's 60 dB
        // prototype rounds the corners of the ramp and moves the crossing by a few thousandths of
        // a sample.
        const double residual = closed.rateErrorPpm();
        std::println("[pps] closed loop: {:.6f} ppm residual = {:.2e} samples of interval", residual, std::abs(residual) * 1e-6 * kSpacing);
        expect(lt(std::abs(residual), 0.15)) << std::format("residual {} ppm", residual);
    });

    cases("correlator.a-deleted-pulse-is-one-missed-edge", [] {
        PpsCorrelator<float> whole{};
        init(whole, settings(kSampleRate));
        const std::vector<float> full = pulseTrain(8UZ);
        std::ignore                   = test::run<gr::DataSet<float>>(whole, std::span<const float>(full), 0UZ, {}, 16UZ);

        PpsCorrelator<float> gapped{};
        init(gapped, settings(kSampleRate));
        const std::vector<float> holed = pulseTrain(8UZ, 3UZ);
        std::ignore                    = test::run<gr::DataSet<float>>(gapped, std::span<const float>(holed), 0UZ, {}, 16UZ);

        expect(eq(whole.nEdges(), 8ULL));
        expect(eq(whole.nIntervals(), 7ULL));
        expect(eq(whole.nMissedEdges(), 0ULL));

        expect(eq(gapped.nEdges(), 7ULL)) << "the deleted pulse is one edge fewer";
        expect(eq(gapped.nIntervals(), 6ULL));
        expect(eq(gapped.nAcceptedIntervals(), 5ULL)) << "the double-length interval is refused as a measurement";
        expect(eq(gapped.nMissedEdges(), 1ULL)) << "one pulse counts as missed";
        expect(eq(gapped.nExtraEdges(), 0ULL));
        // the two means cover different intervals and agree to the edge-location floor; a
        // refused interval of 1e6 ppm in the mean would move it by five orders of magnitude
        std::println("[pps] missed-edge arm: {:.6f} ppm against {:.6f} ppm on the whole train", gapped.rateErrorPpm(), whole.rateErrorPpm());
        expect(lt(std::abs(gapped.rateErrorPpm() - whole.rateErrorPpm()), 1e-3)) << "the refused interval stays out of the mean";
    });

    cases("correlator.the-account-is-independent-of-the-chunking", [] {
        const std::vector<float> train = pulseTrain(6UZ);

        PpsCorrelator<float> reference{};
        init(reference, settings(kSampleRate));
        std::ignore         = test::run<gr::DataSet<float>>(reference, std::span<const float>(train), 0UZ, {}, 16UZ);
        const double wanted = reference.rateErrorPpm();

        for (const std::size_t chunk : {1UZ, 13UZ, 997UZ, 65536UZ}) {
            PpsCorrelator<float> block{};
            init(block, settings(kSampleRate));
            const auto capture = test::run<gr::DataSet<float>>(block, std::span<const float>(train), chunk, {}, 16UZ);
            expect(that % (block.rateErrorPpm() == wanted)) << std::format("chunk {}: bit-identical", chunk);
            expect(eq(block.nEdges(), 6ULL)) << std::format("chunk {}", chunk);
            expect(eq(capture.samples.size(), 5UZ)) << std::format("chunk {}: one record per closed interval", chunk);
        }
    });

    cases("correlator.a-complex-stream-reads-the-same", [] {
        const std::vector<float> train = pulseTrain(6UZ);
        std::vector<CF>          iq(train.size());
        for (std::size_t k = 0UZ; k < train.size(); ++k) {
            iq[k] = CF(train[k] * 0.6f, train[k] * 0.8f); // the magnitude is the real train, rotated off both axes
        }

        PpsCorrelator<float> real{};
        init(real, settings(kSampleRate));
        std::ignore = test::run<gr::DataSet<float>>(real, std::span<const float>(train), 0UZ, {}, 16UZ);

        PpsCorrelator<CF> complexInput{};
        init(complexInput, settings(kSampleRate));
        std::ignore = test::run<gr::DataSet<float>>(complexInput, std::span<const CF>(iq), 0UZ, {}, 16UZ);

        expect(eq(complexInput.nEdges(), 6ULL));
        expect(lt(std::abs(complexInput.rateErrorPpm() - real.rateErrorPpm()), 1e-3)) << "the block thresholds the magnitude";
    });

    // PpsSource writes uint8 zeros and a tag on the first sample of each second
    cases("correlator.pps-source-tags-are-an-edge-stream", [] {
        constexpr double      kClockRate = 8.0; // PpsSource in clock mode writes sample_rate samples per second
        constexpr std::size_t kSeconds   = 5UZ;

        std::vector<std::uint8_t> stream(static_cast<std::size_t>(kClockRate) * (kSeconds + 1UZ), std::uint8_t{0});
        timesource::PpsSource     source{};
        std::vector<gr::Tag>      tags;
        for (std::size_t s = 0UZ; s < kSeconds; ++s) {
            tags.push_back(ppsSourceTag(source, s * static_cast<std::size_t>(kClockRate), 1'700'000'000'000'000'000ULL + s * 1'000'000'000ULL));
        }
        expect(timesource::test::value<std::string>(tags.front().map, "trigger_name") == std::optional<std::string>{"PPS_NTP"}) << "the source's name for a locked NTP second";

        PpsCorrelator<std::uint8_t> block{};
        init(block, settings(kClockRate, {{"edge_source", std::string("trigger_tag")}, {"trigger_filter", std::string("PPS")}}));
        const auto capture = test::run<gr::DataSet<float>>(block, std::span<const std::uint8_t>(stream), 3UZ, std::span<const gr::Tag>(tags), 16UZ);

        expect(eq(block.nEdges(), 5ULL)) << "all-zero samples cross no threshold; the tags are the edges";
        expect(eq(block.nIntervals(), 4ULL));
        expect(eq(block.nAcceptedIntervals(), 4ULL));
        expect(that % (block.rateErrorPpm() == 0.0)) << "eight samples per second against a nominal eight";
        expect(eq(capture.samples.size(), 4UZ));

        // trigger_offset is a delay in seconds, and one offset on every tag moves every edge by rate * offset
        std::vector<gr::Tag> shifted;
        for (std::size_t s = 0UZ; s < kSeconds; ++s) {
            shifted.push_back(triggerTag(s * static_cast<std::size_t>(kClockRate), 0ULL, 0.125f)); // 0.125 s at 8 Hz is one sample
        }
        PpsCorrelator<std::uint8_t> delayed{};
        init(delayed, settings(kClockRate, {{"edge_source", std::string("trigger_tag")}}));
        const auto delayedRun = test::run<gr::DataSet<float>>(delayed, std::span<const std::uint8_t>(stream), 3UZ, std::span<const gr::Tag>(shifted), 16UZ);
        expect(that % (delayed.rateErrorPpm() == 0.0)) << "a common delay cancels in every interval";
        expect(eq(metaDouble(delayedRun.samples.at(0UZ), "edge_index").value_or(-1.), 9.)) << "the second edge sits one sample past its tag";

        // a name outside the filter gives no edge
        PpsCorrelator<std::uint8_t> filtered{};
        init(filtered, settings(kClockRate, {{"edge_source", std::string("trigger_tag")}, {"trigger_filter", std::string("GPS")}}));
        std::ignore = test::run<gr::DataSet<float>>(filtered, std::span<const std::uint8_t>(stream), 3UZ, std::span<const gr::Tag>(tags), 16UZ);
        expect(eq(filtered.nEdges(), 0ULL)) << "trigger_filter selects the source whose edges count";
    });

    cases("correlator.records-state-their-interval", [] {
        const std::vector<float> train = pulseTrain(6UZ);

        PpsCorrelator<float> block{};
        init(block, settings(kSampleRate));
        const auto capture = test::run<gr::DataSet<float>>(block, std::span<const float>(train), 0UZ, {}, 16UZ);

        expect(eq(capture.samples.size(), 5UZ)) << "one record per closed interval, none for the interval still open";
        const gr::DataSet<float>& first = capture.samples.at(0UZ);
        expect(eq(first.signal_names.size(), 3UZ));
        expect(channel(first, "rate_error_ppm").has_value());
        expect(channel(first, "edge_fraction").has_value());
        expect(eq(channel(first, "accepted").value_or(0.f), 1.f));
        expect(lt(std::abs(static_cast<double>(channel(first, "rate_error_ppm").value_or(0.f)) - kPpm), 1e-2));
        expect(eq(metaDouble(first, "interval_index").value_or(-1.), 0.));
        expect(eq(metaDouble(first, "sample_rate").value_or(-1.), kSampleRate));
        expect(lt(std::abs(metaDouble(first, "interval_samples").value_or(0.) - kSpacing), 1e-2));
        // sample_start is the whole sample the interval opened on: the first edge, at kLead + kRamp/2
        expect(eq(metaDouble(first, "sample_start").value_or(-1.), static_cast<double>(kLead + kRamp / 2UZ)));

        for (std::size_t i = 1UZ; i < capture.samples.size(); ++i) {
            expect(eq(metaDouble(capture.samples[i], "interval_index").value_or(-1.), static_cast<double>(i)));
        }
    });

    cases("correlator.refuses-settings-that-cannot-be-met", [] {
        expect(throws([] {
            PpsCorrelator<float> block{};
            init(block, settings(kSampleRate, {{"edge_source", std::string("magic")}}));
        })) << "an unknown edge source is named and refused";
        expect(throws([] {
            PpsCorrelator<float> block{};
            init(block, settings(-1.0));
        }));
        expect(throws([] {
            PpsCorrelator<float> block{};
            init(block, settings(kSampleRate, {{"n_intervals", gr::Size_t(0)}}));
        }));
    });

    const int rc = cases.finish();
    std::fflush(stdout);
    std::_Exit(rc);
}
