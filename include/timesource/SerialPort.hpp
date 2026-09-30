/* -*- c++ -*- */
/*
 * Copyright 2026 Jeff Long
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <libserialport.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace timesource {

/*| role: the serial rates a GPS receiver is read at, named Baud<bits per second>.
    frame: the enumerators count from zero and bitsPerSecond() gives the rate. Baud4800, the
        rate NMEA 0183 names, is the zero value: BaudRate{} is Baud4800.
    why: a setting of enumeration type is reflected by its names, and the reflection covers
        small values only. Enumerators equal to their rates would give the setting no names.
    verified-by: serial.rates-by-name
*/
enum class BaudRate : std::uint8_t { Baud4800, Baud9600, Baud19200, Baud38400, Baud57600, Baud115200, Baud230400 };

[[nodiscard]] constexpr std::uint32_t bitsPerSecond(BaudRate rate) noexcept {
    switch (rate) {
    case BaudRate::Baud4800: return 4800;
    case BaudRate::Baud9600: return 9600;
    case BaudRate::Baud19200: return 19200;
    case BaudRate::Baud38400: return 38400;
    case BaudRate::Baud57600: return 57600;
    case BaudRate::Baud115200: return 115200;
    case BaudRate::Baud230400: return 230400;
    }
    return 4800;
}

/*| role: a USB vendor and product pair that auto-detection takes for a GPS receiver.
    contract: bridge marks a USB-to-serial converter chip. Many receivers use one, and so do
        devices that are not receivers, so a bridge ranks below a receiver's own identity.
*/
struct UsbId {
    std::uint16_t    vendorId  = 0;
    std::uint16_t    productId = 0;
    std::string_view description;
    bool             bridge = false;
};

inline constexpr std::array kKnownReceivers{
    UsbId{0x1546, 0x01a6, "u-blox 6", false},
    UsbId{0x1546, 0x01a7, "u-blox 7", false},
    UsbId{0x1546, 0x01a8, "u-blox 8", false},
    UsbId{0x1546, 0x01a9, "u-blox M8", false},
    UsbId{0x1546, 0x0502, "u-blox M9/M10", false},
    UsbId{0x0681, 0x0002, "SiRF GPS", false},
    UsbId{0x1199, 0x0120, "Sierra Wireless GPS", false},
    UsbId{0x067b, 0x2303, "Prolific PL2303 (common GPS bridge)", true},
    UsbId{0x10c4, 0xea60, "CP210x (common GPS bridge)", true},
    UsbId{0x0403, 0x6001, "FTDI FT232R (common GPS bridge)", true},
    UsbId{0x0403, 0x6015, "FTDI FT-X (common GPS bridge)", true},
    UsbId{0x1a86, 0x7523, "CH340 (common GPS bridge)", true},
};

[[nodiscard]] constexpr std::optional<UsbId> knownReceiver(std::uint16_t vendorId, std::uint16_t productId) noexcept {
    for (const UsbId& id : kKnownReceivers) {
        if (id.vendorId == vendorId && id.productId == productId) {
            return id;
        }
    }
    return std::nullopt;
}

/*| contract: whether a USB product string names a satellite receiver: it contains GPS, GNSS
        or u-blox in any letter case.
*/
[[nodiscard]] inline bool namesReceiver(std::string_view product) {
    std::string lower(product);
    std::ranges::transform(lower, lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower.contains("gps") || lower.contains("gnss") || lower.contains("u-blox");
}

/*| role: a serial port as the system lists it.
    contract: the USB fields are zero and empty for a port on another transport.
*/
struct PortInfo {
    std::string   path{};
    std::string   description{};
    std::string   manufacturer{};
    std::string   product{};
    std::uint16_t vendorId  = 0;
    std::uint16_t productId = 0;
    bool          usb       = false;
};

/*| contract: the rank of a port as a GPS receiver, lower first: 0 for a receiver's own USB
        identity, 1 for a product string that names a receiver, 2 for a known USB-to-serial
        bridge, and nothing for any other port.
*/
[[nodiscard]] inline std::optional<int> receiverRank(const PortInfo& port) {
    if (!port.usb) {
        return std::nullopt;
    }
    const auto known = knownReceiver(port.vendorId, port.productId);
    if (known && !known->bridge) {
        return 0;
    }
    if (namesReceiver(port.product)) {
        return 1;
    }
    if (known) {
        return 2;
    }
    return std::nullopt;
}

/*| contract: the ports that rank as receivers, best first and by path within a rank.
    verified-by: serial.ranks-a-receiver-above-a-bridge
*/
[[nodiscard]] inline std::vector<PortInfo> rankReceivers(std::vector<PortInfo> ports) {
    std::erase_if(ports, [](const PortInfo& p) { return !receiverRank(p).has_value(); });
    std::ranges::stable_sort(ports, [](const PortInfo& a, const PortInfo& b) {
        const int ra = *receiverRank(a);
        const int rb = *receiverRank(b);
        return ra != rb ? ra < rb : a.path < b.path;
    });
    return ports;
}

/*| contract: one line naming a port: its path, and for a USB port its identity, maker and
        product, or the known name of its identity where the port states no product.
*/
[[nodiscard]] inline std::string describe(const PortInfo& port) {
    if (!port.usb) {
        return port.path;
    }
    std::string name = port.product;
    if (name.empty()) {
        if (const auto known = knownReceiver(port.vendorId, port.productId)) {
            name = std::string(known->description);
        }
    }
    return std::format("{} [VID:{:04x} PID:{:04x}] {}{}{}", port.path, port.vendorId, port.productId, port.manufacturer, port.manufacturer.empty() || name.empty() ? "" : " - ", name);
}

namespace detail {

[[nodiscard]] inline std::string lastSerialError() {
    char*       text = sp_last_error_message();
    std::string message(text != nullptr ? text : "unknown error");
    sp_free_error_message(text);
    while (!message.empty() && (message.back() == '\n' || message.back() == '\r')) {
        message.pop_back();
    }
    return message;
}

[[nodiscard]] inline std::string text(const char* value) { return value != nullptr ? std::string(value) : std::string{}; }

[[nodiscard]] inline PortInfo portInfo(sp_port* port) {
    PortInfo info{.path = text(sp_get_port_name(port)), .description = text(sp_get_port_description(port))};
    if (sp_get_port_transport(port) == SP_TRANSPORT_USB) {
        int vid = 0;
        int pid = 0;
        if (sp_get_port_usb_vid_pid(port, &vid, &pid) == SP_OK) {
            info.usb       = true;
            info.vendorId  = static_cast<std::uint16_t>(vid);
            info.productId = static_cast<std::uint16_t>(pid);
        }
        info.manufacturer = text(sp_get_port_usb_manufacturer(port));
        info.product      = text(sp_get_port_usb_product(port));
    }
    return info;
}

} // namespace detail

/*| contract: every serial port the system lists, as libserialport enumerates them.
*/
[[nodiscard]] inline std::vector<PortInfo> listPorts() {
    std::vector<PortInfo> ports;
    sp_port**             list = nullptr;
    if (sp_list_ports(&list) != SP_OK) {
        return ports;
    }
    for (sp_port** p = list; *p != nullptr; ++p) {
        ports.push_back(detail::portInfo(*p));
    }
    sp_free_port_list(list);
    return ports;
}

/*| contract: the best-ranked receiver among ports whose path passedOver does not hold, or the
        best-ranked one where passedOver holds them all. An error names every port seen where
        none ranks.
    verified-by: serial.detection-passes-over-a-silent-port
*/
[[nodiscard]] inline std::expected<PortInfo, std::string> pickReceiver(const std::vector<PortInfo>& ports, std::span<const std::string> passedOver = {}) {
    auto ranked = rankReceivers(ports);
    if (!ranked.empty()) {
        const auto fresh = std::ranges::find_if(ranked, [passedOver](const PortInfo& p) { return !std::ranges::contains(passedOver, p.path); });
        return fresh != ranked.end() ? *fresh : ranked.front();
    }
    if (ports.empty()) {
        return std::unexpected(std::string("no serial port found; connect a GPS receiver or name its port in device_path"));
    }
    std::string seen;
    for (const PortInfo& p : ports) {
        seen += std::format("\n  {}", describe(p));
    }
    return std::unexpected(std::format("no known GPS receiver among {} serial port(s); name one in device_path:{}", ports.size(), seen));
}

/*| role: one serial port open for reading, over libserialport.
    contract: open() configures the port for 8 data bits, no parity, one stop bit and no flow
        control at the rate given, and never writes to it. read() waits at most the time given
        for data and then reads what has arrived without blocking. An error from read() means
        the port went away; the owner closes it.
    contract: open() refuses a path libserialport refuses, and the error carries
        libserialport's message.
    frame: a symbolic link such as /dev/serial/by-id/<name> is resolved first, since
        libserialport identifies a port by its device node.
    trap: on Linux, libserialport looks a port up among the ports sysfs lists. A
        pseudo-terminal is absent there, and open() refuses it as no serial port. On another
        platform the lookup can take the path and the open refuse it.
    verified-by: serial.refuses-a-path-that-does-not-exist
    verified-by: serial.refuses-a-file-that-is-no-serial-port
    verified-by: gps.reads-at-4800-baud-8n1-by-default
*/
class SerialPort {
public:
    SerialPort() = default;
    SerialPort(const SerialPort&)            = delete;
    SerialPort& operator=(const SerialPort&) = delete;
    SerialPort(SerialPort&& other) noexcept { *this = std::move(other); }
    SerialPort& operator=(SerialPort&& other) noexcept {
        if (this != &other) {
            close();
            _port   = std::exchange(other._port, nullptr);
            _events = std::exchange(other._events, nullptr);
            _info   = std::move(other._info);
        }
        return *this;
    }
    ~SerialPort() { close(); }

    [[nodiscard]] static std::expected<SerialPort, std::string> open(std::string_view path, BaudRate rate) {
        if (path.empty()) {
            return std::unexpected(std::string("no device path given"));
        }
        std::string node(path);
#if defined(__unix__) || defined(__APPLE__)
        std::error_code ec;
        if (!std::filesystem::exists(node, ec)) {
            return std::unexpected(std::format("device '{}' does not exist", path));
        }
        if (const auto resolved = std::filesystem::canonical(node, ec); !ec) {
            node = resolved.string();
        }
#endif
        SerialPort port;
        sp_port*   found = nullptr;
        if (sp_get_port_by_name(node.c_str(), &found) != SP_OK) {
            return std::unexpected(std::format("device '{}' is not a serial port: {}", path, detail::lastSerialError()));
        }
        port._port = found;
        port._info = detail::portInfo(found);
        if (sp_open(found, SP_MODE_READ) != SP_OK) {
            return std::unexpected(std::format("cannot open '{}': {}", path, detail::lastSerialError()));
        }
        if (sp_set_baudrate(found, static_cast<int>(bitsPerSecond(rate))) != SP_OK || sp_set_bits(found, 8) != SP_OK || sp_set_parity(found, SP_PARITY_NONE) != SP_OK || sp_set_stopbits(found, 1) != SP_OK || sp_set_flowcontrol(found, SP_FLOWCONTROL_NONE) != SP_OK) {
            return std::unexpected(std::format("cannot configure '{}' for {} baud: {}", path, bitsPerSecond(rate), detail::lastSerialError()));
        }
        if (sp_new_event_set(&port._events) != SP_OK || sp_add_port_events(port._events, found, SP_EVENT_RX_READY) != SP_OK) {
            return std::unexpected(std::format("cannot wait on '{}': {}", path, detail::lastSerialError()));
        }
        return port;
    }

    [[nodiscard]] bool            isOpen() const noexcept { return _port != nullptr; }
    [[nodiscard]] const PortInfo& info() const noexcept { return _info; }

    [[nodiscard]] std::expected<std::size_t, std::string> read(std::span<char> into, std::chrono::milliseconds wait) {
        if (_port == nullptr) {
            return std::unexpected(std::string("the port is not open"));
        }
        const auto began = std::chrono::steady_clock::now();
        if (sp_wait(_events, static_cast<unsigned>(std::max<std::int64_t>(wait.count(), 1))) != SP_OK) {
            return std::unexpected(std::format("waiting on '{}' failed: {}", _info.path, detail::lastSerialError()));
        }
        const int got = sp_nonblocking_read(_port, into.data(), into.size());
        if (got < 0) {
            return std::unexpected(std::format("reading '{}' failed: {}", _info.path, detail::lastSerialError()));
        }
        if (got == 0 && std::chrono::steady_clock::now() - began < wait / 2) {
            return std::unexpected(std::format("'{}' hung up", _info.path));
        }
        return static_cast<std::size_t>(got);
    }

    void close() noexcept {
        if (_events != nullptr) {
            sp_free_event_set(_events);
            _events = nullptr;
        }
        if (_port != nullptr) {
            sp_close(_port);
            sp_free_port(_port);
            _port = nullptr;
        }
    }

private:
    sp_port*      _port   = nullptr;
    sp_event_set* _events = nullptr;
    PortInfo      _info;
};

/*| contract: open the best-ranked GPS receiver the system lists, passing over the paths in
        passedOver as pickReceiver() does.
*/
[[nodiscard]] inline std::expected<SerialPort, std::string> openDetectedReceiver(BaudRate rate, std::span<const std::string> passedOver = {}) {
    const auto picked = pickReceiver(listPorts(), passedOver);
    if (!picked) {
        return std::unexpected(picked.error());
    }
    return SerialPort::open(picked->path, rate);
}

} // namespace timesource
