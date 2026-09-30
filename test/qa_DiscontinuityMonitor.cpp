/* -*- c++ -*- */
/*
 * Copyright 2026 Jeff Long
 * SPDX-License-Identifier: MIT
 */
#include <timesource/DiscontinuityMonitor.hpp>

#include "support/Cases.hpp"
#include "support/Spans.hpp"

#include <gnuradio-4.0/Graph.hpp>
#include <gnuradio-4.0/Scheduler.hpp>
#include <gnuradio-4.0/testing/TagMonitors.hpp>

#include <algorithm>
#include <array>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using timesource::DiscontinuityCause;
using timesource::DiscontinuityMonitor;
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

[[nodiscard]] gr::Tag dropped(std::size_t at, gr::Size_t count, std::string causes = "gap") {
    gr::property_map map;
    map.insert_or_assign(gr::property_map::key_type{gr::tag::N_DROPPED_SAMPLES.shortKey()}, gr::pmt::Value(count));
    map.insert_or_assign(gr::property_map::key_type{"discontinuity"}, gr::pmt::Value(causes));
    return gr::Tag{at, std::move(map)};
}

// A count with no cause list, which the total still carries.
[[nodiscard]] gr::Tag countOnly(std::size_t at, gr::Size_t count) {
    gr::property_map map;
    map.insert_or_assign(gr::property_map::key_type{gr::tag::N_DROPPED_SAMPLES.shortKey()}, gr::pmt::Value(count));
    return gr::Tag{at, std::move(map)};
}

[[nodiscard]] gr::Tag causeOnly(std::size_t at, std::string causes) {
    gr::property_map map;
    map.insert_or_assign(gr::property_map::key_type{"discontinuity"}, gr::pmt::Value(causes));
    return gr::Tag{at, std::move(map)};
}

[[nodiscard]] gr::Tag rateTag(std::size_t at, float rate) {
    gr::property_map map;
    map.insert_or_assign(gr::property_map::key_type{gr::tag::SAMPLE_RATE.shortKey()}, gr::pmt::Value(rate));
    return gr::Tag{at, std::move(map)};
}

[[nodiscard]] std::vector<float> ramp(std::size_t n) {
    std::vector<float> out(n);
    for (std::size_t k = 0UZ; k < n; ++k) {
        out[k] = static_cast<float>(k) * 0.25f;
    }
    return out;
}

// Four events naming eight causes between them, and 1150 dropped samples.
[[nodiscard]] std::vector<gr::Tag> ledger() {
    return {
        dropped(10UZ, 100U),                                       //
        causeOnly(200UZ, "sample_rate,signal_unit"),               //
        dropped(400UZ, 1000U, "gap,signal_range"),                 //
        causeOnly(700UZ, "signal_name,signal_quantity,who_knows"), //
        countOnly(900UZ, 50U),                                     //
    };
}

/*| contract: the indices at which a tag key outside gr::tag::kDefaultTags reaches a sink through
        the monitor, in a graph run by the scheduler.
*/
[[nodiscard]] std::vector<std::size_t> forwardedOffsets(std::string_view key) {
    using gr::testing::ProcessFunction;
    using gr::testing::TagSink;
    using gr::testing::TagSource;

    const gr::property_map::key_type wanted{key};

    gr::Graph graph;
    auto&     source = graph.emplaceBlock<TagSource<float, ProcessFunction::USE_PROCESS_BULK>>({{"n_samples_max", gr::Size_t(2048)}, {"mark_tag", false}});
    for (const std::size_t at : {7UZ, 300UZ, 1000UZ}) {
        gr::property_map map;
        map.insert_or_assign(wanted, gr::pmt::Value(std::string("gap")));
        source._tags.emplace_back(at, std::move(map));
    }
    auto& block = graph.emplaceBlock<DiscontinuityMonitor<float>>({{"nominal_rate", 0.0}});
    auto& sink  = graph.emplaceBlock<TagSink<float, ProcessFunction::USE_PROCESS_ONE>>({{"name", "TagSink"}});

    boost::ut::expect(graph.connect<"out", "in">(source, block).has_value());
    boost::ut::expect(graph.connect<"out", "in">(block, sink).has_value());

    gr::scheduler::Simple scheduler;
    boost::ut::expect(scheduler.exchange(std::move(graph)).has_value());
    boost::ut::expect(scheduler.runAndWait().has_value());

    std::vector<std::size_t> offsets;
    for (const gr::Tag& tag : sink._tags) {
        if (tag.map.contains(wanted)) {
            offsets.push_back(tag.index);
        }
    }
    return offsets;
}

} // namespace

int main(int argc, char** argv) {
    using namespace boost::ut;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    timesource::test::Cases cases(argc, argv);

    cases("discontinuity.totals-dropped-samples-and-counts-every-cause", [] {
        const std::vector<float>   input = ramp(1200UZ);
        const std::vector<gr::Tag> tags  = ledger();

        DiscontinuityMonitor<float> block{};
        init(block, {{"nominal_rate", 0.0}});
        const auto result = test::run<float>(block, std::span<const float>(input), 0UZ, std::span<const gr::Tag>(tags));

        expect(eq(result.consumed, 1200UZ));
        expect(eq(block.nDroppedSamples(), 1150ULL)) << "100 + 1000 + 50, exactly";
        expect(eq(block.nEvents(), 4ULL)) << "four tags carried a discontinuity list; the fifth carried only a count";
        expect(eq(block.nEvents(DiscontinuityCause::Gap), 2ULL));
        expect(eq(block.nEvents(DiscontinuityCause::SampleRate), 1ULL));
        expect(eq(block.nEvents(DiscontinuityCause::SignalUnit), 1ULL));
        expect(eq(block.nEvents(DiscontinuityCause::SignalRange), 1ULL));
        expect(eq(block.nEvents(DiscontinuityCause::SignalName), 1ULL));
        expect(eq(block.nEvents(DiscontinuityCause::SignalQuantity), 1ULL));
        expect(eq(block.nEvents(DiscontinuityCause::Other), 1ULL)) << "an unknown cause name counts as other";
        expect(eq(block.nRateChanges(), 0ULL));
        expect(eq(block.nRateMismatches(), 0ULL));
    });

    cases("discontinuity.a-rate-change-re-anchors-and-both-epochs-convert-exactly", [] {
        constexpr std::int64_t     kEpochNs = 1'756'000'000'000'000'000LL; // a wall-clock nanosecond far past 2^53
        const std::vector<float>   input    = ramp(2000UZ);
        const std::vector<gr::Tag> tags{rateTag(1000UZ, 200'000.f)};

        DiscontinuityMonitor<float> block{};
        init(block, {{"nominal_rate", 0.0}, {"anchor_index", std::uint64_t{0}}, {"anchor_ns", kEpochNs}});

        // the first epoch: 250 kS/s, taken from the first tag of the stream
        const std::vector<gr::Tag> first{rateTag(0UZ, 250'000.f)};
        std::ignore = test::run<float>(block, std::span<const float>(input).first(1UZ), 0UZ, std::span<const gr::Tag>(first));
        expect(block.hasClock());
        const gr::timing::SampleClock before = block.clock();
        expect(eq(before.timeOf(0ULL), kEpochNs));
        expect(eq(before.timeOf(1000ULL), kEpochNs + 4'000'000LL)) << "1000 samples at 250 kS/s is exactly 4 ms";
        expect(eq(before.timeOf(999ULL), kEpochNs + 3'996'000LL));

        const auto second = test::run<float>(block, std::span<const float>(input).subspan(1UZ), 0UZ, std::span<const gr::Tag>(tags), 0UZ, 1UZ);
        expect(eq(second.consumed, 1999UZ));
        expect(eq(block.nRateChanges(), 2ULL)) << "the stream's first stated rate and the change at 1000";

        const gr::timing::SampleClock after = block.clock();
        expect(eq(after.timeOf(1000ULL), before.timeOf(1000ULL))) << "the anchor sample reads the same time on either clock";
        expect(eq(after.timeOf(1500ULL), kEpochNs + 4'000'000LL + 2'500'000LL)) << "500 samples at 200 kS/s is exactly 2.5 ms";
        expect(eq(after.timeOf(2000ULL), kEpochNs + 4'000'000LL + 5'000'000LL));
        expect(eq(after.indexOf(kEpochNs + 4'000'000LL + 2'500'000LL).index, 1500ULL));
        expect(eq(after.indexOf(kEpochNs + 4'000'000LL + 2'500'000LL).remainder_num, 0ULL)) << "the conversion back is exact";
    });

    cases("discontinuity.refuses-counts-and-names-a-rate-outside-the-tolerance", [] {
        const std::vector<float>   input = ramp(400UZ);
        const std::vector<gr::Tag> tags{rateTag(100UZ, 96'000.f), rateTag(200UZ, 48'000.5f), rateTag(300UZ, 48'000.f)};

        DiscontinuityMonitor<float> block{};
        init(block, {{"nominal_rate", 48'000.0}, {"rate_tolerance", 1e-5}});
        expect(block.hasClock()) << "a positive nominal_rate gives the clock its rate";
        std::ignore = test::run<float>(block, std::span<const float>(input), 64UZ, std::span<const gr::Tag>(tags));

        expect(eq(block.nRateMismatches(), 2ULL)) << "96 kHz and 48000.5 Hz both differ by more than 1e-5 relative";
        expect(eq(block.lastRefusedRate(), 48'000.5)) << "the last refused rate is kept";
        expect(eq(block.nRateChanges(), 0ULL)) << "the clock keeps its rate; 48 kHz restated changes nothing";
        expect(eq(block.clock().rate_num, 48'000'000'000ULL));
        expect(eq(block.clock().rate_den, 1'000'000ULL));
    });

    cases("discontinuity.the-account-is-independent-of-the-chunking", [] {
        const std::vector<float>   input = ramp(1200UZ);
        const std::vector<gr::Tag> tags  = ledger();

        DiscontinuityMonitor<float> reference{};
        init(reference, {{"nominal_rate", 0.0}});
        const auto whole  = test::run<float>(reference, std::span<const float>(input), 0UZ, std::span<const gr::Tag>(tags));
        const auto wanted = reference.account();

        expect(eq(whole.samples.size(), input.size()));
        expect(std::ranges::equal(whole.samples, input)) << "the samples pass through bit for bit";

        for (const std::size_t chunk : {1UZ, 7UZ, 128UZ, 1199UZ}) {
            DiscontinuityMonitor<float> block{};
            init(block, {{"nominal_rate", 0.0}});
            const auto result = test::run<float>(block, std::span<const float>(input), chunk, std::span<const gr::Tag>(tags));
            expect(eq(result.samples.size(), input.size())) << std::format("chunk {}", chunk);
            expect(std::ranges::equal(result.samples, input)) << std::format("chunk {}", chunk);
            expect(eq(block.nDroppedSamples(), 1150ULL)) << std::format("chunk {}", chunk);
            expect(block.account() == wanted) << std::format("chunk {}: the whole account", chunk);
        }
    });

    cases("discontinuity.every-type-passes-its-samples-through", [] {
        const std::vector<gr::Tag> tags = ledger();

        std::vector<std::uint8_t> bytes(1200UZ);
        for (std::size_t k = 0UZ; k < bytes.size(); ++k) {
            bytes[k] = static_cast<std::uint8_t>(k & 0xFFUZ);
        }
        DiscontinuityMonitor<std::uint8_t> byteBlock{};
        init(byteBlock, {{"nominal_rate", 0.0}});
        const auto byteRun = test::run<std::uint8_t>(byteBlock, std::span<const std::uint8_t>(bytes), 97UZ, std::span<const gr::Tag>(tags));
        expect(std::ranges::equal(byteRun.samples, bytes)) << "std::uint8_t, the sample type of the timing sources";
        expect(eq(byteBlock.nDroppedSamples(), 1150ULL));

        std::vector<std::complex<float>> iq(1200UZ);
        for (std::size_t k = 0UZ; k < iq.size(); ++k) {
            iq[k] = std::complex<float>(static_cast<float>(k), -static_cast<float>(k));
        }
        DiscontinuityMonitor<std::complex<float>> iqBlock{};
        init(iqBlock, {{"nominal_rate", 0.0}});
        const auto iqRun = test::run<std::complex<float>>(iqBlock, std::span<const std::complex<float>>(iq), 97UZ, std::span<const gr::Tag>(tags));
        expect(std::ranges::equal(iqRun.samples, iq));
        expect(eq(iqBlock.nDroppedSamples(), 1150ULL));
    });

    // `discontinuity` is outside the reserved tags, and filtered propagation drops such a key
    cases("discontinuity.forwards-a-key-the-default-filter-drops", [] {
        static_assert(DiscontinuityMonitor<float>::unfilteredTagPropagation, "the block forwards every key");
        expect(!std::ranges::contains(gr::tag::kDefaultTags, std::string_view("discontinuity"))) << "the key is outside the reserved tags";

        const std::vector<std::size_t> offsets = forwardedOffsets("discontinuity");
        expect(eq(offsets.size(), 3UZ)) << "all three tags reach the sink";
        expect(offsets == std::vector<std::size_t>{7UZ, 300UZ, 1000UZ}) << "each at the index it arrived at";
    });

    const int rc = cases.finish();
    std::fflush(stdout);
    std::_Exit(rc);
}
