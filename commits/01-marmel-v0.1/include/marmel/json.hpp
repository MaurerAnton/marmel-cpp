#pragma once
// Minimal JSON value + parser/stringifier (std-only).
// Rust origin: serde_json usage across src/types.rs, src/harness/*, src/mcp/*.
// Why vendored: keep the v0.1 port buildable with only the C++ standard
// library + libcurl. API mirrors the subset the port needs:
//   parse / dump / object+array access / equality / merge.
// Objects use std::map (sorted keys) matching Rust BTreeMap/serde_json
// deterministic ordering used in semantic comparisons.

#include <cctype>
#include <cstdint>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace marmel {

class JsonParseError : public std::runtime_error {
public:
    explicit JsonParseError(const std::string& msg) : std::runtime_error(msg) {}
};

class Json {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };
    using Array = std::vector<Json>;
    using Object = std::map<std::string, Json>;

    Json() : type_(Type::Null), bool_(false), number_(0.0) {}
    Json(std::nullptr_t) : Json() {}
    Json(bool b) : type_(Type::Bool), bool_(b), number_(0.0) {}
    Json(int i) : type_(Type::Number), bool_(false), number_(static_cast<double>(i)) {}
    Json(long i) : type_(Type::Number), bool_(false), number_(static_cast<double>(i)) {}
    Json(long long i) : type_(Type::Number), bool_(false), number_(static_cast<double>(i)) {}
    Json(unsigned u) : type_(Type::Number), bool_(false), number_(static_cast<double>(u)) {}
    Json(std::size_t u) : type_(Type::Number), bool_(false), number_(static_cast<double>(u)) {}
    Json(double d) : type_(Type::Number), bool_(false), number_(d) {}
    Json(const char* s) : type_(Type::String), bool_(false), number_(0.0), string_(s ? s : "") {}
    Json(const std::string& s) : type_(Type::String), bool_(false), number_(0.0), string_(s) {}
    Json(std::string&& s) : type_(Type::String), bool_(false), number_(0.0), string_(std::move(s)) {}
    Json(const Array& a) : type_(Type::Array), bool_(false), number_(0.0), array_(std::make_shared<Array>(a)) {}
    Json(Array&& a) : type_(Type::Array), bool_(false), number_(0.0), array_(std::make_shared<Array>(std::move(a))) {}
    Json(const Object& o) : type_(Type::Object), bool_(false), number_(0.0), object_(std::make_shared<Object>(o)) {}
    Json(Object&& o) : type_(Type::Object), bool_(false), number_(0.0), object_(std::make_shared<Object>(std::move(o))) {}

    static Json array(std::initializer_list<Json> items = {}) { return Json(Array(items)); }
    static Json object(std::initializer_list<std::pair<std::string, Json>> items = {}) {
        Object o;
        for (auto& kv : items) o.emplace(kv.first, kv.second);
        return Json(std::move(o));
    }

    Type type() const { return type_; }
    bool is_null() const { return type_ == Type::Null; }
    bool is_bool() const { return type_ == Type::Bool; }
    bool is_number() const { return type_ == Type::Number; }
    bool is_string() const { return type_ == Type::String; }
    bool is_array() const { return type_ == Type::Array; }
    bool is_object() const { return type_ == Type::Object; }

    bool as_bool(bool fallback = false) const { return type_ == Type::Bool ? bool_ : fallback; }
    double as_double(double fallback = 0.0) const { return type_ == Type::Number ? number_ : fallback; }
    long long as_int(long long fallback = 0) const {
        return type_ == Type::Number ? static_cast<long long>(number_) : fallback;
    }
    const std::string& as_string() const {
        if (type_ != Type::String) throw std::runtime_error("Json: not a string");
        return string_;
    }
    std::string as_string_or(const std::string& fallback) const {
        return type_ == Type::String ? string_ : fallback;
    }

    const Array& as_array() const {
        if (type_ != Type::Array || !array_) throw std::runtime_error("Json: not an array");
        return *array_;
    }
    Array& as_array() {
        if (type_ != Type::Array || !array_) throw std::runtime_error("Json: not an array");
        return *array_;
    }
    const Object& as_object() const {
        if (type_ != Type::Object || !object_) throw std::runtime_error("Json: not an object");
        return *object_;
    }
    Object& as_object() {
        if (type_ != Type::Object || !object_) throw std::runtime_error("Json: not an object");
        return *object_;
    }

    bool contains(const std::string& key) const {
        return type_ == Type::Object && object_ && object_->count(key) != 0;
    }
    const Json& at(const std::string& key) const { return as_object().at(key); }
    const Json& at(std::size_t i) const { return as_array().at(i); }
    std::size_t size() const {
        if (type_ == Type::Array && array_) return array_->size();
        if (type_ == Type::Object && object_) return object_->size();
        if (type_ == Type::String) return string_.size();
        return 0;
    }
    bool empty() const { return size() == 0; }

    Json value(const std::string& key, const Json& fallback) const {
        if (type_ != Type::Object || !object_) return fallback;
        auto it = object_->find(key);
        return it == object_->end() ? fallback : it->second;
    }
    std::string str_or(const std::string& key, const std::string& fallback) const {
        if (type_ != Type::Object || !object_) return fallback;
        auto it = object_->find(key);
        if (it == object_->end() || !it->second.is_string()) return fallback;
        return it->second.string_;
    }

    Json& operator[](const std::string& key) {
        if (type_ != Type::Object) {
            type_ = Type::Object;
            object_ = std::make_shared<Object>();
        }
        return (*object_)[key];
    }

    bool operator==(const Json& o) const {
        if (type_ != o.type_) return false;
        switch (type_) {
            case Type::Null: return true;
            case Type::Bool: return bool_ == o.bool_;
            case Type::Number: return number_ == o.number_;
            case Type::String: return string_ == o.string_;
            case Type::Array:
                if (!array_ || !o.array_) return array_ == o.array_;
                return *array_ == *o.array_;
            case Type::Object:
                if (!object_ || !o.object_) return object_ == o.object_;
                return *object_ == *o.object_;
        }
        return false;
    }
    bool operator!=(const Json& o) const { return !(*this == o); }

    std::string dump() const {
        std::string out;
        dump_into(out);
        return out;
    }

    /// Pretty printer (2-space indent), mirroring serde_json::to_string_pretty.
    /// Used for pty_list output and the Deep-Freeze snapshot file.
    std::string dump_pretty() const {
        std::string out;
        dump_pretty_into(out, 0);
        return out;
    }

    static Json parse(const std::string& text) { return parse(std::string_view(text)); }
    static Json parse(const char* text) { return parse(std::string_view(text ? text : "")); }
    static Json parse(std::string_view text) {
        Parser p(text);
        Json v = p.parse_value();
        p.skip_ws();
        if (!p.eof()) throw JsonParseError("Json: trailing characters after value");
        return v;
    }

    /// Parse, falling back to a string Json on failure (used for tool-arg recovery).
    static Json parse_or_string(const std::string& text) {
        try {
            return parse(text);
        } catch (...) {
            return Json(text);
        }
    }

private:
    struct Parser {
        std::string_view s;
        std::size_t pos = 0;
        explicit Parser(std::string_view t) : s(t) {}
        bool eof() const { return pos >= s.size(); }
        void skip_ws() {
            while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t' || s[pos] == '\n' || s[pos] == '\r')) pos++;
        }
        char peek() const {
            if (pos >= s.size()) throw JsonParseError("Json: unexpected end of input");
            return s[pos];
        }
        void expect(char c) {
            if (pos >= s.size() || s[pos] != c) throw JsonParseError(std::string("Json: expected '") + c + "'");
            pos++;
        }
        void expect_lit(const char* lit) {
            for (const char* p = lit; *p; ++p) {
                if (pos >= s.size() || s[pos] != *p) throw JsonParseError("Json: invalid literal");
                pos++;
            }
        }
        Json parse_value() {
            skip_ws();
            char c = peek();
            if (c == '{') return parse_object();
            if (c == '[') return parse_array();
            if (c == '"') return Json(parse_string());
            if (c == 't') { expect_lit("true"); return Json(true); }
            if (c == 'f') { expect_lit("false"); return Json(false); }
            if (c == 'n') { expect_lit("null"); return Json(); }
            if (c == '-' || (c >= '0' && c <= '9')) return parse_number();
            throw JsonParseError("Json: unexpected character");
        }
        Json parse_object() {
            expect('{');
            Object o;
            skip_ws();
            if (!eof() && peek() == '}') { pos++; return Json(std::move(o)); }
            while (true) {
                skip_ws();
                if (eof() || peek() != '"') throw JsonParseError("Json: expected object key");
                std::string k = parse_string();
                skip_ws();
                expect(':');
                o.emplace(std::move(k), parse_value());
                skip_ws();
                if (eof()) throw JsonParseError("Json: unterminated object");
                char c = peek();
                if (c == ',') { pos++; continue; }
                if (c == '}') { pos++; break; }
                throw JsonParseError("Json: expected ',' or '}' in object");
            }
            return Json(std::move(o));
        }
        Json parse_array() {
            expect('[');
            Array a;
            skip_ws();
            if (!eof() && peek() == ']') { pos++; return Json(std::move(a)); }
            while (true) {
                a.push_back(parse_value());
                skip_ws();
                if (eof()) throw JsonParseError("Json: unterminated array");
                char c = peek();
                if (c == ',') { pos++; continue; }
                if (c == ']') { pos++; break; }
                throw JsonParseError("Json: expected ',' or ']' in array");
            }
            return Json(std::move(a));
        }
        std::string parse_string() {
            expect('"');
            std::string out;
            while (true) {
                if (pos >= s.size()) throw JsonParseError("Json: unterminated string");
                char c = s[pos++];
                if (c == '"') break;
                if (c == '\\') {
                    if (pos >= s.size()) throw JsonParseError("Json: bad escape");
                    char e = s[pos++];
                    switch (e) {
                        case '"': out += '"'; break;
                        case '\\': out += '\\'; break;
                        case '/': out += '/'; break;
                        case 'b': out += '\b'; break;
                        case 'f': out += '\f'; break;
                        case 'n': out += '\n'; break;
                        case 'r': out += '\r'; break;
                        case 't': out += '\t'; break;
                        case 'u': {
                            if (pos + 4 > s.size()) throw JsonParseError("Json: bad \\u escape");
                            unsigned cp = 0;
                            for (int i = 0; i < 4; i++) {
                                char h = s[pos++];
                                cp <<= 4;
                                if (h >= '0' && h <= '9') cp |= static_cast<unsigned>(h - '0');
                                else if (h >= 'a' && h <= 'f') cp |= static_cast<unsigned>(h - 'a' + 10);
                                else if (h >= 'A' && h <= 'F') cp |= static_cast<unsigned>(h - 'A' + 10);
                                else throw JsonParseError("Json: bad \\u escape");
                            }
                            // Encode as UTF-8 (no surrogate-pair combining: matches v0.1 needs).
                            if (cp < 0x80) {
                                out += static_cast<char>(cp);
                            } else if (cp < 0x800) {
                                out += static_cast<char>(0xC0 | (cp >> 6));
                                out += static_cast<char>(0x80 | (cp & 0x3F));
                            } else {
                                out += static_cast<char>(0xE0 | (cp >> 12));
                                out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                                out += static_cast<char>(0x80 | (cp & 0x3F));
                            }
                            break;
                        }
                        default: throw JsonParseError("Json: bad escape");
                    }
                } else {
                    out += c;
                }
            }
            return out;
        }
        Json parse_number() {
            std::size_t start = pos;
            if (!eof() && peek() == '-') pos++;
            while (pos < s.size() && std::isdigit(static_cast<unsigned char>(s[pos]))) pos++;
            if (pos < s.size() && s[pos] == '.') {
                pos++;
                while (pos < s.size() && std::isdigit(static_cast<unsigned char>(s[pos]))) pos++;
            }
            if (pos < s.size() && (s[pos] == 'e' || s[pos] == 'E')) {
                pos++;
                if (pos < s.size() && (s[pos] == '+' || s[pos] == '-')) pos++;
                while (pos < s.size() && std::isdigit(static_cast<unsigned char>(s[pos]))) pos++;
            }
            try {
                return Json(std::stod(std::string(s.substr(start, pos - start))));
            } catch (...) {
                throw JsonParseError("Json: bad number");
            }
        }
    };

    void dump_into(std::string& out) const {
        switch (type_) {
            case Type::Null: out += "null"; break;
            case Type::Bool: out += bool_ ? "true" : "false"; break;
            case Type::Number: {
                std::string n = std::to_string(number_);
                // Trim float formatting to a compact representation.
                if (n.find('.') != std::string::npos) {
                    while (!n.empty() && n.back() == '0') n.pop_back();
                    if (!n.empty() && n.back() == '.') n.pop_back();
                }
                if (n.empty()) n = "0";
                out += n;
                break;
            }
            case Type::String: dump_string(out, string_); break;
            case Type::Array:
                out += '[';
                if (array_) {
                    for (std::size_t i = 0; i < array_->size(); i++) {
                        if (i) out += ',';
                        (*array_)[i].dump_into(out);
                    }
                }
                out += ']';
                break;
            case Type::Object:
                out += '{';
                if (object_) {
                    bool first = true;
                    for (auto& kv : *object_) {
                        if (!first) out += ',';
                        first = false;
                        dump_string(out, kv.first);
                        out += ':';
                        kv.second.dump_into(out);
                    }
                }
                out += '}';
                break;
        }
    }
    void dump_pretty_into(std::string& out, int depth) const {
        std::string pad(static_cast<std::size_t>(depth) * 2, ' ');
        std::string pad1(static_cast<std::size_t>(depth + 1) * 2, ' ');
        switch (type_) {
            case Type::Null:
            case Type::Bool:
            case Type::Number:
            case Type::String: dump_into(out); break;
            case Type::Array: {
                if (!array_ || array_->empty()) {
                    out += "[]";
                    break;
                }
                out += "[\n";
                for (std::size_t i = 0; i < array_->size(); i++) {
                    out += pad1;
                    (*array_)[i].dump_pretty_into(out, depth + 1);
                    if (i + 1 < array_->size()) out += ',';
                    out += '\n';
                }
                out += pad;
                out += ']';
                break;
            }
            case Type::Object: {
                if (!object_ || object_->empty()) {
                    out += "{}";
                    break;
                }
                out += "{\n";
                std::size_t i = 0;
                for (auto& kv : *object_) {
                    out += pad1;
                    dump_string(out, kv.first);
                    out += ": ";
                    kv.second.dump_pretty_into(out, depth + 1);
                    if (++i < object_->size()) out += ',';
                    out += '\n';
                }
                out += pad;
                out += '}';
                break;
            }
        }
    }
    static void dump_string(std::string& out, const std::string& s) {
        out += '"';
        for (char c : s) {
            switch (c) {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\b': out += "\\b"; break;
                case '\f': out += "\\f"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20) {
                        char buf[7];
                        std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                        out += buf;
                    } else {
                        out += c;
                    }
            }
        }
        out += '"';
    }

    Type type_;
    bool bool_;
    double number_;
    std::string string_;
    std::shared_ptr<Array> array_;
    std::shared_ptr<Object> object_;
};

} // namespace marmel
