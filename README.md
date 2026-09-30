<!-- Copyright 2026 Jeff Long; SPDX-License-Identifier: MIT -->
# gr4-timesource

GPS and PPS timing sources for GNU Radio 4, loaded as a block library.

Two blocks mark the seconds of a time reference with tags on a stream of
`uint8` samples:

- `timesource::GpsSource` reads a GPS receiver's NMEA 0183 sentences from a
  serial port and tags each UTC second with the receiver's time and fix.
- `timesource::PpsSource` tags each second of a kernel clock (NTP, PTP, TAI)
  or of a pulse-per-second device, with the kernel's clock discipline.

The package installs `libgr4-timesource-blocks.so` into the plugin directory
of GNU Radio 4, where a program that loads plugins finds both blocks by name.

## Moving a graph from the in-tree timing blocks

The blocks take the settings and the tag keys of the timing blocks that were
part of the GNU Radio 4 blocks tree. A graph moves over by its block names:

| In-tree block                      | This package             |
| ---------------------------------- | ------------------------ |
| `gr::blocks::timing::GpsSource`    | `timesource::GpsSource`  |
| `gr::blocks::timing::PpsSource`    | `timesource::PpsSource`  |

These behaviors differ from the in-tree blocks:

- `baud_rate` takes the names `Baud4800` to `Baud230400` and defaults to
  `Baud4800`, the rate NMEA 0183 names. A graph that reads a receiver at
  9600 names `Baud9600`.
- `update_rate_ms` bounds each serial read, 100 ms by default.
- A `GpsSource` tag carries the UTC second whose pulse has just passed.
- A GPS second is locked on RMC status A, a GGA quality above zero or a GSA
  fix of two or three dimensions. The in-tree block needed the GSA fix.
- A `device_path` that does not open refuses the start. The in-tree block
  retried it each second. An empty `device_path` still retries detection.
- Detection passes over a port that gives no checked sentence of a read
  kind within 5 s.
- `device_name` and the record's `device_info` carry the port's USB
  identity after its path.
- A GPS leap second, 23:59:60, gives a tag at 23:59:59 with a
  `trigger_offset` of one second.
- `PpsSource` Auto is the system clock, as in NTP mode. The in-tree Auto
  took PTP where `/dev/ptpN` opened. PTP and HwPps are taken by name alone.
- A PTP tag's `trigger_time` is the UTC second. The new setting
  `ptp_time_scale` names the PTP clock's scale, TAI by default, and
  `tai_utc_offset_s` gives the TAI-UTC offset where the kernel reports 0.
- A PTP second is locked where the PTP clock, moved to UTC, also differs
  from the system clock by less than the new setting `ptp_offset_limit_ns`.
- PTP and HwPps refuse to start where their device does not open. The
  in-tree block fell back to NTP.
- HwPps marks the whole second nearest the pulse, with a signed
  `wakeup_offset_ns`. The in-tree block took the second the pulse fell in.
- HwPps answers a missed pulse after 1.1 s. The in-tree block waited 2 s.
  A fetch error is printed once and gives unlocked seconds at that pace.
- A step of the marked clock repeats no second. After a step back the
  block waits for the second after the last one tagged. After a step
  forward it tags the second of the new reading.
- `synchronised` in the record is false in the kernel's error state as well
  as where `STA_UNSYNC` is set.
- The package is written for Linux, FreeBSD, NetBSD and macOS through the
  libraries named below, with PTP on Linux alone. It has been built on
  Linux. The in-tree `PpsSource` built on Linux alone. The in-tree
  `GpsSource` named Windows and Web Serial support, and this package is
  written for neither.

## GpsSource

The receiver writes its sentences for a second after the pulse that starts
the second. The first sentence stamped with a new second places that
second's tag, and the tag carries the fix the receiver reported for the
second before it. The tag follows the pulse by the receiver's output delay
and the serial latency; for a timing edge of better precision, use the
receiver's PPS line through `PpsSource` in HwPps mode.

The block reads GGA, RMC, ZDA, GSA and VTG sentences of any talker (GP, GN,
GL, GA, BD). It opens the port read-only and writes nothing to the receiver.

| Setting            | Type     | Default     | Meaning                        |
| ------------------ | -------- | ----------- | ------------------------------ |
| `device_path`      | string   | empty       | serial port; empty detects one |
| `device_name`      | string   | set at open | the port in use                |
| `trigger_name`     | string   | `GPS_PPS`   | tag trigger name               |
| `context`          | string   | empty       | `context` tag value            |
| `emit_mode`        | enum     | `ppsOnly`   | `ppsOnly` or `clock`           |
| `sample_rate`      | float    | 1           | samples per second in `clock`  |
| `emit_meta_info`   | bool     | true        | attach the fix record          |
| `emit_device_info` | bool     | true        | add the port to the record     |
| `baud_rate`        | enum     | `Baud4800`  | serial rate                    |
| `update_rate_ms`   | uint32   | 100         | longest wait of one read, ms   |

With `device_path` empty the block lists the serial ports and opens the best
match: a receiver's own USB identity (u-blox 6 to M10, SiRF, Sierra
Wireless), then a USB product string naming GPS, GNSS or u-blox, then a
common USB-to-serial bridge (PL2303, CP210x, FTDI FT232R and FT-X, CH340).
It retries each second until one opens. A detected port that gives no
checked sentence of the five read kinds within 5 s is closed, and the next
ranked port is opened; after the last, the best-ranked one again. A line
without a checksum, stray text and sentences of other kinds count for
nothing in that test. A path given in `device_path` that does not open
refuses the start. A symbolic link such as
`/dev/serial/by-id/...` is accepted.

A second is locked where the receiver stated its date and time and a valid
fix. A locked tag's `trigger_time` is that UTC second. An unlocked tag's
`trigger_time` is the host clock at the arrival of the second's first
sentence, and its name ends in `(unlocked)`, as in `GPS_PPS(unlocked)`.
A leap second, 23:59:60, gives a locked tag at 23:59:59 with a
`trigger_offset` of one second; the next tag is 00:00:00.

## PpsSource

| Setting               | Type   | Default   | Meaning                        |
| --------------------- | ------ | --------- | ------------------------------ |
| `clock_mode`          | enum   | `Auto`    | clock marked: `NTP`, `PTP`,    |
|                       |        |           | `TAI`, `HwPps` or `Auto`       |
| `ptp_device_index`    | uint8  | 0         | N of `/dev/ptpN`               |
| `pps_device_index`    | uint8  | 0         | N of `/dev/ppsN`               |
| `ptp_time_scale`      | enum   | `TAI`     | PTP clock scale: `TAI`, `UTC`  |
| `tai_utc_offset_s`    | int32  | 37        | fallback TAI-UTC offset, s     |
| `ptp_offset_limit_ns` | uint64 | 1000000   | bound on a locked PTP offset   |
| `trigger_name`        | string | `PPS`     | tag trigger name               |
| `context`             | string | empty     | `context` tag value            |
| `emit_mode`           | enum   | `ppsOnly` | `ppsOnly` or `clock`           |
| `sample_rate`         | float  | 1         | samples per second in `clock`  |
| `emit_meta_info`      | bool   | true      | attach the kernel discipline   |

NTP marks the seconds of the system clock, and TAI those of the kernel's TAI
clock, moved to UTC by the kernel's TAI offset. PTP marks the seconds of a
PTP hardware clock on the scale `ptp_time_scale` names. PTP defines TAI,
the default. The block moves a TAI second to UTC by the TAI offset
`ntp_adjtime()` reports, or by `tai_utc_offset_s` where the kernel reports
0. HwPps takes the assert edge of a PPS device and marks the whole second
nearest to it. Auto is the system clock, as in NTP mode, and opens no
device. PTP and HwPps are taken by name alone. A mode the platform or the
device cannot give refuses the start and names the reason.

A second is locked where `ntp_adjtime()` reports the system clock
synchronized. In HwPps mode the pulse must also arrive. After 1.1 s without
one, the block tags an unlocked second at host time. In PTP mode the two
clocks, on one scale, must also differ by less than `ptp_offset_limit_ns`.
The block reads both clocks just after each mark and moves the PTP reading
to UTC before it compares them. A PTP clock that no daemon steers drifts
from the system clock, and its seconds read unlocked once the drift passes
the limit.

The seconds of NTP, TAI and PTP modes rise by at least one second from tag
to tag. A clock stepped back gives no tag until it passes the second after
the last one tagged. A clock stepped forward gives the second of its new
reading. HwPps marks each pulse on the system clock as it reads at the
pulse, and its seconds follow a step of that clock.

HwPps reads `/dev/ppsN`: set `clock_mode` to `HwPps` and
`pps_device_index` to N. On Linux, `/sys/class/pps/ppsN/name` names the
source behind each device. The kernel usually creates the device node for
root alone, so an ordinary user needs a udev rule that gives a group access
to it, or runs the program as root:

```
KERNEL=="pps[0-9]*", GROUP="dialout", MODE="0660"
```

The block opens the node for reading and writing, and for reading alone
where writing is refused. Where the device has assert capture off, the
block turns it on, which Linux allows only with `CAP_SYS_TIME`.

The tag name is `trigger_name`, an underscore and the mode, as in `PPS_NTP`.
An unlocked second's name ends in ` (unlocked)`. A fetch error from the PPS
device is printed once; the block goes on waiting for pulses.

## Tags

Both blocks place one tag on the first sample of each second:

| Key                 | Type    | Content                                  |
| ------------------- | ------- | ---------------------------------------- |
| `trigger_name`      | string  | the name above                           |
| `trigger_time`      | uint64  | the UTC second, ns since the Unix epoch  |
| `trigger_offset`    | float   | 0, or 1 s at a GPS leap second           |
| `trigger_meta_info` | map     | the record below, when enabled           |
| `context`           | string  | the `context` setting, when set          |

`trigger_time` is the UTC second in every mode of both blocks. A PTP clock
on TAI is moved to UTC as the PpsSource section states.

`GpsSource` record: `geolocation` (a GeoJSON point: `type` "Point" and
`coordinates` longitude, latitude and altitude in meters), `local_time`
(ns), `satellites`, `hdop`, `fix_type` (`none`, `fix2D`, `fix3D`),
`speed_kmh`, `heading_deg`, and `device_info` when enabled.

`PpsSource` record: `clock_mode`, `wakeup_offset_ns`, `sequence`,
`synchronised`, `kernel_offset_ns`, `est_error_ns`, `max_error_ns`,
`freq_ppb`, `jitter_ns`, `tai_utc_offset_s` and `leap_status` (`OK`,
`insert_leap`, `delete_leap`, `leap_in_progress`, `leap_occurred`,
`unsynchronised`). The keys keep the spelling graphs already read.

In `ppsOnly` mode a block writes one zero sample per second. In `clock` mode
it writes `sample_rate` zero samples per second, rounded to a whole number,
with the tag on the first.

## Platform libraries

| Library or interface  | Serves                     | Platforms              |
| --------------------- | -------------------------- | ---------------------- |
| libserialport         | open, configure, read and  | Linux, the BSDs,       |
|                       | list serial ports; USB     | macOS, Windows         |
|                       | vendor, product, strings   |                        |
| RFC 2783 `timepps.h`  | HwPps                      | Linux (pps-tools),     |
|                       |                            | FreeBSD, NetBSD        |
| `ntp_adjtime()`       | the kernel discipline      | Linux, FreeBSD,        |
|                       |                            | NetBSD, macOS          |
| `clock_gettime()` on  | PTP                        | Linux                  |
| a PTP clock id        |                            |                        |

The package builds where `sys/timex.h` declares `ntp_adjtime()`. OpenBSD
lacks that header. The block opens a serial port through libserialport
alone. On Linux, libserialport looks a path up among the ports sysfs lists,
and a path absent there, such as a pseudo-terminal, refuses the start with
libserialport's message. On another platform the open can refuse it.

The configure finds libserialport with pkg-config. Without `sys/timepps.h`
it prints a status line and builds HwPps as unavailable. Every supported
receiver is a serial-class USB device, and libserialport reports its USB
identity, so the package needs no libusb.

## Building

The package needs GNU Radio 4 with its block-library macros
(`GnuRadioBlockLib`), libserialport and pkg-config, and a C++23 compiler.

```
cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH=<gnuradio4 prefix>
cmake --build build
cmake --install build --prefix <gnuradio4 prefix>
```

Without `CMAKE_BUILD_TYPE` the package builds Release. Three options choose
what else is built:

| Option                                | Default   | Builds              |
| ------------------------------------- | --------- | ------------------- |
| `GR4TIMESOURCE_ENABLE_TESTING`        | top level | the offline cases   |
| `GR4TIMESOURCE_ENABLE_HARDWARE_TESTS` | OFF       | the receiver case   |
| `GR4TIMESOURCE_ENABLE_EXAMPLES`       | top level | `gps_status`        |

"top level" is ON where the package is the top-level CMake project.

## Tests

The cases need Boost.UT, one header, `boost/ut.hpp`. The configure looks
for it under `include/` of each `CMAKE_PREFIX_PATH` entry and in the
system's include directories. Add the prefix Boost.UT is installed under
to `CMAKE_PREFIX_PATH`, or set the cache variable
`GR4TIMESOURCE_UT_INCLUDE_DIR` to the directory holding `boost/ut.hpp`.
Without either, the configure prints a status line and builds no case.

```
cmake -S . -B build -G Ninja \
    -DCMAKE_PREFIX_PATH="<gnuradio4 prefix>;<Boost.UT prefix>"
cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH=<gnuradio4 prefix> \
    -DGR4TIMESOURCE_UT_INCLUDE_DIR=<Boost.UT prefix>/include
```

Each case is a ctest entry of its own. The offline cases carry the label
`offline` and open no device. The GPS block's cases link
`test/support/FakeSerialPort.cpp`, which defines the libserialport
functions the block calls over a port the case writes sentences to.

```
ctest --test-dir build -L offline
```

`GR4TIMESOURCE_ENABLE_HARDWARE_TESTS=ON` adds a case labeled `hardware`
that reads a real receiver for up to thirty seconds. It opens the port
read-only. Name its port, and its rate in `GR4TIMESOURCE_GPS_BAUD` where it
is not 4800:

```
GR4TIMESOURCE_GPS_DEVICE=/dev/ttyUSB0 ctest --test-dir build -L hardware
```

## Examples

`examples/` holds two ways to watch a GPS receiver: `gps_status`, a program
built with the package, and `gps_status.grc`, a graph file for `rungraph`.
Each prints the receiver's time, fix, satellites, HDOP and position once per
second. `examples/README.md` gives the arguments and the printed fields.

```
build/gps_status --seconds 20
rungraph --graph examples/gps_status.grc --seconds 20 \
    --plugin-dir build/lib/gnuradio-4/plugins
```

Both read at 4800 baud, the default of `GpsSource`; `--baud` and `--set
gps.baud_rate=Baud9600` change the rate.

## License

The package is under the MIT license, the author's choice for it. Every file
carries its own SPDX-License-Identifier header; `LICENSE` and
`LICENSES/MIT.txt` hold the text.
