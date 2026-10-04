#include "nanobook/journal.hpp"

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace nanobook {
namespace {

constexpr std::size_t record_size = 49;
constexpr std::uint32_t record_magic = 0x4e424a31; // "NBJ1"
constexpr std::uint8_t journal_version = 1;

void put_u16(std::byte* out, std::uint16_t value) {
    out[0] = std::byte(value >> 8);
    out[1] = std::byte(value);
}

void put_u32(std::byte* out, std::uint32_t value) {
    for (int i = 3; i >= 0; --i) out[3 - i] = std::byte(value >> (i * 8));
}

void put_u64(std::byte* out, std::uint64_t value) {
    for (int i = 7; i >= 0; --i) out[7 - i] = std::byte(value >> (i * 8));
}

std::uint16_t get_u16(const std::byte* in) {
    return (std::to_integer<std::uint16_t>(in[0]) << 8) |
           std::to_integer<std::uint16_t>(in[1]);
}

std::uint32_t get_u32(const std::byte* in) {
    std::uint32_t value = 0;
    for (int i = 0; i < 4; ++i) value = (value << 8) | std::to_integer<std::uint8_t>(in[i]);
    return value;
}

std::uint64_t get_u64(const std::byte* in) {
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) value = (value << 8) | std::to_integer<std::uint8_t>(in[i]);
    return value;
}

std::uint32_t crc32(std::span<const std::byte> bytes) {
    std::uint32_t crc = 0xffffffffU;
    for (const auto byte : bytes) {
        crc ^= std::to_integer<std::uint8_t>(byte);
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
        }
    }
    return ~crc;
}

std::array<std::byte, record_size> encode_record(const Command& command) {
    std::array<std::byte, record_size> record{};
    put_u32(record.data(), record_magic);
    put_u16(record.data() + 4, record_size);
    record[6] = std::byte(journal_version);
    record[7] = std::byte(static_cast<std::uint8_t>(command.type));
    put_u64(record.data() + 8, command.sequence);
    std::memcpy(record.data() + 16, command.symbol.bytes.data(), command.symbol.bytes.size());
    put_u64(record.data() + 24, command.order_id);
    record[32] = std::byte(command.side == Side::buy ? 0 : 1);
    put_u64(record.data() + 33, static_cast<std::uint64_t>(command.price));
    put_u32(record.data() + 41, command.quantity);
    put_u32(record.data() + 45, crc32({record.data(), 45}));
    return record;
}

bool decode_record(const std::array<std::byte, record_size>& record, Command& command) {
    if (get_u32(record.data()) != record_magic || get_u16(record.data() + 4) != record_size ||
        std::to_integer<std::uint8_t>(record[6]) != journal_version ||
        get_u32(record.data() + 45) != crc32({record.data(), 45})) return false;
    const auto type = std::to_integer<std::uint8_t>(record[7]);
    if (type > static_cast<std::uint8_t>(CommandType::replace)) return false;
    const auto side = std::to_integer<std::uint8_t>(record[32]);
    if (side > 1) return false;
    command.type = static_cast<CommandType>(type);
    command.sequence = get_u64(record.data() + 8);
    std::memcpy(command.symbol.bytes.data(), record.data() + 16, command.symbol.bytes.size());
    command.order_id = get_u64(record.data() + 24);
    command.side = side == 0 ? Side::buy : Side::sell;
    command.price = static_cast<std::int64_t>(get_u64(record.data() + 33));
    command.quantity = get_u32(record.data() + 41);
    return command.sequence != 0 && !command.symbol.empty();
}

bool write_all(int descriptor, std::span<const std::byte> bytes) {
    while (!bytes.empty()) {
        const auto written = ::write(descriptor, bytes.data(), bytes.size());
        if (written > 0) {
            bytes = bytes.subspan(static_cast<std::size_t>(written));
        } else if (written < 0 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

bool read_at(int descriptor, std::span<std::byte> bytes, off_t offset, std::size_t& read_count) {
    read_count = 0;
    while (!bytes.empty()) {
        const auto count = ::pread(descriptor, bytes.data(), bytes.size(), offset + read_count);
        if (count > 0) {
            read_count += static_cast<std::size_t>(count);
            bytes = bytes.subspan(static_cast<std::size_t>(count));
        } else if (count == 0) {
            return true;
        } else if (errno != EINTR) {
            return false;
        }
    }
    return true;
}

struct ReplayContext {
    RecoveryResult* result;
};

void observe_replay(const Event& event, void* opaque) {
    if (event.type == EventType::rejected) {
        ++static_cast<ReplayContext*>(opaque)->result->rejected_commands;
    }
}

RecoveryResult scan(int descriptor, CommandProcessor* processor) {
    RecoveryResult result;
    struct stat metadata{};
    if (::fstat(descriptor, &metadata) != 0) {
        result.status = RecoveryStatus::io_error;
        result.system_error = errno;
        return result;
    }
    const auto file_size = static_cast<std::uint64_t>(metadata.st_size);
    std::array<std::byte, record_size> record{};
    ReplayContext replay{&result};
    for (std::uint64_t offset = 0; offset + record_size <= file_size; offset += record_size) {
        std::size_t count = 0;
        if (!read_at(descriptor, record, static_cast<off_t>(offset), count) || count != record_size) {
            result.status = RecoveryStatus::io_error;
            result.system_error = errno;
            return result;
        }
        Command command;
        if (!decode_record(record, command) || command.sequence <= result.last_sequence) {
            result.status = RecoveryStatus::corrupt;
            result.valid_bytes = offset;
            return result;
        }
        if (processor) processor->process(command, observe_replay, &replay);
        ++result.records;
        result.last_sequence = command.sequence;
        result.valid_bytes = offset + record_size;
    }
    if (result.valid_bytes != file_size) result.status = RecoveryStatus::truncated_tail;
    return result;
}

} // namespace

RecoveryResult recover_journal(std::string_view path, CommandProcessor& processor) {
    const std::string name(path);
    const int descriptor = ::open(name.c_str(), O_RDONLY);
    if (descriptor < 0) {
        return {RecoveryStatus::io_error, 0, 0, 0, 0, errno};
    }
    auto result = scan(descriptor, &processor);
    ::close(descriptor);
    return result;
}

JournaledProcessor::JournaledProcessor(std::string_view path, CommandProcessor& downstream,
                                       JournalOptions options)
    : downstream_(downstream), options_(options) {
    if (options_.durability == Durability::batch && options_.batch_size == 0) {
        throw std::invalid_argument("journal batch size must be positive");
    }
    const std::string name(path);
    descriptor_ = ::open(name.c_str(), O_CREAT | O_RDWR | O_APPEND, 0644);
    if (descriptor_ < 0) throw std::runtime_error("cannot open journal: " + std::string(std::strerror(errno)));
    const auto inspected = scan(descriptor_, nullptr);
    if (inspected.status == RecoveryStatus::corrupt || inspected.status == RecoveryStatus::io_error) {
        ::close(descriptor_);
        descriptor_ = -1;
        throw std::runtime_error("journal validation failed");
    }
    if (inspected.status == RecoveryStatus::truncated_tail &&
        ::ftruncate(descriptor_, static_cast<off_t>(inspected.valid_bytes)) != 0) {
        ::close(descriptor_);
        descriptor_ = -1;
        throw std::runtime_error("cannot truncate journal crash tail");
    }
    last_sequence_ = inspected.last_sequence;
}

JournaledProcessor::~JournaledProcessor() {
    if (descriptor_ >= 0) ::close(descriptor_);
}

bool JournaledProcessor::append(const Command& command) {
    if (!healthy_ || command.sequence <= last_sequence_) return false;
    const auto record = encode_record(command);
    if (!write_all(descriptor_, record)) {
        healthy_ = false;
        return false;
    }
    last_sequence_ = command.sequence;
    ++unsynced_;
    const bool must_sync = options_.durability == Durability::every_command ||
        (options_.durability == Durability::batch && unsynced_ >= options_.batch_size);
    if (must_sync && !flush()) {
        healthy_ = false;
        return false;
    }
    return true;
}

bool JournaledProcessor::flush() {
    if (descriptor_ < 0 || ::fsync(descriptor_) != 0) {
        healthy_ = false;
        return false;
    }
    unsynced_ = 0;
    return true;
}

void JournaledProcessor::process(const Command& command, EventCallback callback, void* context) {
    if (!append(command)) {
        if (callback) {
            callback(Event{EventType::rejected, command.sequence, command.symbol,
                           command.order_id, 0, command.price, command.quantity, 0,
                           Status::invalid_order, ProtocolError::journal_failure}, context);
        }
        return;
    }
    downstream_.process(command, callback, context);
}

} // namespace nanobook
