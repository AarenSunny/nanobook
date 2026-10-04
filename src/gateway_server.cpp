#include "nanobook/journal.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

namespace {

volatile std::sig_atomic_t running = 1;

void stop(int) { running = 0; }

bool write_all(int descriptor, std::span<const std::byte> bytes) {
    while (!bytes.empty()) {
        const auto written = ::send(descriptor, bytes.data(), bytes.size(), 0);
        if (written > 0) {
            bytes = bytes.subspan(static_cast<std::size_t>(written));
            continue;
        }
        if (written < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

struct ClientContext {
    int descriptor;
    bool healthy = true;
};

void send_response(std::span<const std::byte> frame, void* opaque) {
    auto& client = *static_cast<ClientContext*>(opaque);
    if (client.healthy) client.healthy = write_all(client.descriptor, frame);
}

std::uint16_t parse_port(std::string_view text) {
    const std::string value_text(text);
    char* end = nullptr;
    const auto value = std::strtoul(value_text.c_str(), &end, 10);
    if (text.empty() || text.front() == '-' || end != value_text.c_str() + value_text.size() ||
        value == 0 || value > 65'535) {
        throw std::invalid_argument("port must be between 1 and 65535");
    }
    return static_cast<std::uint16_t>(value);
}

} // namespace

int main(int argc, char** argv) try {
    std::uint16_t port = 9'001;
    auto symbol = nanobook::Symbol::from_string("AAPL");
    std::optional<std::string> journal_path;
    std::uint32_t sync_every = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string_view option = argv[i];
        if (option == "--port" && i + 1 < argc) port = parse_port(argv[++i]);
        else if (option == "--symbol" && i + 1 < argc) symbol = nanobook::Symbol::from_string(argv[++i]);
        else if (option == "--journal" && i + 1 < argc) journal_path = argv[++i];
        else if (option == "--sync-every" && i + 1 < argc) {
            const std::string_view text = argv[++i];
            const std::string value_text(text);
            char* end = nullptr;
            const auto parsed = std::strtoul(value_text.c_str(), &end, 10);
            if (text.empty() || text.front() == '-' ||
                end != value_text.c_str() + value_text.size()) {
                throw std::invalid_argument("invalid sync interval");
            }
            if (parsed > UINT32_MAX) throw std::invalid_argument("sync interval is too large");
            sync_every = static_cast<std::uint32_t>(parsed);
        }
        else {
            std::cerr << "usage: nanobook-gateway [--port PORT] [--symbol SYMBOL] "
                         "[--journal PATH] [--sync-every N]\n";
            return 2;
        }
    }

    // Prices use $0.0001 units. This ladder accepts $0.01-$1,000 in one-cent ticks.
    nanobook::OrderBook book({100, 10'000'000, 100, 1'000'000});
    nanobook::SingleBookProcessor processor(symbol, book);
    nanobook::CommandProcessor* active_processor = &processor;
    std::unique_ptr<nanobook::JournaledProcessor> journal;
    std::uint64_t first_sequence = 1;
    if (journal_path) {
        const auto recovery = nanobook::recover_journal(*journal_path, processor);
        if (recovery.status == nanobook::RecoveryStatus::corrupt ||
            (recovery.status == nanobook::RecoveryStatus::io_error &&
             recovery.system_error != ENOENT)) {
            throw std::runtime_error("journal recovery failed");
        }
        nanobook::JournalOptions journal_options;
        if (sync_every == 1) journal_options.durability = nanobook::Durability::every_command;
        else if (sync_every > 1) {
            journal_options.durability = nanobook::Durability::batch;
            journal_options.batch_size = sync_every;
        }
        journal = std::make_unique<nanobook::JournaledProcessor>(
            *journal_path, processor, journal_options);
        active_processor = journal.get();
        first_sequence = recovery.last_sequence + 1;
        std::cout << "Recovered " << recovery.records << " commands; next sequence "
                  << first_sequence << '\n';
    }
    nanobook::Gateway gateway(*active_processor, first_sequence);

    std::signal(SIGINT, stop);
    std::signal(SIGTERM, stop);
    std::signal(SIGPIPE, SIG_IGN);

    const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) throw std::runtime_error(std::strerror(errno));
    const int reuse = 1;
    if (::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        throw std::runtime_error(std::strerror(errno));
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    if (::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0 ||
        ::listen(listener, 64) < 0) {
        throw std::runtime_error(std::strerror(errno));
    }

    std::cout << "Nanobook gateway listening on 127.0.0.1:" << port
              << " for " << symbol.view() << '\n';
    std::array<std::byte, 64 * 1024> buffer{};
    while (running) {
        const int client_descriptor = ::accept(listener, nullptr, nullptr);
        if (client_descriptor < 0) {
            if (errno == EINTR) continue;
            throw std::runtime_error(std::strerror(errno));
        }
        const int no_delay = 1;
        ::setsockopt(client_descriptor, IPPROTO_TCP, TCP_NODELAY, &no_delay, sizeof(no_delay));
        ClientContext client{client_descriptor};
        while (running && client.healthy) {
            const auto received = ::recv(client_descriptor, buffer.data(), buffer.size(), 0);
            if (received > 0) {
                gateway.consume({buffer.data(), static_cast<std::size_t>(received)},
                                send_response, &client);
            } else if (received == 0) {
                break;
            } else if (errno != EINTR) {
                break;
            }
        }
        ::close(client_descriptor);
        gateway.reset_session();
    }
    if (journal && !journal->flush()) throw std::runtime_error("journal flush failed");
    ::close(listener);
    return 0;
} catch (const std::exception& error) {
    std::cerr << "gateway error: " << error.what() << '\n';
    return 1;
}
