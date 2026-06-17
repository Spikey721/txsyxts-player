#pragma once

#include <string>
#include <optional>

namespace txs {

struct Command {
    std::string name;
    std::string args;
};

std::optional<Command> parse_command(const std::string& input);

extern const char* HELP_TEXT;

} // namespace txs
