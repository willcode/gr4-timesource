/* -*- c++ -*- */
/*
 * Copyright 2026 Jeff Long
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <system_error>

namespace timesource {

/*| role: the fix dimension a receiver states in its GSA sentence.
    frame: the values are the GSA fix-type codes, so a cast gives the code a receiver sent.
*/
enum class FixType : std::uint8_t { none = 0, fix2D = 2, fix3D = 3 };

/*| role: the output shape of both timing sources.
    contract: ppsOnly writes one sample per second, and clock writes sample_rate samples per
        second. The tag of a second sits on its first sample in both.
*/
enum class EmitMode : std::uint8_t { ppsOnly, clock };

/*| role: what one receiver epoch reported, gathered from every sentence of that UTC second.
    frame: utcTimestampNs and localTimeNs are nanoseconds since the Unix epoch. utcTimestampNs
        is the time the epoch's sentences state; localTimeNs is the host clock when the
        sentence that closed the epoch arrived. Latitude and longitude are decimal degrees,
        north and east positive; altitude is meters above mean sea level.
    contract: a field keeps its default where no sentence of the epoch carried it. hasTime
        holds where a time and a date were both known, hasPosition where a sentence gave a
        position, and valid where the receiver declared its fix good: RMC status A, a GGA
        quality above zero, or a GSA fix of two or three dimensions.
*/
struct GpsFix {
    std::uint64_t utcTimestampNs = 0;
    std::uint64_t localTimeNs    = 0;
    double        latitude       = 0.0;
    double        longitude      = 0.0;
    float         altitude       = 0.f;
    std::int32_t  satellites     = 0;
    float         hdop           = 0.f;
    FixType       fixType        = FixType::none;
    float         speedKmh       = 0.f;
    float         headingDeg     = 0.f;
    bool          hasPosition    = false;
    bool          hasTime        = false;
    bool          valid          = false;
};

/*| role: the start of a new UTC second, as the first sentence stamped with that second shows it.
    contract: utcNs is the whole second that sentence names, where a date is known (hasTime).
        A receiver writes the sentences of a second after that second's pulse, so utcNs is the
        time of the pulse that has just passed. previous is the completed record of the second
        before it, with localTimeNs set to the arrival of the sentence that opened this one.
    contract: a leap second, 23:59:60, gives utcNs at 23:59:59 of its day and leapSecond set:
        the second lies one second after utcNs.
    verified-by: nmea.a-leap-second-is-23-59-59-plus-one-second
*/
struct SecondEdge {
    std::uint64_t utcNs      = 0;
    bool          hasTime    = false;
    bool          leapSecond = false;
    GpsFix        previous;
};

/*| role: the sentences a parser took and refused, by kind.
    contract: rejected counts lines that started a sentence and failed its checksum, its field
        count or its time field. other counts well-formed sentences of a kind the parser does
        not read. checked counts the sentences of the five read kinds that carried a checksum
        and matched it; a line without a checksum or of another kind leaves it alone.
    verified-by: nmea.checked-counts-checked-sentences-of-the-read-kinds
*/
struct SentenceCounts {
    std::size_t rmc      = 0;
    std::size_t gga      = 0;
    std::size_t gsa      = 0;
    std::size_t vtg      = 0;
    std::size_t zda      = 0;
    std::size_t other    = 0;
    std::size_t rejected = 0;
    std::size_t checked  = 0;

    // The well-formed sentences of the five read kinds.
    [[nodiscard]] std::size_t read() const noexcept { return rmc + gga + gsa + vtg + zda; }
};

namespace nmea {

/*| contract: the NMEA 0183 checksum of body, the exclusive-or of its bytes. body is the text
        between the dollar sign and the asterisk.
*/
[[nodiscard]] constexpr std::uint8_t checksum(std::string_view body) noexcept {
    std::uint8_t sum = 0;
    for (const char c : body) {
        sum = static_cast<std::uint8_t>(sum ^ static_cast<std::uint8_t>(c));
    }
    return sum;
}

/*| contract: the text between the dollar sign and the checksum of line, or nothing where line
        holds no dollar sign or its checksum does not match. A line without an asterisk is
        accepted unchecked. A line with one needs exactly two hexadecimal digits after it.
    frame: the sentence starts at the last dollar sign of the line, so bytes of another protocol
        ahead of it are skipped. A trailing carriage return is dropped first.
*/
[[nodiscard]] inline std::optional<std::string_view> body(std::string_view line) noexcept {
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
        line.remove_suffix(1);
    }
    const std::size_t dollar = line.rfind('$');
    if (dollar == std::string_view::npos) {
        return std::nullopt;
    }
    line.remove_prefix(dollar + 1);
    const std::size_t star = line.find('*');
    if (star == std::string_view::npos) {
        return line;
    }
    if (star + 3 != line.size()) {
        return std::nullopt;
    }
    std::uint8_t stated = 0;
    const char*  first  = line.data() + star + 1;
    const auto [end, ec] = std::from_chars(first, first + 2, stated, 16);
    if (ec != std::errc{} || end != first + 2 || stated != checksum(line.substr(0, star))) {
        return std::nullopt;
    }
    return line.substr(0, star);
}

// Whether the sentence of line, from its last dollar sign, carries a checksum.
[[nodiscard]] inline bool carriesChecksum(std::string_view line) noexcept {
    const std::size_t dollar = line.rfind('$');
    return dollar != std::string_view::npos && line.find('*', dollar) != std::string_view::npos;
}

/*| role: the comma-separated fields of a sentence body, field 0 being the address.
    contract: at most kMaxFields fields are kept; count() is the number the body held.
*/
class Fields {
public:
    static constexpr std::size_t kMaxFields = 24;

    explicit Fields(std::string_view text) noexcept {
        std::size_t start = 0;
        for (;;) {
            const std::size_t comma = text.find(',', start);
            if (_count < kMaxFields) {
                _fields[_count] = text.substr(start, comma == std::string_view::npos ? std::string_view::npos : comma - start);
            }
            ++_count;
            if (comma == std::string_view::npos) {
                break;
            }
            start = comma + 1;
        }
    }

    [[nodiscard]] std::size_t      count() const noexcept { return _count; }
    [[nodiscard]] std::string_view operator[](std::size_t i) const noexcept { return i < _count && i < kMaxFields ? _fields[i] : std::string_view{}; }

private:
    std::array<std::string_view, kMaxFields> _fields{};
    std::size_t                              _count = 0;
};

/*| contract: the number field holds, or nothing where the field is empty or is not a number
        in full.
*/
template <typename T>
[[nodiscard]] std::optional<T> number(std::string_view field) noexcept {
    if (field.empty()) {
        return std::nullopt;
    }
    T value{};
    const auto [end, ec] = std::from_chars(field.data(), field.data() + field.size(), value);
    if (ec != std::errc{} || end != field.data() + field.size()) {
        return std::nullopt;
    }
    return value;
}

/*| contract: a latitude or longitude in decimal degrees from the NMEA form, degrees followed
        by two digits of whole minutes and a decimal fraction of a minute (DDMM.mmmm or
        DDDMM.mmmm), and its hemisphere letter. S and W are negative. Nothing where either
        field is empty or malformed, or the minutes reach 60.
*/
[[nodiscard]] inline std::optional<double> coordinate(std::string_view value, std::string_view hemisphere) noexcept {
    const auto raw = number<double>(value);
    if (!raw || *raw < 0.0 || hemisphere.size() != 1) {
        return std::nullopt;
    }
    const double degrees = static_cast<double>(static_cast<std::int64_t>(*raw / 100.0));
    const double minutes = *raw - degrees * 100.0;
    if (minutes >= 60.0) {
        return std::nullopt;
    }
    const double decimal = degrees + minutes / 60.0;
    switch (hemisphere[0]) {
    case 'N':
    case 'E': return decimal;
    case 'S':
    case 'W': return -decimal;
    default: return std::nullopt;
    }
}

/*| role: a UTC time of day from an NMEA time field.
    frame: second is 0 to 86400, the top value being the leap second 23:59:60; fractionNs is
        the part after the decimal point.
*/
struct TimeOfDay {
    std::int32_t second     = 0;
    std::int64_t fractionNs = 0;
};

inline constexpr std::int32_t kLeapSecond = 86400;

/*| contract: the time of day of an hhmmss or hhmmss.sss field, or nothing where the field is
        shorter than six digits or out of range. Second 60 is accepted at 23:59 alone, as the
        leap second.
*/
[[nodiscard]] inline std::optional<TimeOfDay> timeOfDay(std::string_view field) noexcept {
    if (field.size() < 6) {
        return std::nullopt;
    }
    const auto h = number<std::int32_t>(field.substr(0, 2));
    const auto m = number<std::int32_t>(field.substr(2, 2));
    const auto s = number<std::int32_t>(field.substr(4, 2));
    if (!h || !m || !s || *h > 23 || *m > 59 || *s > 60 || *h < 0 || *m < 0 || *s < 0 || (*s == 60 && (*h != 23 || *m != 59))) {
        return std::nullopt;
    }
    TimeOfDay tod{.second = *h * 3600 + *m * 60 + *s};
    if (field.size() > 6) {
        if (field[6] != '.') {
            return std::nullopt;
        }
        const std::string_view digits = field.substr(7);
        std::int64_t           scale  = 100'000'000;
        for (const char c : digits) {
            if (c < '0' || c > '9') {
                return std::nullopt;
            }
            tod.fractionNs += static_cast<std::int64_t>(c - '0') * scale;
            scale /= 10;
        }
    }
    return tod;
}

/*| contract: a calendar date, or nothing where it is not a valid date.
*/
[[nodiscard]] inline std::optional<std::chrono::sys_days> date(std::int32_t year, std::int32_t month, std::int32_t day) noexcept {
    if (month < 1 || month > 12 || day < 1 || day > 31) {
        return std::nullopt;
    }
    const std::chrono::year_month_day ymd{std::chrono::year{year}, std::chrono::month{static_cast<unsigned>(month)}, std::chrono::day{static_cast<unsigned>(day)}};
    if (!ymd.ok()) {
        return std::nullopt;
    }
    return std::chrono::sys_days{ymd};
}

/*| contract: the date of an RMC ddmmyy field. The two-digit year counts from 2000.
*/
[[nodiscard]] inline std::optional<std::chrono::sys_days> rmcDate(std::string_view field) noexcept {
    if (field.size() != 6) {
        return std::nullopt;
    }
    const auto d = number<std::int32_t>(field.substr(0, 2));
    const auto m = number<std::int32_t>(field.substr(2, 2));
    const auto y = number<std::int32_t>(field.substr(4, 2));
    if (!d || !m || !y) {
        return std::nullopt;
    }
    return date(2000 + *y, *m, *d);
}

/*| contract: nanoseconds since the Unix epoch of a date and a time of day. The leap second
        23:59:60 maps onto 23:59:59 of its day; the caller carries the extra second.
*/
[[nodiscard]] inline std::uint64_t utcNs(std::chrono::sys_days day, TimeOfDay tod, bool withFraction = true) noexcept {
    const auto since = std::chrono::duration_cast<std::chrono::nanoseconds>(day.time_since_epoch()) + std::chrono::seconds{tod.second == kLeapSecond ? kLeapSecond - 1 : tod.second};
    return static_cast<std::uint64_t>(since.count() + (withFraction ? tod.fractionNs : 0));
}

inline constexpr float kKnotsToKmh = 1.852f;

} // namespace nmea

/*| role: turns NMEA 0183 text, one line at a time, into a record per UTC second.
    contract: parseLine() reads the GGA, RMC, ZDA, GSA and VTG sentences of any talker, GP, GN,
        GL, GA, BD and the rest alike, and ignores every other line. It answers a SecondEdge
        when a line stamped with a new UTC second arrives: the time of that second and the
        completed record of the second before it. Every other line answers nothing.
    frame: GGA, RMC and ZDA carry the time and so open seconds. GSA and VTG carry none and
        belong to the second of the last stamped sentence before them. The date comes from RMC
        or ZDA and is kept across seconds. A GGA whose time of day runs back by more than twelve
        hours from the last one moves the kept date on by a day.
    invariant: a line that fails its checksum, has too few fields for its kind or carries a
        malformed time changes nothing, not even the second. A stamped sentence with an empty
        time field, as a receiver sends before it knows the time, opens no second; its other
        fields go to the open record.
    trap: a second is closed by the first stamped line of the next one, so the record of a
        second is complete only then, about one second after that second's pulse.
    verified-by: nmea.rmc-fields
    verified-by: nmea.a-new-second-closes-the-record
*/
class NmeaParser {
public:
    [[nodiscard]] std::optional<SecondEdge> parseLine(std::string_view line, std::uint64_t localTimeNs) {
        const auto text = nmea::body(line);
        if (!text) {
            if (line.find('$') != std::string_view::npos) {
                ++_counts.rejected;
            }
            return std::nullopt;
        }
        const std::size_t readBefore = _counts.read();
        auto              edge       = parseSentence(nmea::Fields{*text}, localTimeNs);
        if (_counts.read() > readBefore && nmea::carriesChecksum(line)) {
            ++_counts.checked;
        }
        return edge;
    }

    // The record of the last second a SecondEdge closed.
    [[nodiscard]] const GpsFix& lastFix() const noexcept { return _lastComplete; }
    // The record of the second still open.
    [[nodiscard]] const GpsFix&         pendingFix() const noexcept { return _pending; }
    [[nodiscard]] const SentenceCounts& counts() const noexcept { return _counts; }

private:
    std::optional<SecondEdge> parseSentence(const nmea::Fields& f, std::uint64_t localTimeNs) {
        const std::string_view address = f[0];
        if (address.starts_with('P')) {
            ++_counts.other; // a proprietary sentence
            return std::nullopt;
        }
        if (address.size() < 5) {
            ++_counts.rejected;
            return std::nullopt;
        }
        const std::string_view kind = address.substr(address.size() - 3);
        if (kind == "RMC") {
            return rmc(f, localTimeNs);
        }
        if (kind == "GGA") {
            return gga(f, localTimeNs);
        }
        if (kind == "ZDA") {
            return zda(f, localTimeNs);
        }
        if (kind == "GSA") {
            gsa(f);
            return std::nullopt;
        }
        if (kind == "VTG") {
            vtg(f);
            return std::nullopt;
        }
        ++_counts.other;
        return std::nullopt;
    }

    /*| contract: the date a stamped sentence belongs to. stated is the date the sentence
            carries, where it carries one.
    */
    std::optional<std::chrono::sys_days> dateFor(nmea::TimeOfDay tod, std::optional<std::chrono::sys_days> stated) {
        if (stated) {
            _date = stated;
        } else if (_date && _lastSecond && *_lastSecond - tod.second > 43200) {
            *_date += std::chrono::days{1};
        }
        return _date;
    }

    /*| contract: open the second of a stamped sentence, answering the edge where it differs from
            the second open before, and stamp the open record with the sentence's time.
    */
    std::optional<SecondEdge> stamp(nmea::TimeOfDay tod, std::optional<std::chrono::sys_days> day, std::uint64_t localTimeNs) {
        std::optional<SecondEdge> edge;
        if (_lastSecond && *_lastSecond != tod.second) {
            _lastComplete             = _pending;
            _lastComplete.localTimeNs = localTimeNs;
            edge                      = SecondEdge{.utcNs = day ? nmea::utcNs(*day, tod, false) : 0U, .hasTime = day.has_value(), .leapSecond = tod.second == nmea::kLeapSecond, .previous = _lastComplete};
            _pending                  = GpsFix{};
        }
        _lastSecond = tod.second;
        if (day) {
            _pending.utcTimestampNs = nmea::utcNs(*day, tod);
            _pending.hasTime        = true;
        }
        return edge;
    }

    void position(std::string_view lat, std::string_view latHemisphere, std::string_view lon, std::string_view lonHemisphere) {
        const auto la = nmea::coordinate(lat, latHemisphere);
        const auto lo = nmea::coordinate(lon, lonHemisphere);
        if (la && lo) {
            _pending.latitude    = *la;
            _pending.longitude   = *lo;
            _pending.hasPosition = true;
        }
    }

    /*| contract: the time of day of a stamped sentence with at least minFields fields. The
            outer optional is empty where the sentence is refused, counted as rejected: too few
            fields, or a time field that is present and malformed. The inner one is empty where
            the time field is empty, as a receiver sends it before it knows the time.
    */
    std::optional<std::optional<nmea::TimeOfDay>> timeField(const nmea::Fields& f, std::size_t minFields) {
        if (f.count() < minFields) {
            ++_counts.rejected;
            return std::nullopt;
        }
        if (f[1].empty()) {
            return std::optional<nmea::TimeOfDay>{};
        }
        const auto tod = nmea::timeOfDay(f[1]);
        if (!tod) {
            ++_counts.rejected;
            return std::nullopt;
        }
        return tod;
    }

    // $--RMC,hhmmss.ss,A,llll.ll,a,yyyyy.yy,a,x.x,x.x,ddmmyy,x.x,a[,a[,a]]
    std::optional<SecondEdge> rmc(const nmea::Fields& f, std::uint64_t localTimeNs) {
        const auto tod = timeField(f, 10);
        if (!tod) {
            return std::nullopt;
        }
        ++_counts.rmc;
        auto edge = *tod ? stamp(**tod, dateFor(**tod, nmea::rmcDate(f[9])), localTimeNs) : std::nullopt;
        if (f[2] == "A") {
            _pending.valid = true;
            position(f[3], f[4], f[5], f[6]);
            if (const auto knots = nmea::number<float>(f[7])) {
                _pending.speedKmh = *knots * nmea::kKnotsToKmh;
            }
            if (const auto course = nmea::number<float>(f[8])) {
                _pending.headingDeg = *course;
            }
        }
        return edge;
    }

    // $--GGA,hhmmss.ss,llll.ll,a,yyyyy.yy,a,q,nn,x.x,x.x,M,x.x,M,x.x,xxxx
    std::optional<SecondEdge> gga(const nmea::Fields& f, std::uint64_t localTimeNs) {
        const auto tod = timeField(f, 10);
        if (!tod) {
            return std::nullopt;
        }
        ++_counts.gga;
        auto edge = *tod ? stamp(**tod, dateFor(**tod, std::nullopt), localTimeNs) : std::nullopt;
        if (const auto used = nmea::number<std::int32_t>(f[7])) {
            _pending.satellites = *used;
        }
        if (const auto quality = nmea::number<std::int32_t>(f[6]); quality && *quality > 0) {
            _pending.valid = true;
            position(f[2], f[3], f[4], f[5]);
            if (const auto hdop = nmea::number<float>(f[8])) {
                _pending.hdop = *hdop;
            }
            if (const auto altitude = nmea::number<float>(f[9])) {
                _pending.altitude = *altitude;
            }
        }
        return edge;
    }

    // $--ZDA,hhmmss.ss,dd,mm,yyyy,zh,zm
    std::optional<SecondEdge> zda(const nmea::Fields& f, std::uint64_t localTimeNs) {
        const auto tod = timeField(f, 5);
        if (!tod || !*tod) {
            if (tod) {
                ++_counts.zda;
            }
            return std::nullopt;
        }
        ++_counts.zda;
        std::optional<std::chrono::sys_days> day;
        const auto                           d = nmea::number<std::int32_t>(f[2]);
        const auto                           m = nmea::number<std::int32_t>(f[3]);
        const auto                           y = nmea::number<std::int32_t>(f[4]);
        if (d && m && y) {
            day = nmea::date(*y, *m, *d);
        }
        return stamp(**tod, dateFor(**tod, day), localTimeNs);
    }

    // $--GSA,a,x,xx,xx,xx,xx,xx,xx,xx,xx,xx,xx,xx,xx,x.x,x.x,x.x[,h]
    void gsa(const nmea::Fields& f) {
        if (f.count() < 18) {
            ++_counts.rejected;
            return;
        }
        ++_counts.gsa;
        const auto mode = nmea::number<std::int32_t>(f[2]);
        if (mode && (*mode == 2 || *mode == 3)) {
            _pending.fixType = *mode == 3 ? FixType::fix3D : FixType::fix2D;
            _pending.valid   = true;
        } else if (mode) {
            _pending.fixType = FixType::none;
        }
        if (const auto hdop = nmea::number<float>(f[16])) {
            _pending.hdop = *hdop;
        }
    }

    // $--VTG,x.x,T,x.x,M,x.x,N,x.x,K[,a]
    void vtg(const nmea::Fields& f) {
        if (f.count() < 9) {
            ++_counts.rejected;
            return;
        }
        ++_counts.vtg;
        if (f.count() > 9 && f[9] == "N") {
            return; // the mode indicator marks the data not valid
        }
        if (const auto course = nmea::number<float>(f[1])) {
            _pending.headingDeg = *course;
        }
        if (const auto kmh = nmea::number<float>(f[7])) {
            _pending.speedKmh = *kmh;
        } else if (const auto knots = nmea::number<float>(f[5])) {
            _pending.speedKmh = *knots * nmea::kKnotsToKmh;
        }
    }

    GpsFix                               _pending;
    GpsFix                               _lastComplete;
    std::optional<std::int32_t>          _lastSecond;
    std::optional<std::chrono::sys_days> _date;
    SentenceCounts                       _counts;
};

} // namespace timesource
