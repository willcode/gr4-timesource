/* -*- c++ -*- */
/*
 * Copyright 2026 Jeff Long
 * SPDX-License-Identifier: MIT
 */
#include <timesource/NmeaParser.hpp>

#include "support/Cases.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <format>
#include <string>
#include <string_view>

namespace {

using namespace std::chrono_literals;

// A sentence with its checksum, from the text between the dollar sign and the asterisk.
std::string sentence(std::string_view body) { return std::format("${}*{:02X}", body, timesource::nmea::checksum(body)); }

std::uint64_t utc(std::chrono::sys_days day, std::chrono::nanoseconds since) { return static_cast<std::uint64_t>((std::chrono::duration_cast<std::chrono::nanoseconds>(day.time_since_epoch()) + since).count()); }

bool near(double a, double b, double tolerance) { return std::fabs(a - b) <= tolerance; }

constexpr std::chrono::sys_days kMarch11{std::chrono::year{2026} / 3 / 11};

} // namespace

int main(int argc, char** argv) {
    using namespace boost::ut;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    timesource::test::Cases cases(argc, argv);
    using timesource::NmeaParser;

    cases("nmea.checksum-of-a-known-sentence", [] {
        using timesource::nmea::body;
        expect(body("$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47") == std::optional<std::string_view>{"GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,"}) << "the published GGA example checks";
        expect(body("$GPRMC,225444,A,4916.45,N,12311.12,W,000.5,054.7,191194,020.3,E*6A\r").has_value()) << "an RMC checks, carriage return and all";
        expect(body("$GPVTG,054.7,T,034.4,M,005.5,N,010.2,K*48").has_value()) << "the published VTG example checks";
        expect(body("\x01\x02$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47").has_value()) << "bytes ahead of the dollar sign are skipped";
        expect(body("$GPTXT,01,01,02,lower*2e").has_value() && body("$GPTXT,01,01,02,lower*2E").has_value()) << "hexadecimal digits of either case check";
        expect(body("$GPZDA,201530.00,04,07,2002,00,00").has_value()) << "a sentence without a checksum is taken unchecked";
    });

    cases("nmea.rmc-fields", [] {
        NmeaParser p;
        expect(!p.parseLine(sentence("GPRMC,120000.00,A,5001.1900,N,00840.6570,E,0.5,45.0,110326,,,A"), 1000).has_value()) << "the first second has nothing to close";
        const auto edge = p.parseLine(sentence("GPRMC,120001.00,A,5001.1900,N,00840.6570,E,0.5,45.0,110326,,,A"), 2000);
        expect(fatal(edge.has_value())) << "the next second closes the first";
        const auto& fix = edge->previous;
        expect(fix.valid && fix.hasPosition && fix.hasTime) << "status A is a valid fix with a position and a time";
        expect(near(fix.latitude, 50.0 + 1.19 / 60.0, 1e-9)) << "5001.1900 N is 50 degrees 1.19 minutes";
        expect(near(fix.longitude, 8.0 + 40.657 / 60.0, 1e-9)) << "00840.6570 E is 8 degrees 40.657 minutes";
        expect(near(fix.speedKmh, 0.5 * 1.852, 1e-5)) << "the speed arrives in knots";
        expect(near(fix.headingDeg, 45.0, 1e-6)) << "the course over ground";
        expect(eq(fix.utcTimestampNs, utc(kMarch11, 12h))) << "time and date of the first second";
        expect(eq(fix.localTimeNs, 2000ULL)) << "the arrival of the sentence that closed it";
        expect(edge->hasTime && edge->utcNs == utc(kMarch11, 12h + 1s)) << "the edge is the whole second the closing sentence names";
        expect(eq(p.counts().rmc, 2UZ));
    });

    cases("nmea.gga-fields", [] {
        NmeaParser p;
        expect(!p.parseLine("$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47", 1).has_value());
        const auto& open = p.pendingFix();
        expect(open.valid && open.hasPosition) << "quality 1 is a valid fix with a position";
        expect(near(open.latitude, 48.0 + 7.038 / 60.0, 1e-9) && near(open.longitude, 11.0 + 31.0 / 60.0, 1e-9));
        expect(open.satellites == 8 && near(open.hdop, 0.9, 1e-6) && near(open.altitude, 545.4, 1e-4)) << "satellites used, HDOP and altitude";
        expect(!open.hasTime) << "a GGA carries no date, so alone it gives no time";
        const auto edge = p.parseLine(sentence("GPGGA,123520,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,"), 2);
        expect(fatal(edge.has_value())) << "a GGA opens a second";
        expect(!edge->hasTime) << "and states no time without a date";
        expect(!p.parseLine(sentence("GPGGA,123520,4807.038,N,01131.000,E,0,00,,,M,,M,,"), 3).has_value());
        expect(eq(p.pendingFix().satellites, 0)) << "the satellite count is read at any quality";
    });

    cases("nmea.zda-fields", [] {
        NmeaParser p;
        expect(!p.parseLine(sentence("GPZDA,201530.25,04,07,2002,00,00"), 1).has_value());
        constexpr std::chrono::sys_days july4{std::chrono::year{2002} / 7 / 4};
        expect(p.pendingFix().hasTime && p.pendingFix().utcTimestampNs == utc(july4, 20h + 15min + 30s + 250ms)) << "the four-digit year, the date and the fraction of the second";
        const auto edge = p.parseLine(sentence("GPGGA,201531.00,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,"), 2);
        expect(fatal(edge.has_value()));
        expect(edge->hasTime && edge->utcNs == utc(july4, 20h + 15min + 31s)) << "a later GGA takes the date the ZDA gave";
        expect(eq(p.counts().zda, 1UZ));
    });

    cases("nmea.gsa-fields", [] {
        NmeaParser p;
        std::ignore = p.parseLine(sentence("GPGSA,A,3,04,05,09,12,,,,,,,,,1.8,1.0,1.5"), 1);
        expect(p.pendingFix().fixType == timesource::FixType::fix3D && p.pendingFix().valid) << "mode 3 is a 3D fix";
        expect(near(p.pendingFix().hdop, 1.0, 1e-6)) << "HDOP is the sixteenth field";
        std::ignore = p.parseLine(sentence("GPGSA,A,2,04,05,09,,,,,,,,,,2.5,2.0,1.5"), 2);
        expect(p.pendingFix().fixType == timesource::FixType::fix2D) << "mode 2 is a 2D fix";
        NmeaParser none;
        std::ignore = none.parseLine(sentence("GPGSA,A,1,,,,,,,,,,,,,99.99,99.99,99.99"), 1);
        expect(none.pendingFix().fixType == timesource::FixType::none && !none.pendingFix().valid) << "mode 1 is no fix";
        expect(eq(static_cast<int>(timesource::FixType::fix3D), 3)) << "the enumeration holds the GSA codes";
    });

    cases("nmea.vtg-fields", [] {
        NmeaParser p;
        std::ignore = p.parseLine("$GPVTG,054.7,T,034.4,M,005.5,N,010.2,K*48", 1);
        expect(near(p.pendingFix().headingDeg, 54.7, 1e-5) && near(p.pendingFix().speedKmh, 10.2, 1e-5)) << "true course and km/h";
        NmeaParser knots;
        std::ignore = knots.parseLine(sentence("GPVTG,054.7,T,,M,005.5,N,,K,A"), 1);
        expect(near(knots.pendingFix().speedKmh, 5.5 * 1.852, 1e-4)) << "knots where the km/h field is empty";
        NmeaParser invalid;
        std::ignore = invalid.parseLine(sentence("GPVTG,054.7,T,,M,005.5,N,010.2,K,N"), 1);
        expect(eq(invalid.pendingFix().speedKmh, 0.f)) << "mode N marks the data not valid";
    });

    cases("nmea.a-new-second-closes-the-record", [] {
        NmeaParser p;
        expect(!p.parseLine(sentence("GPRMC,120000.00,A,5001.1900,N,00840.6570,E,0.5,45.0,110326,,,A"), 10).has_value());
        expect(!p.parseLine(sentence("GPGGA,120000.00,5001.1900,N,00840.6570,E,1,10,0.8,136.0,M,47.0,M,,"), 11).has_value());
        expect(!p.parseLine(sentence("GPGSA,A,3,04,05,09,12,,,,,,,,,1.8,1.0,1.5"), 12).has_value());
        expect(!p.parseLine(sentence("GPRMC,120000.50,A,5001.1900,N,00840.6570,E,0.5,45.0,110326,,,A"), 13).has_value()) << "a later fraction of the same second closes nothing";
        const auto edge = p.parseLine(sentence("GPGGA,120001.00,5001.1900,N,00840.6570,E,1,11,0.8,136.0,M,47.0,M,,"), 20);
        expect(fatal(edge.has_value()));
        const auto& fix = edge->previous;
        expect(fix.fixType == timesource::FixType::fix3D && fix.satellites == 10 && near(fix.altitude, 136.0, 1e-4) && near(fix.speedKmh, 0.926, 1e-4)) << "the record gathers every sentence of its second";
        expect(near(fix.hdop, 1.0, 1e-6)) << "the GSA after the GGA gives the HDOP last";
        expect(eq(fix.localTimeNs, 20ULL)) << "stamped with the arrival of the closing line";
        expect(p.pendingFix().satellites == 11 && p.pendingFix().fixType == timesource::FixType::none) << "the new second starts a fresh record";
        expect(eq(p.lastFix().satellites, 10)) << "lastFix is the closed record";
    });

    cases("nmea.a-second-carries-the-date-across-a-midnight", [] {
        NmeaParser p;
        std::ignore = p.parseLine(sentence("GPRMC,235959.00,A,5001.1900,N,00840.6570,E,0.0,0.0,110326,,,A"), 1);
        const auto edge = p.parseLine(sentence("GPGGA,000000.00,5001.1900,N,00840.6570,E,1,10,0.8,136.0,M,47.0,M,,"), 2);
        expect(fatal(edge.has_value()));
        expect(edge->hasTime && edge->utcNs == utc(kMarch11 + std::chrono::days{1}, 0s)) << "the GGA after midnight belongs to the next day";
    });

    cases("nmea.a-leap-second-is-23-59-59-plus-one-second", [] {
        constexpr std::chrono::sys_days kDecember31{std::chrono::year{2016} / 12 / 31};
        NmeaParser                      p;
        std::ignore     = p.parseLine(sentence("GPRMC,235958.00,A,5001.1900,N,00840.6570,E,0.0,0.0,311216,,,A"), 1);
        const auto s59  = p.parseLine(sentence("GPRMC,235959.00,A,5001.1900,N,00840.6570,E,0.0,0.0,311216,,,A"), 2);
        const auto s60  = p.parseLine(sentence("GPRMC,235960.00,A,5001.1900,N,00840.6570,E,0.0,0.0,311216,,,A"), 3);
        const auto s00  = p.parseLine(sentence("GPRMC,000000.00,A,5001.1900,N,00840.6570,E,0.0,0.0,010117,,,A"), 4);
        const auto s01  = p.parseLine(sentence("GPRMC,000001.00,A,5001.1900,N,00840.6570,E,0.0,0.0,010117,,,A"), 5);
        expect(fatal(s59.has_value() && s60.has_value() && s00.has_value() && s01.has_value())) << "one edge for each second, the leap second included";
        expect(s59->utcNs == utc(kDecember31, 23h + 59min + 59s) && !s59->leapSecond);
        expect(s60->utcNs == utc(kDecember31, 23h + 59min + 59s) && s60->leapSecond) << "23:59:60 is 23:59:59 plus one second";
        expect(s00->utcNs == utc(kDecember31 + std::chrono::days{1}, 0s) && !s00->leapSecond) << "the next second is the next day's first";
        expect(eq(s01->utcNs - s00->utcNs, 1'000'000'000ULL) && !s01->leapSecond);
        expect(eq(p.counts().rejected, 0UZ)) << "no line of the sequence is refused";
        std::ignore = p.parseLine(sentence("GPRMC,123060.00,A,5001.1900,N,00840.6570,E,0.0,0.0,010117,,,A"), 6);
        expect(eq(p.counts().rejected, 1UZ)) << "second 60 outside 23:59 is refused";
    });

    cases("nmea.any-talker-prefix", [] {
        NmeaParser p;
        int        second = 0;
        for (const std::string_view talker : {"GP", "GN", "GL", "GA", "BD", "GB"}) {
            std::ignore = p.parseLine(sentence(std::format("{}RMC,1200{:02d}.00,A,5001.1900,N,00840.6570,E,0.0,0.0,110326,,,A", talker, second++)), 1);
        }
        expect(eq(p.counts().rmc, 6UZ)) << "every talker's RMC is read";
        expect(eq(p.counts().rejected + p.counts().other, 0UZ)) << "and nothing is refused";
    });

    cases("nmea.a-bad-checksum-changes-nothing", [] {
        NmeaParser p;
        std::ignore     = p.parseLine(sentence("GPRMC,120000.00,A,5001.1900,N,00840.6570,E,0.0,0.0,110326,,,A"), 1);
        std::string bad = sentence("GPRMC,120001.00,A,5001.1900,N,00840.6570,E,0.0,0.0,110326,,,A");
        bad[bad.size() - 1] = bad[bad.size() - 1] == '0' ? '1' : '0';
        expect(!p.parseLine(bad, 2).has_value()) << "a wrong checksum closes no second";
        expect(!p.parseLine(sentence("GPRMC,120001.00,A,5001.1900,N,00840.6570,E,0.0,0.0,110326,,,A") + "0", 3).has_value()) << "three checksum digits are refused";
        expect(eq(p.counts().rejected, 2UZ));
        expect(p.parseLine(sentence("GPRMC,120001.00,A,5001.1900,N,00840.6570,E,0.0,0.0,110326,,,A"), 4).has_value()) << "the good line still finds the second open";
    });

    cases("nmea.a-truncated-sentence-changes-nothing", [] {
        NmeaParser p;
        std::ignore = p.parseLine(sentence("GPRMC,120000.00,A,5001.1900,N,00840.6570,E,0.0,0.0,110326,,,A"), 1);
        expect(!p.parseLine(sentence("GPRMC,120001.00,A,50"), 2).has_value()) << "an RMC cut short, checksum and all";
        expect(!p.parseLine("$GPGGA,120001.00,5001.1900,N", 3).has_value()) << "a GGA cut off in transit";
        expect(!p.parseLine("$GP", 4).has_value()) << "a bare address";
        expect(!p.parseLine(sentence("GPGSA,A,3,04"), 5).has_value()) << "a GSA cut short";
        expect(eq(p.counts().rejected, 4UZ));
        expect(p.pendingFix().fixType == timesource::FixType::none) << "the short GSA set no fix type";
        expect(p.parseLine(sentence("GPRMC,120001.00,A,5001.1900,N,00840.6570,E,0.0,0.0,110326,,,A"), 6).has_value()) << "the second is still open";
    });

    cases("nmea.an-empty-field-keeps-the-default", [] {
        NmeaParser p;
        std::ignore = p.parseLine(sentence("GPGGA,120000.00,5001.1900,N,00840.6570,E,1,07,,,M,,M,,"), 1);
        expect(p.pendingFix().satellites == 7 && p.pendingFix().hasPosition) << "the fields that are present are read";
        expect(p.pendingFix().hdop == 0.f && p.pendingFix().altitude == 0.f) << "an empty HDOP and altitude keep their defaults";
        std::ignore = p.parseLine(sentence("GPRMC,120000.00,A,5001.1900,N,00840.6570,E,,,110326,,,A"), 2);
        expect(p.pendingFix().speedKmh == 0.f && p.pendingFix().headingDeg == 0.f) << "an empty speed and course keep their defaults";
        std::ignore = p.parseLine(sentence("GPRMC,120000.00,A,,,,,0.0,0.0,110326,,,A"), 3);
        expect(near(p.pendingFix().latitude, 50.0 + 1.19 / 60.0, 1e-9)) << "an empty position leaves the position of the GGA";
    });

    cases("nmea.a-sentence-without-time-opens-no-second", [] {
        NmeaParser p;
        for (int i = 0; i < 3; ++i) {
            expect(!p.parseLine(sentence("GPRMC,,V,,,,,,,,,,N"), 1).has_value());
            expect(!p.parseLine(sentence("GPGGA,,,,,,0,00,99.99,,,,,,"), 2).has_value());
        }
        expect(p.counts().rmc == 3UZ && p.counts().gga == 3UZ && p.counts().rejected == 0UZ) << "a receiver without a time sends well-formed sentences";
        expect(!p.pendingFix().hasTime && !p.pendingFix().valid);
    });

    cases("nmea.other-text-is-ignored", [] {
        NmeaParser p;
        expect(!p.parseLine("", 1).has_value());
        expect(!p.parseLine("not a sentence", 1).has_value());
        expect(!p.parseLine(sentence("GPGSV,3,1,11,03,03,111,00,04,15,270,00,06,01,010,00,13,06,292,00"), 1).has_value());
        expect(!p.parseLine(sentence("PUBX,00,081350.00,4717.113210,N,00833.915187,E,546.589,G3,2.1,2.0,0.007,77.52,0.007,,0.92,1.19,0.77,9,0,0"), 1).has_value());
        expect(p.counts().other == 2UZ && p.counts().rejected == 0UZ) << "sentences of other kinds are counted and left";
    });

    cases("nmea.checked-counts-checked-sentences-of-the-read-kinds", [] {
        NmeaParser p;
        std::ignore = p.parseLine(sentence("GPRMC,120000.00,A,5001.1900,N,00840.6570,E,0.0,0.0,110326,,,A"), 1);
        std::ignore = p.parseLine(sentence("GPGSA,A,3,04,05,09,12,,,,,,,,,1.8,1.0,1.5"), 1);
        expect(eq(p.counts().checked, 2UZ)) << "a checked RMC and a checked GSA";
        std::ignore = p.parseLine("$GPZDA,,,,,,", 2);
        std::ignore = p.parseLine("$GPRMC,120001.00,A,5001.1900,N,00840.6570,E,0.0,0.0,110326,,,A", 2);
        expect(eq(p.counts().zda, 1UZ) && eq(p.counts().rmc, 2UZ)) << "lines without a checksum are read";
        expect(eq(p.counts().checked, 2UZ)) << "and leave the checked count alone";
        std::ignore = p.parseLine("$ stray text on the port", 3);
        std::ignore = p.parseLine(sentence("GPTXT,01,01,02,ANTSTATUS=OK"), 3);
        expect(eq(p.counts().other, 2UZ)) << "stray text and a checked sentence of another kind";
        std::string bad = sentence("GPGGA,120002.00,5001.1900,N,00840.6570,E,1,10,0.8,136.0,M,47.0,M,,");
        bad[bad.size() - 1] = bad[bad.size() - 1] == '0' ? '1' : '0';
        std::ignore = p.parseLine(bad, 4);
        expect(eq(p.counts().rejected, 1UZ)) << "a failed checksum";
        expect(eq(p.counts().checked, 2UZ)) << "none of them counts as checked";
        std::ignore = p.parseLine(sentence("GNZDA,120002.00,11,03,2026,00,00"), 5);
        expect(eq(p.counts().checked, 3UZ)) << "a checked ZDA of any talker counts";
    });

    return cases.finish();
}
