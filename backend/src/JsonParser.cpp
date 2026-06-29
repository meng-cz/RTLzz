#include "JsonParser.h"

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <sstream>

namespace rtlzz::json {
namespace {

class Parser {
public:
    explicit Parser(const std::string& text) : text_(text) {}

    Value parseDocument() {
        skipWhitespace();
        Value value = parseValue();
        skipWhitespace();
        if (!eof()) fail("unexpected trailing characters");
        return value;
    }

private:
    const std::string& text_;
    std::size_t pos_ = 0;

    bool eof() const { return pos_ >= text_.size(); }
    char peek() const { return eof() ? '\0' : text_[pos_]; }

    char take() {
        if (eof()) fail("unexpected end of input");
        return text_[pos_++];
    }

    void fail(const std::string& message) const {
        throw ParseError(message, pos_);
    }

    void skipWhitespace() {
        while (!eof() && std::isspace(static_cast<unsigned char>(peek()))) ++pos_;
    }

    bool consume(char ch) {
        if (peek() != ch) return false;
        ++pos_;
        return true;
    }

    void expect(char ch) {
        if (!consume(ch)) {
            std::string message = "expected '";
            message += ch;
            message += "'";
            fail(message);
        }
    }

    Value parseValue() {
        skipWhitespace();
        char ch = peek();
        if (ch == '{') return Value{parseObject()};
        if (ch == '[') return Value{parseArray()};
        if (ch == '"') return Value{parseString()};
        if (ch == '-' || std::isdigit(static_cast<unsigned char>(ch))) return Value{parseNumber()};
        if (matchLiteral("true")) return Value{true};
        if (matchLiteral("false")) return Value{false};
        if (matchLiteral("null")) return Value{nullptr};
        fail("expected JSON value");
        return Value{nullptr};
    }

    bool matchLiteral(const char* literal) {
        std::size_t start = pos_;
        for (const char* p = literal; *p; ++p) {
            if (pos_ >= text_.size() || text_[pos_] != *p) {
                pos_ = start;
                return false;
            }
            ++pos_;
        }
        return true;
    }

    Object parseObject() {
        Object object;
        expect('{');
        skipWhitespace();
        if (consume('}')) return object;
        while (true) {
            skipWhitespace();
            if (peek() != '"') fail("expected object key string");
            std::string key = parseString();
            skipWhitespace();
            expect(':');
            object.emplace(std::move(key), parseValue());
            skipWhitespace();
            if (consume('}')) break;
            expect(',');
        }
        return object;
    }

    Array parseArray() {
        Array array;
        expect('[');
        skipWhitespace();
        if (consume(']')) return array;
        while (true) {
            array.push_back(parseValue());
            skipWhitespace();
            if (consume(']')) break;
            expect(',');
        }
        return array;
    }

    std::string parseString() {
        std::string result;
        expect('"');
        while (true) {
            if (eof()) fail("unterminated string");
            char ch = take();
            if (ch == '"') break;
            if (static_cast<unsigned char>(ch) < 0x20) fail("control character in string");
            if (ch != '\\') {
                result.push_back(ch);
                continue;
            }
            char esc = take();
            switch (esc) {
            case '"': result.push_back('"'); break;
            case '\\': result.push_back('\\'); break;
            case '/': result.push_back('/'); break;
            case 'b': result.push_back('\b'); break;
            case 'f': result.push_back('\f'); break;
            case 'n': result.push_back('\n'); break;
            case 'r': result.push_back('\r'); break;
            case 't': result.push_back('\t'); break;
            case 'u':
                appendUtf8(parseHexQuad(), result);
                break;
            default:
                fail("invalid string escape");
            }
        }
        return result;
    }

    unsigned parseHexQuad() {
        unsigned value = 0;
        for (int i = 0; i < 4; ++i) {
            char ch = take();
            value <<= 4;
            if (ch >= '0' && ch <= '9') value |= static_cast<unsigned>(ch - '0');
            else if (ch >= 'a' && ch <= 'f') value |= static_cast<unsigned>(ch - 'a' + 10);
            else if (ch >= 'A' && ch <= 'F') value |= static_cast<unsigned>(ch - 'A' + 10);
            else fail("invalid unicode escape");
        }
        return value;
    }

    static void appendUtf8(unsigned codepoint, std::string& out) {
        if (codepoint <= 0x7f) {
            out.push_back(static_cast<char>(codepoint));
        } else if (codepoint <= 0x7ff) {
            out.push_back(static_cast<char>(0xc0 | (codepoint >> 6)));
            out.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
        } else {
            out.push_back(static_cast<char>(0xe0 | (codepoint >> 12)));
            out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
        }
    }

    double parseNumber() {
        std::size_t start = pos_;
        consume('-');
        if (consume('0')) {
        } else {
            if (!std::isdigit(static_cast<unsigned char>(peek()))) fail("invalid number");
            while (std::isdigit(static_cast<unsigned char>(peek()))) ++pos_;
        }
        if (consume('.')) {
            if (!std::isdigit(static_cast<unsigned char>(peek()))) fail("invalid number fraction");
            while (std::isdigit(static_cast<unsigned char>(peek()))) ++pos_;
        }
        if (peek() == 'e' || peek() == 'E') {
            ++pos_;
            if (peek() == '+' || peek() == '-') ++pos_;
            if (!std::isdigit(static_cast<unsigned char>(peek()))) fail("invalid number exponent");
            while (std::isdigit(static_cast<unsigned char>(peek()))) ++pos_;
        }
        char* end = nullptr;
        std::string token = text_.substr(start, pos_ - start);
        double value = std::strtod(token.c_str(), &end);
        if (!end || *end != '\0' || !std::isfinite(value)) fail("invalid number value");
        return value;
    }
};

template <typename T>
const T& require(const Value::Storage& data, const char* type_name) {
    const T* value = std::get_if<T>(&data);
    if (!value) {
        std::string message = "JSON value is not ";
        message += type_name;
        throw std::runtime_error(message);
    }
    return *value;
}

} // namespace

bool Value::isNull() const { return std::holds_alternative<std::nullptr_t>(data); }
bool Value::isBool() const { return std::holds_alternative<bool>(data); }
bool Value::isNumber() const { return std::holds_alternative<double>(data); }
bool Value::isString() const { return std::holds_alternative<std::string>(data); }
bool Value::isArray() const { return std::holds_alternative<Array>(data); }
bool Value::isObject() const { return std::holds_alternative<Object>(data); }

bool Value::asBool() const { return require<bool>(data, "a bool"); }

int Value::asInt() const {
    double value = asNumber();
    if (std::floor(value) != value) throw std::runtime_error("JSON number is not an integer");
    return static_cast<int>(value);
}

double Value::asNumber() const { return require<double>(data, "a number"); }
const std::string& Value::asString() const { return require<std::string>(data, "a string"); }
const Array& Value::asArray() const { return require<Array>(data, "an array"); }
const Object& Value::asObject() const { return require<Object>(data, "an object"); }

const Value& Value::at(const std::string& key) const {
    const auto& object = asObject();
    auto it = object.find(key);
    if (it == object.end()) throw std::runtime_error("missing JSON object key: " + key);
    return it->second;
}

const Value* Value::find(const std::string& key) const {
    if (!isObject()) return nullptr;
    const auto& object = asObject();
    auto it = object.find(key);
    if (it == object.end()) return nullptr;
    return &it->second;
}

ParseError::ParseError(std::string message, std::size_t offset)
    : std::runtime_error(message + " at byte offset " + std::to_string(offset)), offset_(offset) {}

Value parse(const std::string& text) {
    return Parser(text).parseDocument();
}

} // namespace rtlzz::json
