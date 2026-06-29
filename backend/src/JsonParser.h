#pragma once

#include <cstddef>
#include <map>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace rtlzz::json {

struct Value;
using Array = std::vector<Value>;
using Object = std::map<std::string, Value>;

struct Value {
    using Storage = std::variant<std::nullptr_t, bool, double, std::string, Array, Object>;

    Storage data;

    bool isNull() const;
    bool isBool() const;
    bool isNumber() const;
    bool isString() const;
    bool isArray() const;
    bool isObject() const;

    bool asBool() const;
    int asInt() const;
    double asNumber() const;
    const std::string& asString() const;
    const Array& asArray() const;
    const Object& asObject() const;

    const Value& at(const std::string& key) const;
    const Value* find(const std::string& key) const;
};

class ParseError : public std::runtime_error {
public:
    ParseError(std::string message, std::size_t offset);
    std::size_t offset() const { return offset_; }

private:
    std::size_t offset_;
};

Value parse(const std::string& text);

} // namespace rtlzz::json
