#pragma once

#include "nanobook/gateway.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace nanobook {

enum class Durability : std::uint8_t {
    buffered,
    batch,
    every_command
};

struct JournalOptions {
    Durability durability = Durability::buffered;
    std::uint32_t batch_size = 1'024;
};

enum class RecoveryStatus : std::uint8_t {
    clean,
    truncated_tail,
    corrupt,
    io_error
};

struct RecoveryResult {
    RecoveryStatus status = RecoveryStatus::clean;
    std::uint64_t records = 0;
    std::uint64_t rejected_commands = 0;
    std::uint64_t last_sequence = 0;
    std::uint64_t valid_bytes = 0;
    int system_error = 0;
};

// Replays all checksum-valid records in sequence order. A short final record is
// treated as a crash tail and is not applied. Corruption in a complete record
// fails recovery rather than guessing at state.
RecoveryResult recover_journal(std::string_view path, CommandProcessor& processor);

// Write-ahead decorator: a command is appended (and optionally synced) before
// it reaches the wrapped matching processor.
class JournaledProcessor final : public CommandProcessor {
public:
    JournaledProcessor(std::string_view path, CommandProcessor& downstream,
                       JournalOptions options = {});
    ~JournaledProcessor() override;
    JournaledProcessor(const JournaledProcessor&) = delete;
    JournaledProcessor& operator=(const JournaledProcessor&) = delete;

    void process(const Command& command, EventCallback callback, void* context) override;
    bool flush();
    bool healthy() const { return healthy_; }
    std::uint64_t last_sequence() const { return last_sequence_; }

private:
    int descriptor_ = -1;
    CommandProcessor& downstream_;
    JournalOptions options_;
    std::uint32_t unsynced_ = 0;
    std::uint64_t last_sequence_ = 0;
    bool healthy_ = true;

    bool append(const Command& command);
};

} // namespace nanobook
