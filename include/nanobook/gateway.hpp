#pragma once

#include "nanobook/protocol.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace nanobook {

using EventCallback = void (*)(const Event&, void*);
using WireCallback = void (*)(std::span<const std::byte>, void*);

class CommandProcessor {
public:
    virtual ~CommandProcessor() = default;
    virtual void process(const Command& command, EventCallback callback, void* context) = 0;
};

// Adapts one OrderBook to the command interface used by the network gateway.
// Multi-symbol routing implements the same interface in a later layer.
class SingleBookProcessor final : public CommandProcessor {
public:
    SingleBookProcessor(Symbol symbol, OrderBook& book) : symbol_(symbol), book_(book) {}
    void process(const Command& command, EventCallback callback, void* context) override;

private:
    Symbol symbol_;
    OrderBook& book_;
};

// Owns stream framing and the single-writer ingress sequence. A Gateway object
// represents one ordered session; callers may feed arbitrarily fragmented TCP
// reads and receive one or more complete response frames.
class Gateway {
public:
    explicit Gateway(CommandProcessor& processor, std::uint64_t first_sequence = 1)
        : processor_(processor), next_sequence_(first_sequence) {}

    void consume(std::span<const std::byte> input, WireCallback callback, void* context);
    // Discards a partial frame when a transport session closes while preserving
    // the global ingress sequence.
    void reset_session() { decoder_.reset(); }
    std::uint64_t next_sequence() const { return next_sequence_; }

private:
    struct DecodeContext {
        Gateway* gateway;
        WireCallback callback;
        void* callback_context;
    };

    FrameDecoder decoder_;
    CommandProcessor& processor_;
    std::uint64_t next_sequence_;

    static void on_command(const Command& command, void* context);
    static void on_protocol_error(ProtocolError error, void* context);
    static void on_event(const Event& event, void* context);
};

} // namespace nanobook
