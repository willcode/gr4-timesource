/* -*- c++ -*- */
/*
 * Copyright 2026 Jeff Long
 * SPDX-License-Identifier: MIT
 */
#include "support/FakeSerialPort.hpp"

#include <libserialport.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <unistd.h>

/*| role: the definitions of the libserialport functions SerialPort calls, over ports a case
        registers. Linked into a test binary as objects, they take the place of the shared
        library's definitions in that binary.
    contract: one event set waits on one port. sp_wait() returns at the first queued byte or
        after the full timeout, never earlier with nothing queued.
*/

namespace {

struct Channel {
    std::mutex                                 lock;
    std::condition_variable                    arrived;
    std::string                                pending;
    timesource::test::FakePortSettings         settings;
    std::optional<timesource::test::FakeUsbId> usb;
};

std::mutex                                      gRegistryLock;
std::map<std::string, std::shared_ptr<Channel>> gRegistry;
thread_local std::string                        gLastError;

std::shared_ptr<Channel> channelOf(const std::string& path) {
    std::lock_guard guard(gRegistryLock);
    const auto      it = gRegistry.find(path);
    return it != gRegistry.end() ? it->second : nullptr;
}

sp_return fail(sp_return code, std::string message) {
    gLastError = std::move(message);
    return code;
}

} // namespace

struct sp_port {
    std::string              name;
    std::shared_ptr<Channel> channel;
};

namespace timesource::test {

FakePort::FakePort(FakePort&& other) noexcept : _path(std::exchange(other._path, std::string{})) {}

FakePort::~FakePort() {
    if (_path.empty()) {
        return;
    }
    {
        std::lock_guard guard(gRegistryLock);
        gRegistry.erase(_path);
    }
    std::error_code ec;
    std::filesystem::remove(_path, ec);
}

std::optional<FakePort> FakePort::create(std::optional<FakeUsbId> usb) {
    static int      serial = 0;
    std::error_code ec;
    const auto      file = std::filesystem::temp_directory_path(ec) / std::format("timesource-fake-port-{}-{}", ::getpid(), ++serial);
    if (ec || !std::ofstream{file}) {
        return std::nullopt;
    }
    const auto node = std::filesystem::canonical(file, ec);
    if (ec) {
        return std::nullopt;
    }
    FakePort port;
    port._path = node.string();
    std::lock_guard guard(gRegistryLock);
    auto            channel = std::make_shared<Channel>();
    channel->usb            = std::move(usb);
    gRegistry.insert_or_assign(port._path, std::move(channel));
    return port;
}

void FakePort::write(std::string_view bytes) const {
    const auto channel = channelOf(_path);
    if (channel == nullptr) {
        return;
    }
    {
        std::lock_guard guard(channel->lock);
        channel->pending.append(bytes);
    }
    channel->arrived.notify_all();
}

FakePortSettings FakePort::settings() const {
    const auto channel = channelOf(_path);
    if (channel == nullptr) {
        return {};
    }
    std::lock_guard guard(channel->lock);
    return channel->settings;
}

} // namespace timesource::test

extern "C" {

sp_return sp_get_port_by_name(const char* portname, sp_port** port_ptr) {
    if (port_ptr == nullptr) {
        return fail(SP_ERR_ARG, "null result pointer");
    }
    *port_ptr          = nullptr;
    const std::string name = portname != nullptr ? portname : "";
    auto              channel = channelOf(name);
    if (channel == nullptr) {
        return fail(SP_ERR_ARG, std::format("no fake port named '{}'", name));
    }
    *port_ptr = new sp_port{.name = name, .channel = std::move(channel)};
    return SP_OK;
}

void sp_free_port(sp_port* port) { delete port; }

sp_return sp_list_ports(sp_port*** list_ptr) {
    if (list_ptr == nullptr) {
        return fail(SP_ERR_ARG, "null result pointer");
    }
    std::lock_guard guard(gRegistryLock);
    *list_ptr = static_cast<sp_port**>(std::calloc(gRegistry.size() + 1, sizeof(sp_port*)));
    if (*list_ptr == nullptr) {
        return fail(SP_ERR_MEM, "out of memory");
    }
    std::size_t i = 0;
    for (const auto& [name, channel] : gRegistry) {
        (*list_ptr)[i++] = new sp_port{.name = name, .channel = channel};
    }
    return SP_OK;
}

void sp_free_port_list(sp_port** ports) {
    if (ports == nullptr) {
        return;
    }
    for (sp_port** p = ports; *p != nullptr; ++p) {
        sp_free_port(*p);
    }
    std::free(ports);
}

sp_return sp_open(sp_port* port, sp_mode flags) {
    std::lock_guard guard(port->channel->lock);
    port->channel->settings.open = true;
    port->channel->settings.mode = static_cast<int>(flags);
    return SP_OK;
}

sp_return sp_close(sp_port* port) {
    std::lock_guard guard(port->channel->lock);
    port->channel->settings.open = false;
    return SP_OK;
}

char* sp_get_port_name(const sp_port* port) { return const_cast<char*>(port->name.c_str()); }

char* sp_get_port_description(const sp_port* port) { return const_cast<char*>(port->name.c_str()); }

sp_transport sp_get_port_transport(const sp_port* port) { return port->channel->usb ? SP_TRANSPORT_USB : SP_TRANSPORT_NATIVE; }

sp_return sp_get_port_usb_vid_pid(const sp_port* port, int* usb_vid, int* usb_pid) {
    const auto& usb = port->channel->usb;
    if (!usb) {
        return fail(SP_ERR_ARG, "not a USB port");
    }
    *usb_vid = usb->vendorId;
    *usb_pid = usb->productId;
    return SP_OK;
}

char* sp_get_port_usb_manufacturer(const sp_port*) { return nullptr; }

char* sp_get_port_usb_product(const sp_port* port) { return port->channel->usb ? const_cast<char*>(port->channel->usb->product.c_str()) : nullptr; }

sp_return sp_set_baudrate(sp_port* port, int baudrate) {
    std::lock_guard guard(port->channel->lock);
    port->channel->settings.baudrate = baudrate;
    return SP_OK;
}

sp_return sp_set_bits(sp_port* port, int bits) {
    std::lock_guard guard(port->channel->lock);
    port->channel->settings.bits = bits;
    return SP_OK;
}

sp_return sp_set_parity(sp_port* port, sp_parity parity) {
    std::lock_guard guard(port->channel->lock);
    port->channel->settings.parity = static_cast<int>(parity);
    return SP_OK;
}

sp_return sp_set_stopbits(sp_port* port, int stopbits) {
    std::lock_guard guard(port->channel->lock);
    port->channel->settings.stopbits = stopbits;
    return SP_OK;
}

sp_return sp_set_flowcontrol(sp_port* port, sp_flowcontrol flowcontrol) {
    std::lock_guard guard(port->channel->lock);
    port->channel->settings.flowcontrol = static_cast<int>(flowcontrol);
    return SP_OK;
}

sp_return sp_new_event_set(sp_event_set** result_ptr) {
    *result_ptr = new sp_event_set{.handles = nullptr, .masks = nullptr, .count = 0};
    return SP_OK;
}

sp_return sp_add_port_events(sp_event_set* event_set, const sp_port* port, sp_event) {
    if (event_set->count != 0) {
        return fail(SP_ERR_SUPP, "an event set waits on one port");
    }
    event_set->handles = const_cast<sp_port*>(port);
    event_set->count   = 1;
    return SP_OK;
}

sp_return sp_wait(sp_event_set* event_set, unsigned int timeout_ms) {
    if (event_set->count == 0) {
        return SP_OK;
    }
    const auto&       channel = static_cast<sp_port*>(event_set->handles)->channel;
    const auto        until   = std::chrono::steady_clock::now() + std::chrono::milliseconds{timeout_ms};
    std::unique_lock  guard(channel->lock);
    channel->arrived.wait_until(guard, until, [&channel] { return !channel->pending.empty(); });
    return SP_OK;
}

void sp_free_event_set(sp_event_set* event_set) { delete event_set; }

sp_return sp_nonblocking_read(sp_port* port, void* buf, size_t count) {
    std::lock_guard   guard(port->channel->lock);
    if (!port->channel->settings.open) {
        return fail(SP_ERR_ARG, "the port is not open");
    }
    const std::size_t n = std::min(count, port->channel->pending.size());
    std::memcpy(buf, port->channel->pending.data(), n);
    port->channel->pending.erase(0, n);
    return static_cast<sp_return>(n);
}

char* sp_last_error_message(void) { return ::strdup(gLastError.c_str()); }

void sp_free_error_message(char* message) { std::free(message); }

} // extern "C"
