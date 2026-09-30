/* -*- c++ -*- */
/*
 * Copyright 2026 Jeff Long
 * SPDX-License-Identifier: MIT
 */
#include <timesource/SerialPort.hpp>

#include "support/Cases.hpp"

#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <vector>

#include <unistd.h>

namespace {

timesource::PortInfo usbPort(std::string path, std::uint16_t vid, std::uint16_t pid, std::string product = {}) { return timesource::PortInfo{.path = std::move(path), .description = {}, .manufacturer = {}, .product = std::move(product), .vendorId = vid, .productId = pid, .usb = true}; }

} // namespace

int main(int argc, char** argv) {
    using namespace boost::ut;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    timesource::test::Cases cases(argc, argv);

    cases("serial.ranks-a-receiver-above-a-bridge", [] {
        const std::vector<timesource::PortInfo> ports{
            usbPort("/dev/ttyUSB0", 0x0403, 0x6001),                         // an FTDI bridge
            timesource::PortInfo{.path = "/dev/ttyS0"},                      // a built-in port
            usbPort("/dev/ttyACM3", 0x2341, 0x0043, "Arduino Uno"),          // no receiver at all
            usbPort("/dev/ttyACM1", 0x1546, 0x01a8),                         // a u-blox 8
            usbPort("/dev/ttyUSB1", 0x1234, 0x5678, "USB GNSS Receiver"),    // a receiver by its name
            usbPort("/dev/ttyACM0", 0x1546, 0x01a9),                         // a u-blox M8
        };
        const auto ranked = timesource::rankReceivers(ports);
        expect(fatal(eq(ranked.size(), 4UZ))) << "the port without USB and the unknown device drop out";
        expect(ranked[0].path == "/dev/ttyACM0" && ranked[1].path == "/dev/ttyACM1") << "receivers by their own identity first, by path";
        expect(ranked[2].path == "/dev/ttyUSB1") << "then a product string naming a receiver";
        expect(ranked[3].path == "/dev/ttyUSB0") << "then a bare bridge";
        expect(timesource::knownReceiver(0x067b, 0x2303).has_value() && timesource::knownReceiver(0x067b, 0x2303)->bridge) << "the PL2303 is a known bridge";
        expect(eq(timesource::kKnownReceivers.size(), 12UZ)) << "the table holds the twelve identities";
    });

    cases("serial.rates-by-name", [] {
        using timesource::BaudRate;
        expect(eq(timesource::bitsPerSecond(BaudRate::Baud4800), 4800U) && eq(timesource::bitsPerSecond(BaudRate::Baud9600), 9600U) && eq(timesource::bitsPerSecond(BaudRate::Baud230400), 230400U));
        expect(eq(static_cast<int>(BaudRate::Baud230400), 6)) << "the enumerators stay small enough to be reflected by name";
    });

    cases("serial.a-product-string-names-a-receiver", [] {
        expect(timesource::namesReceiver("u-blox GNSS receiver"));
        expect(timesource::namesReceiver("USB-GPS"));
        expect(timesource::namesReceiver("U-BLOX 7"));
        expect(!timesource::namesReceiver("USB-Serial Controller")) << "a bridge's generic name is no receiver";
        expect(!timesource::namesReceiver(""));
    });

    cases("serial.describes-a-usb-port", [] {
        auto port         = usbPort("/dev/ttyUSB0", 0x067b, 0x2303, "USB-Serial Controller");
        port.manufacturer = "Prolific Technology Inc.";
        expect(eq(timesource::describe(port), std::string("/dev/ttyUSB0 [VID:067b PID:2303] Prolific Technology Inc. - USB-Serial Controller")));
        expect(eq(timesource::describe(usbPort("/dev/ttyACM0", 0x1546, 0x01a8)), std::string("/dev/ttyACM0 [VID:1546 PID:01a8] u-blox 8"))) << "the table's name where the port states none";
        expect(eq(timesource::describe(timesource::PortInfo{.path = "/dev/ttyS0"}), std::string("/dev/ttyS0")));
    });

    cases("serial.names-the-ports-it-saw", [] {
        const auto none = timesource::pickReceiver({});
        expect(!none.has_value() && none.error().contains("no serial port")) << "an empty system says so";
        const auto other = timesource::pickReceiver({usbPort("/dev/ttyACM3", 0x2341, 0x0043, "Arduino Uno")});
        expect(!other.has_value() && other.error().contains("/dev/ttyACM3 [VID:2341 PID:0043]")) << "a system without a receiver lists what it has";
        const auto found = timesource::pickReceiver({usbPort("/dev/ttyACM1", 0x1546, 0x01a8)});
        expect(found.has_value() && found->path == "/dev/ttyACM1");
    });

    cases("serial.detection-passes-over-a-silent-port", [] {
        const std::vector<timesource::PortInfo> ports{usbPort("/dev/ttyUSB0", 0x1a86, 0x7523), usbPort("/dev/ttyACM0", 0x1546, 0x01a9)};
        expect(timesource::pickReceiver(ports).value_or(timesource::PortInfo{}).path == "/dev/ttyACM0") << "the best-ranked port first";
        const std::vector<std::string> silent{"/dev/ttyACM0"};
        expect(timesource::pickReceiver(ports, silent).value_or(timesource::PortInfo{}).path == "/dev/ttyUSB0") << "then the next ranked one";
        const std::vector<std::string> both{"/dev/ttyACM0", "/dev/ttyUSB0"};
        expect(timesource::pickReceiver(ports, both).value_or(timesource::PortInfo{}).path == "/dev/ttyACM0") << "after the last, the best-ranked one again";
    });

    cases("serial.refuses-a-path-that-does-not-exist", [] {
        const auto opened = timesource::SerialPort::open("/dev/timesource-no-such-port", timesource::BaudRate::Baud9600);
        expect(!opened.has_value()) << "nothing opens";
        expect(!opened.has_value() && opened.error() == "device '/dev/timesource-no-such-port' does not exist") << "and the reason names the path";
        expect(!timesource::SerialPort::open("", timesource::BaudRate::Baud9600).has_value()) << "an empty path is refused";
    });

    cases("serial.refuses-a-file-that-is-no-serial-port", [] {
        const auto file = std::filesystem::temp_directory_path() / std::format("timesource-qa-{}.txt", ::getpid());
        std::ofstream{file} << "$GPTXT,01,01,02,not a port\r\n";
        const auto opened = timesource::SerialPort::open(file.string(), timesource::BaudRate::Baud9600);
        std::filesystem::remove(file);
        expect(fatal(!opened.has_value())) << "a regular file is refused";
        const std::string lookup = std::format("device '{}' is not a serial port: ", file.string());
#if defined(__linux__)
        const std::string prefix = lookup; // the sysfs lookup refuses the path
#else
        const std::string opening = std::format("cannot open '{}': ", file.string());
        const std::string prefix  = opened.error().starts_with(opening) ? opening : lookup;
#endif
        expect(opened.error().starts_with(prefix)) << "the reason names the path: " << opened.error();
        expect(opened.error().size() > prefix.size()) << "and carries libserialport's message";
    });

    return cases.finish();
}
