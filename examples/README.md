<!-- Copyright 2026 Jeff Long; SPDX-License-Identifier: MIT -->
# Examples

Two examples read a GPS receiver with `timesource::GpsSource` and print its
status once per second. Both open the serial port for reading only and write
nothing to the receiver.

## The baud rate

Both examples read at 4800 baud, the default of `GpsSource` (`Baud4800`)
and the rate NMEA 0183 names. A USB serial bridge runs at the rate the host
sets, and the receiver behind it decides which rate delivers sentences. A
receiver read at a rate it does not use delivers no sentence, and nothing
is printed.

## gps_status, a program

The program builds the graph in code: `GpsSource` into a small sink that
prints one line for each tag. It is built with the package when the package
is the top-level project; `-DGR4TIMESOURCE_ENABLE_EXAMPLES=OFF` leaves it
out. It is not installed.

```
cmake --build build --target gps_status
build/gps_status [--device <path>] [--baud <rate>] [--seconds <s>]
```

| Argument          | Default | Meaning                                  |
| ----------------- | ------- | ---------------------------------------- |
| `--device <path>` | detect  | serial port of the receiver              |
| `--baud <rate>`   | 4800    | 4800, 9600, 19200, 38400, 57600, 115200  |
|                   |         | or 230400                                |
| `--seconds <s>`   | none    | stop after s seconds                     |
| `--help`          |         | print the usage                          |

Without `--device` the block detects the receiver by its USB identity, as
the package README describes. Without `--seconds` the program runs until
Ctrl-C. SIGINT or SIGTERM stops the graph; the program then prints the
number of lines to standard error and exits 0. It exits 1 where the graph
stops on an error, such as a `--device` path that does not open, and 2 for
an argument it refuses.

Each second gives one line. The line below illustrates the form, with
placeholders for the two coordinates, wrapped to fit the page:

```
2026-09-29 21:08:48 UTC locked fix3D sats 6 hdop 1.50 lat <lat> lon <lon>
alt 136.0 m device /dev/ttyUSB0 [VID:067b PID:2303] Prolific Technology
Inc. - USB-Serial Controller
```

| Field      | Meaning                                                   |
| ---------- | --------------------------------------------------------- |
| time       | the UTC second the receiver reported; while unlocked, the |
|            | host clock when that second's first sentence arrived      |
| `locked`   | the receiver reported its date, time and a valid fix;     |
|            | `unlocked` otherwise                                      |
| fix type   | `none`, `fix2D` or `fix3D`                                |
| `sats`     | satellites in use                                         |
| `hdop`     | horizontal dilution of precision                          |
| `lat`      | latitude in degrees, north positive                       |
| `lon`      | longitude in degrees, east positive                       |
| `alt`      | altitude in meters above mean sea level                   |
| `device`   | the serial port and its USB identity                      |

The fix on a line is the one the receiver reported for the second before
the line's time. The first line after a start can hold a partial record,
such as a position with no satellite count and no altitude.

## gps_status.grc, a graph file

The graph file runs under `rungraph` of a GNU Radio 4 installation. It
connects `GpsSource` through `gr::blocks::basic::Convert<uint8, float32>` to
`gr::blocks::testing::TagSink<float32>`, which prints each tag it receives
when `verbose_console` is set. The installed console sinks take `float32`
samples, so the `uint8` output of `GpsSource` passes through `Convert`.

```
rungraph --graph examples/gps_status.grc --seconds 20 \
    --plugin-dir build/lib/gnuradio-4/plugins
```

`--plugin-dir` names the build tree's block library; a library installed
into the installation's own plugin directory needs no option. Without
`--seconds` the run lasts until Ctrl-C. The file detects the receiver and
reads at 4800 baud, the block's default; `--set` names another port or
rate:

```
rungraph --graph examples/gps_status.grc --seconds 20 \
    --plugin-dir build/lib/gnuradio-4/plugins \
    --set gps.device_path=/dev/ttyUSB0 --set gps.baud_rate=Baud9600
```

`TagSink` prints the whole tag map on one line: `trigger_name`,
`trigger_time` in nanoseconds since the Unix epoch, `trigger_offset`, and
`trigger_meta_info` with the fix record. The package README lists those
keys under "Tags".
