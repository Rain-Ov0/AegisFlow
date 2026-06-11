#include "aegisflow/rule/rule_parser.hpp"

#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace aegisflow::rule {

namespace {

int parsePriority(const Token& token) {
    if (token.text.find('.') != std::string::npos) {
        throw RuleParseError(token.line, token.column, "priority must be an integer");
    }

    try {
        const long long value = std::stoll(token.text);
        if (value > std::numeric_limits<int>::max()) {
            throw RuleParseError(token.line, token.column, "priority is too large");
        }
        return static_cast<int>(value);
    } catch (const std::invalid_argument&) {
        throw RuleParseError(token.line, token.column, "invalid priority");
    } catch (const std::out_of_range&) {
        throw RuleParseError(token.line, token.column, "priority is too large");
    }
}

double parseNumber(const Token& token) {
    try {
        size_t parsed = 0;
        const double value = std::stod(token.text, &parsed);
        if (parsed != token.text.size()) {
            throw RuleParseError(token.line, token.column, "invalid number literal");
        }
        return value;
    } catch (const std::invalid_argument&) {
        throw RuleParseError(token.line, token.column, "invalid number literal");
    } catch (const std::out_of_range&) {
        throw RuleParseError(token.line, token.column, "number literal is out of range");
    }
}

} // namespace

RuleParser::RuleParser(std::vector<Token> tokens)
    : tokens_(std::move(tokens)) {
    if (tokens_.empty() || tokens_.back().type != TokenType::End) {
        tokens_.push_back({TokenType::End, "", 1, 1});
    }
}

RuleSet RuleParser::parseRuleSet() {
    pos_ = 0;
    rule_set_ = RuleSet{};
    condition_cache_.clear();

    while (!isAtEnd()) {
        rule_set_.rules.push_back(parseRule());
    }

    return rule_set_;
}

Rule RuleParser::parseRule() {
    consume(TokenType::Rule, "expected RULE");

    const Token name = consume(TokenType::Identifier, "expected rule name");

    consume(TokenType::Scene, "expected SCENE");
    const Token scene = consume(TokenType::Identifier, "expected scene name");

    consume(TokenType::Priority, "expected PRIORITY");
    const Token priority = consume(TokenType::Number, "expected priority number");

    consume(TokenType::If, "expected IF");
    const uint32_t root_node_id = parseExpr();

    consume(TokenType::Then, "expected THEN");
    const DecisionAction action = parseAction();

    consume(TokenType::Reason, "expected REASON");
    const Token reason = consume(TokenType::String, "expected reason string");

    Rule rule;
    rule.id = static_cast<uint64_t>(rule_set_.rules.size() + 1);
    rule.name = name.text;
    rule.scene = scene.text;
    rule.priority = parsePriority(priority);
    rule.action = action;
    rule.reason_code = reason.text;
    rule.root_node_id = root_node_id;
    return rule;
}

uint32_t RuleParser::parseExpr() {
    return parseOr();
}

uint32_t RuleParser::parseOr() {
    uint32_t left = parseAnd();

    while (match(TokenType::Or)) {
        const uint32_t right = parseAnd();
        left = makeLogicNode(NodeType::Or, {left, right});
    }

    return left;
}

uint32_t RuleParser::parseAnd() {
    uint32_t left = parseUnary();

    while (match(TokenType::And)) {
        const uint32_t right = parseUnary();
        left = makeLogicNode(NodeType::And, {left, right});
    }

    return left;
}

uint32_t RuleParser::parseUnary() {
    if (match(TokenType::Not)) {
        const uint32_t child = parseUnary();
        return makeLogicNode(NodeType::Not, {child});
    }

    return parsePrimary();
}

uint32_t RuleParser::parsePrimary() {
    if (match(TokenType::LParen)) {
        const uint32_t node_id = parseExpr();
        consume(TokenType::RParen, "expected ')' after expression");
        return node_id;
    }

    return parseCondition();
}

uint32_t RuleParser::parseCondition() {
    const Token feature = consume(TokenType::Identifier, "expected feature name");

    Condition condition;
    condition.feature_name = feature.text;
    condition.line = feature.line;
    condition.column = feature.column;
    condition.op = parseCompareOp();
    condition.expected = parseValue();

    return makeConditionNode(condition);
}

CompareOp RuleParser::parseCompareOp() {
    if (match(TokenType::Eq)) {
        return CompareOp::Eq;
    }
    if (match(TokenType::Ne)) {
        return CompareOp::Ne;
    }
    if (match(TokenType::Gt)) {
        return CompareOp::Gt;
    }
    if (match(TokenType::Ge)) {
        return CompareOp::Ge;
    }
    if (match(TokenType::Lt)) {
        return CompareOp::Lt;
    }
    if (match(TokenType::Le)) {
        return CompareOp::Le;
    }

    fail(peek(), "expected comparison operator");
}

Value RuleParser::parseValue() {
    if (match(TokenType::Number)) {
        Value value;
        value.type = ValueType::Number;
        value.number_value = parseNumber(previous());
        return value;
    }

    if (match(TokenType::True)) {
        Value value;
        value.type = ValueType::Bool;
        value.bool_value = true;
        return value;
    }

    if (match(TokenType::False)) {
        Value value;
        value.type = ValueType::Bool;
        value.bool_value = false;
        return value;
    }

    if (match(TokenType::String)) {
        Value value;
        value.type = ValueType::String;
        value.string_value = previous().text;
        return value;
    }

    fail(peek(), "expected value");
}

DecisionAction RuleParser::parseAction() {
    if (match(TokenType::Pass)) {
        return DecisionAction::Pass;
    }
    if (match(TokenType::Review)) {
        return DecisionAction::Review;
    }
    if (match(TokenType::Reject)) {
        return DecisionAction::Reject;
    }

    fail(peek(), "expected action PASS, REVIEW, or REJECT");
}

Token RuleParser::consume(TokenType type, const std::string& message) {
    if (check(type)) {
        const Token token = peek();
        ++pos_;
        return token;
    }
    fail(peek(), message);
}

bool RuleParser::match(TokenType type) {
    if (!check(type)) {
        return false;
    }

    ++pos_;
    return true;
}

bool RuleParser::check(TokenType type) const {
    if (tokens_.empty()) {
        return type == TokenType::End;
    }

    return peek().type == type;
}

bool RuleParser::isAtEnd() const {
    return check(TokenType::End);
}

const Token& RuleParser::peek() const {
    if (pos_ >= tokens_.size()) {
        return tokens_.back();
    }

    return tokens_[pos_];
}

const Token& RuleParser::previous() const {
    return tokens_[pos_ - 1];
}

void RuleParser::fail(
    const Token& token,
    const std::string& message
) const {
    throw RuleParseError(token.line, token.column, message);
}

uint32_t RuleParser::makeConditionNode(const Condition& condition) {
    const std::string key = conditionKey(condition);
    auto it = condition_cache_.find(key);
    if (it != condition_cache_.end()) {
        return it->second;
    }

    const uint32_t id = static_cast<uint32_t>(rule_set_.nodes.size());

    RuleNode node;
    node.id = id;
    node.type = NodeType::Condition;
    node.condition = condition;

    rule_set_.nodes.push_back(std::move(node));
    condition_cache_.emplace(key, id);

    return id;
}

uint32_t RuleParser::makeLogicNode(
    NodeType type,
    const std::vector<uint32_t>& children
) {
    const uint32_t id = static_cast<uint32_t>(rule_set_.nodes.size());

    RuleNode node;
    node.id = id;
    node.type = type;
    node.children = children;

    rule_set_.nodes.push_back(std::move(node));
    return id;
}

std::string RuleParser::conditionKey(const Condition& condition) {
    return condition.feature_name + "|" +
        opToString(condition.op) + "|" +
        valueToString(condition.expected);
}

std::string RuleParser::opToString(CompareOp op) {
    switch (op) {
    case CompareOp::Eq:
        return "==";
    case CompareOp::Ne:
        return "!=";
    case CompareOp::Gt:
        return ">";
    case CompareOp::Ge:
        return ">=";
    case CompareOp::Lt:
        return "<";
    case CompareOp::Le:
        return "<=";
    }

    return "";
}

std::string RuleParser::valueToString(const Value& value) {
    switch (value.type) {
    case ValueType::Number: {
        std::ostringstream oss;
        oss << "number:" << std::setprecision(std::numeric_limits<double>::max_digits10)
            << value.number_value;
        return oss.str();
    }
    case ValueType::Bool:
        return value.bool_value ? "bool:true" : "bool:false";
    case ValueType::String:
        return "string:" + value.string_value;
    }

    return "";
}

} // namespace aegisflow::rule