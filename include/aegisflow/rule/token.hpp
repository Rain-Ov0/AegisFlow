#pragma once

#include <cstddef>
#include <string>

namespace aegisflow::rule {

enum class TokenType {
    End,
    Identifier,
    Number,
    String,

    Rule,
    Scene,
    Priority,
    If,
    Then,
    Reason,

    And,
    Or,
    Not,

    Pass,
    Review,
    Reject,

    True,
    False,

    LParen,
    RParen,

    Eq,
    Ne,
    Gt,
    Ge,
    Lt,
    Le
};

struct Token {
    TokenType type = TokenType::End;
    std::string text;
    size_t line = 1;
    size_t column = 1;
};

} // namespace aegisflow::rule