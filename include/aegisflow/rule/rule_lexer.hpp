#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "aegisflow/rule/token.hpp"

namespace aegisflow::rule {

class RuleLexer {
public:
    explicit RuleLexer(std::string input);

    std::vector<Token> tokenize();

private:
    Token nextToken();
    Token readIdentifier();
    Token readNumber();
    Token readString();

    char peek() const;
    char peekNext() const;
    char advance();

    void skipWhitespaceAndComments();

    [[noreturn]] void fail(const std::string& message) const;
    [[noreturn]] void failAt(
        size_t line,
        size_t column,
        const std::string& message
    ) const;

    static bool isIdentifierStart(char c);
    static bool isIdentifierPart(char c);

private:
    std::string input_;
    size_t pos_ = 0;
    size_t line_ = 1;
    size_t column_ = 1;
};

} // namespace aegisflow::rule