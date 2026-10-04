#include "nanobook/gateway.hpp"

namespace nanobook {
namespace {
struct ExecutionContext {
    const Command* command;
    EventCallback callback;
    void* callback_context;
};

void emit_trade(const Trade& trade, void* opaque) {
    auto& context = *static_cast<ExecutionContext*>(opaque);
    if (!context.callback) return;
    context.callback(Event{EventType::executed,
                           context.command->sequence,
                           context.command->symbol,
                           trade.incoming_id,
                           trade.resting_id,
                           trade.price,
                           trade.quantity,
                           0,
                           Status::ok,
                           ProtocolError::none},
                     context.callback_context);
}

void emit_terminal(const Command& command, const Result& result, EventType success_type,
                   EventCallback callback, void* context) {
    if (!callback) return;
    const bool accepted = result.status == Status::ok;
    callback(Event{accepted ? success_type : EventType::rejected,
                   command.sequence,
                   command.symbol,
                   command.order_id,
                   0,
                   command.price,
                   static_cast<std::uint32_t>(result.executed_quantity),
                   result.resting_quantity,
                   result.status,
                   ProtocolError::none},
             context);
}
} // namespace

void SingleBookProcessor::process(const Command& command, EventCallback callback, void* context) {
    if (command.symbol != symbol_) {
        if (callback) {
            callback(Event{EventType::rejected, command.sequence, command.symbol,
                           command.order_id, 0, command.price, command.quantity, 0,
                           Status::invalid_order, ProtocolError::unknown_symbol}, context);
        }
        return;
    }

    Result result;
    ExecutionContext execution{&command, callback, context};
    switch (command.type) {
    case CommandType::enter:
        result = book_.add(command.order_id, command.side, command.price, command.quantity,
                           emit_trade, &execution);
        emit_terminal(command, result, EventType::accepted, callback, context);
        break;
    case CommandType::cancel:
        result = book_.cancel(command.order_id);
        emit_terminal(command, result, EventType::canceled, callback, context);
        break;
    case CommandType::replace:
        result = book_.modify(command.order_id, command.price, command.quantity,
                              emit_trade, &execution);
        emit_terminal(command, result, EventType::replaced, callback, context);
        break;
    }
}

void Gateway::consume(std::span<const std::byte> input, WireCallback callback, void* context) {
    DecodeContext decode_context{this, callback, context};
    decoder_.consume(input, on_command, on_protocol_error, &decode_context);
}

void Gateway::on_command(const Command& decoded, void* opaque) {
    auto& context = *static_cast<DecodeContext*>(opaque);
    auto command = decoded;
    command.sequence = context.gateway->next_sequence_++;
    context.gateway->processor_.process(command, on_event, &context);
}

void Gateway::on_protocol_error(ProtocolError error, void* opaque) {
    auto& context = *static_cast<DecodeContext*>(opaque);
    const Event event{EventType::rejected,
                      context.gateway->next_sequence_++,
                      {}, 0, 0, 0, 0, 0,
                      Status::invalid_order,
                      error};
    on_event(event, &context);
}

void Gateway::on_event(const Event& event, void* opaque) {
    auto& context = *static_cast<DecodeContext*>(opaque);
    if (!context.callback) return;
    const auto frame = encode_event(event);
    context.callback({frame.bytes.data(), frame.size}, context.callback_context);
}

} // namespace nanobook
