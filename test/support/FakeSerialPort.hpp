/* -*- c++ -*- */
/*
 * Copyright 2026 Jeff Long
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <timesource/NmeaParser.hpp>

#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>

namespace timesource::test {

/*| role: the settings a fake port was given through the libserialport calls, as numbers.
    contract: mode is the sp_mode of the last sp_open() and zero before one. A field no call set
        holds -1.
*/
struct FakePortSettings {
    bool open        = false;
    int  mode        = 0;
    int  baudrate    = -1;
    int  bits        = -1;
    int  parity      = -1;
    int  stopbits    = -1;
    int  flowcontrol = -1;
};

/*| role: the USB identity a fake port reports.
*/
struct FakeUsbId {
    std::uint16_t vendorId  = 0;
    std::uint16_t productId = 0;
    std::string   product;
};

/*| role: a serial port answered by FakeSerialPort.cpp, which defines the libserialport
        functions SerialPort calls. A binary that links it reads the bytes a case writes here and
        reaches no device.
    contract: create() makes an empty regular file under the temporary directory and registers
        its canonical path; sp_get_port_by_name() answers the registered paths alone.
        sp_list_ports() lists every registered port. A port created with a USB identity reports
        the USB transport and that identity; any other reports a native port. The destructor
        unregisters the path and removes the file.
*/
class FakePort {
public:
    FakePort(const FakePort&)            = delete;
    FakePort& operator=(const FakePort&) = delete;
    FakePort(FakePort&& other) noexcept;
    FakePort& operator=(FakePort&&) = delete;
    ~FakePort();

    [[nodiscard]] static std::optional<FakePort> create(std::optional<FakeUsbId> usb = std::nullopt);

    [[nodiscard]] const std::string& path() const noexcept { return _path; }

    // Queue bytes for the reader and wake a wait on the port.
    void write(std::string_view bytes) const;

    // Queue one sentence from its body, with its checksum and a carriage return and line feed.
    void sentence(std::string_view body) const { write(std::format("${}*{:02X}\r\n", body, nmea::checksum(body))); }

    [[nodiscard]] FakePortSettings settings() const;

private:
    FakePort() = default;

    std::string _path;
};

} // namespace timesource::test
