#include "aegisflow/rule/rule_lexer.hpp"

#include <cctype>
#include <stdexcept>
#include <utility>

namespace aegisflow::rule {

namespace {
bool isDigit(char c) {
    return std::isdigit(static_cast<unsigned char>(c)) != 0;
}

bool isWhitespace(char c) {
    return std::isspace(static_cast<unsigned char>(c)) != 0;
}

Token makeToken(
    TokenType type,
    std::string text,
    size_t line,
    size_t column
) {
    return {type, std::move(text), line, column};
}

TokenType keywordType(const std::string& text) {
    if (text == "RULE") {
        return TokenType::Rule;
    }

    if (text == "SCENE") {
        return TokenType::Scene;
    }

    if (text == "PRIORITY") {
        return TokenType::Priority;
    }

    if (text == "IF") {
        return TokenType::If;
    }

    if (text == "THEN") {
        return TokenType::Then;
    }

    if (text == "REASON") {
        return TokenType::Reason;
    }

    if (text == "AND") {
        return TokenType::And;
    }

    if (text == "OR") {
        return TokenType::Or;
    }

    if (text == "NOT") {
        return TokenType::Not;
    }

    if (text == "PASS") {
        return TokenType::Pass;
    }

    if (text == "REVIEW") {
        return TokenType::Review;
    }

    if (text == "REJECT") {
        return TokenType::Reject;
    }

    if (text == "true") {
        return TokenType::True;
    }

    if (text == "false") {
        return TokenType::False;
    }

    return TokenType::Identifier;
}

} // namespace

RuleLexer::RuleLexer(std::string input) 
    : input_(std::move(input)) {}

std::vector<Token> RuleLexer::tokenize() {
    std::vector<Token> tokens;

    while  (true) {
        Token token = nextToken();
        const bool end = token.type == TokenType::End;
        tokens.push_back(std::move(token));

        if (end) {
            break;
        }

    }

    return tokens;
}

Token RuleLexer::nextToken() {
    skipWhitespaceAndComments();

    const size_t token_line = line_;
    const size_t token_column = column_;
    const char c = peek();

    if (c == '\0') {
        return makeToken(TokenType::End, "", token_line, token_column);
    }

    if (isIdentifierStart(c)) {
        return readIdentifier();
    }

    if (isDigit(c)) {
        return readNumber();
    }

    if (c == '"') {
        return readString();
    }

    if (c == '(') {
        advance();
        return makeToken(TokenType::LParen, "(", token_line, token_column);
    }

    if (c == ')') {
        advance();
        return makeToken(TokenType::RParen, ")", token_line, token_column);
    }

    if (c == '>') {
        advance();
        if (peek() == '=') {
            advance();
            return makeToken(TokenType::Ge, ">=", token_line, token_column);
        }
        return makeToken(TokenType::Gt, ">", token_line, token_column);
    }

    if (c == '<') {
        advance();
        if (peek() == '=') {
            advance();
            return makeToken(TokenType::Le, "<=", token_line, token_column);
        }
        return makeToken(TokenType::Lt, "<", token_line, token_column);
    }

    if (c == '=') {
        advance();
        if (peek() == '=') {
            advance();
            return makeToken(TokenType::Eq, "==", token_line, token_column);
        }
        failAt(token_line, token_column, "expected '=' after '='");
    }

    if (c == '!') {
        advance();
        if (peek() == '=') {
            advance();
            return makeToken(TokenType::Ne, "!=", token_line, token_column);
        }
        failAt(token_line, token_column, "expected '=' after '!'");
    }

    failAt(
        token_line,
        token_column,
        "unexpected character '" + std::string(1, c) + "'"
    );
}

Token RuleLexer::readIdentifier() {
    const size_t token_line = line_;
    const size_t token_column = column_;

    std::string text;
    while (isIdentifierPart(peek())) {
        text.push_back(advance());
    }

    const TokenType type = keywordType(text);
    return makeToken(type, std::move(text), token_line, token_column);
}

Token RuleLexer::readNumber() {
    const size_t token_line = line_;
    const size_t token_column = column_;

    std::string text;
    while (isDigit(peek())) {
        text.push_back(advance());
    }

    if (peek() == '.' && isDigit(peekNext())) {
        text.push_back(advance());

        while (isDigit(peek())) {
            text.push_back(advance());
        }
    }

    return makeToken(TokenType::Number, std::move(text), token_line, token_column);
}


Token RuleLexer::readString() {
    const size_t token_line = line_;
    const size_t token_column = column_;

    advance();
    
    std::string text;
    while (true) {
        const char c = peek();

        if (c == '\0') {
            failAt(token_line, token_column, "unterminated string literal");
        }

        if (c == '\n') {
            failAt(token_line, token_column, "unterminated string literal");
        }

        if (c == '"') {
            advance();
            return makeToken(
                TokenType::String,
                std::move(text),
                token_line,
                token_column
            );
        }
        text.push_back(advance());
    }
}

char RuleLexer::peek() const {
    if (pos_ >= input_.size()) {
        return '\0';
    }
    return input_[pos_];
}

char RuleLexer::peekNext() const {
    const size_t next_pos = pos_ + 1;
    if (next_pos >= input_.size()) {
        return '\0';
    }

    return input_[next_pos];
}

char RuleLexer::advance() {
    if (pos_ >= input_.size()) {
        return '\0';
    }

    const char c = input_[pos_++];

    if (c == '\n') {
        ++line_;
        column_ = 1;
    } else {
        ++column_;
    }

    return c;
}

void RuleLexer::skipWhitespaceAndComments() {
    while (true) {
        bool skipped = false;

        while (isWhitespace(peek())) {
            advance();
            skipped = true;
        }

        if (peek() == '#') {
            while (peek() != '\0' && peek() != '\n') {
                advance();
            }
            skipped = true;
        }

        if (!skipped) {
            return;
        }
    }
}

void RuleLexer::fail(const std::string& message) const {
    failAt(line_, column_, message);
}

void RuleLexer::failAt(
    size_t line,
    size_t column,
    const std::string& message
) const {
    throw std::runtime_error(
        "rule lexer error at " + std::to_string(line) +
        ":" + std::to_string(column) + " " + message
    );
}

bool RuleLexer::isIdentifierStart(char c) {
    return std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_';
}

bool RuleLexer::isIdentifierPart(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 ||
        c == '_' ||
        c == '.';
}

} // namespace aegisflow::rule
