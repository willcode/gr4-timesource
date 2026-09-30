<!-- Copyright 2026 Jeff Long; SPDX-License-Identifier: MIT -->
# timesource — the GPS and PPS timing sources

**purpose**
`timesource::GpsSource` turns a GPS receiver's NMEA 0183 output into one tagged sample per
UTC second. `timesource::PpsSource` does the same for the seconds of a kernel clock or the
assert edge of a PPS device. Both write `uint8` zeros; the tags carry the information. The
settings and tag keys are those of the timing blocks the GNU Radio 4 blocks tree carried, so a
graph moves over by its block names.

**algorithm**
`NmeaParser` keeps one open record per UTC second. GGA, RMC and ZDA carry a time and open
seconds; GSA and VTG carry none and fill the open record. The first stamped sentence of a new
second closes the record and answers a `SecondEdge`: the new second's UTC time and the closed
record. The date comes from RMC (two-digit year from 2000) or ZDA (four digits) and is kept; a
GGA whose time of day runs back by more than twelve hours moves the kept date on a day. A
sentence starts at the last `$` of its line. A sentence without `*` is taken unchecked; one
with `*` needs exactly two hexadecimal digits that match. An empty field changes nothing. A
stamped sentence with an empty time, as a receiver sends before it knows the time, opens no
second.

The GPS io thread reads the port in waits of `update_rate_ms` that end at the first byte, splits
lines, and stamps every line of one read with the host clock at the read's return. Each edge
goes to a `TickQueue` and moves the graph's progress counter. `work()` on the scheduler thread
applies staged settings and hands the queue to `TickWriter`, which writes one sample per second
(ppsOnly) or `round(sample_rate)` samples (clock), the tag on the first, capped by the room on
the output edge and finished by later calls. A second is locked where its time is known and the
closed record is valid (RMC status A, GGA quality above zero, or a GSA fix of two or three
dimensions). A locked tag's `trigger_time` is the new second; an unlocked one's is the host
clock at the closing line, with `(unlocked)` appended to the name without a space. Second 60 is
accepted at 23:59 alone; its edge carries 23:59:59 and `leapSecond`, and its locked tag a
`trigger_offset` of 1 s. With the device path empty, a port that gives no checked sentence of
the five read kinds within 5 s of its open is passed over for the next ranked one, and the list
of passed-over ports empties once every ranked port is on it. `SentenceCounts::checked` is that
test's count: a line without `*`, stray text or a sentence of another kind leaves it alone.

`ClockSource` waits for the next whole second of its clock in relative sleeps of at most 100 ms
on the monotonic clock, testing the stop request between them and reading the marked clock after
each. The mark is the clock's next whole second and never earlier than one second after the last
second answered. A reading past the mark answers the whole second of that reading. A clock
stepped back keeps the mark, so no second is answered twice and the seconds answered never run
backward; a step back of n seconds leaves about n seconds untagged. HwPps fetches assert events
through RFC 2783 in 100 ms waits, takes a new `assert_sequence` as the pulse, marks the nearest
whole second and reports the edge's offset from it, negative for an early edge. After 1.1 s on
the monotonic clock without a pulse it answers a missed pulse at host time, which the block tags
unlocked. A fetch error other than `ETIMEDOUT` or `EINTR` sleeps out the rest of its 100 ms
slice. The first error of a run is kept for `takeFault()`, and the block prints it once. The
discipline is `ntp_adjtime()` with no modes, converted to nanoseconds and parts per billion. In
PTP mode the io thread also reads the PTP clock between two system clock readings. On the
scheduler thread `makeTag()` takes the TAI-UTC offset (`taiUtcOffsetS()`: the kernel's where
positive, else `tai_utc_offset_s`), moves the PTP second to UTC by it where `ptp_time_scale` is
TAI, and `ptpLocked()` decides the lock: the moved PTP clock differs from the system clock by
less than `ptp_offset_limit_ns`. Every clock read, sleep, PPS fetch and discipline read goes
through `ClockCalls`; `ClockSource::through()` builds a source on given calls with no device,
and `PpsSource::_openClock` is the opener `start()` uses. The cases answer both.

**io**
In: a serial port by path, or the best-ranked port libserialport lists; a kernel clock or
`/dev/ptpN` or `/dev/ppsN`. Out: `out`, `uint8`, one tag per second. `device_name` shows the
port in use.

**guarantees**
The GPS port is opened read-only and nothing is written to it. A `device_path` that does not
open refuses the start with the reason; an empty one retries detection each second. A port that
fails while running is reopened. A `PpsSource` mode the platform or device cannot give refuses
the start and names why. Auto is the system clock and opens no device. A stop takes effect
within one read wait or 100 ms of clock wait.

**invariants**
Each block holds its `IoThread` as its last member, so the io thread ends before the port, the
parser, the clock and the queue it uses. The io thread touches only the port or clock, the
parser and the queue; the scheduler thread writes the output and owns the settings. The queue
holds sixteen seconds and drops the oldest when full.

**domain**
A receiver writes a second's sentences after that second's pulse, beginning some tens to
hundreds of milliseconds after it depending on the receiver and the rate, so a GPS tag lags its
pulse by that delay plus the USB-serial latency. The first record after a start or a reopen can
be partial: it holds only the sentences after the first stamped one seen. `trigger_time` is the
UTC second in every mode. The kernel's TAI clock runs ahead of UTC by the kernel's TAI offset,
and reads UTC where that offset is 0. PTP defines TAI as its scale; a PTP clock steered to the
system clock without that offset runs on UTC, and `ptp_time_scale` names which. The kernel's TAI
offset stays 0 until a daemon sets it. `tai_utc_offset_s` is the fallback, 37 s since 2017. A
PPS tag is locked where `ntp_adjtime()` reports the system clock synchronized; HwPps also needs
the pulse, and PTP the two clocks on one scale within `ptp_offset_limit_ns`. The kernel's
discipline describes the system clock alone, and a PTP clock no daemon steers runs free of it.
HwPps seconds are the system clock's at the pulse and follow its steps. NMEA 0183 names 4800
bit/s, and `baud_rate` defaults to `Baud4800`, the zero value of `BaudRate`. A USB serial bridge
runs at the rate the host sets; the receiver behind it decides which rate delivers sentences.

**rejected**
A serial layer of our own over sysfs, termios and the registry: libserialport covers opening,
configuring, reading, listing and the USB identity on every platform. libusb: every supported
receiver is a serial-class device. Publishing samples on the io thread: a full output edge would
hold the io thread while the receiver's bytes wait, and the settings would be applied on two
threads. Linux `adjtimex`: `ntp_adjtime` is the same call where Linux has it and exists on
FreeBSD, NetBSD and macOS; OpenBSD has no `sys/timex.h`. Auto taking PTP where `/dev/ptpN`
opens: a network card's PTP clock opens on hosts where nothing steers it. Auto taking HwPps
where `/dev/ppsN` opens: a PPS source without a signal gives unlocked host-time seconds in
place of the NTP ones, and Auto would change the capture mode of a device another daemon may
own. A lock test that accepts either scale: a TAI clock reads unlocked while the kernel's
offset is 0, and a locked tag cannot say which scale matched. `clock_nanosleep` to
an absolute mark: a step of the clock holds the wait, and the stop with it. A termios
path beside libserialport for the ports it does not list: the cases reach the GPS block through
`test/support/FakeSerialPort.cpp`, which defines the libserialport functions `SerialPort` calls,
in the test binary alone.

**fails**
On Linux, libserialport refuses a pseudo-terminal: it looks the port up in sysfs. On another
platform the lookup can accept the path and the open refuse it.
`SerialPort::open` refuses every path libserialport refuses and carries its message. A read that
returns early with no bytes is taken as a hang-up. Enumerations used as settings must stay small:
the reflection that names them covers small values only, so `BaudRate` counts from zero and
`bitsPerSecond()` gives the rate.
