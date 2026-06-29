// json.cpp — small recursive-descent JSON parser + serialiser.
#include "json.h"

#include <cctype>
#include <cmath>
#include <cstdio>

namespace {

struct Parser {
    const char *p;
    const char *end;
    std::string err;

    void skip_ws() {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
            ++p;
    }

    bool fail(const char *msg) { if (err.empty()) err = msg; return false; }

    bool parse_value(JsonValue &v) {
        skip_ws();
        if (p >= end) return fail("unexpected end of input");
        switch (*p) {
            case '{': return parse_object(v);
            case '[': return parse_array(v);
            case '"': return parse_string_value(v);
            case 't': case 'f': return parse_bool(v);
            case 'n': return parse_null(v);
            default:  return parse_number(v);
        }
    }

    bool parse_object(JsonValue &v) {
        v.type = JsonValue::Object;
        ++p; // {
        skip_ws();
        if (p < end && *p == '}') { ++p; return true; }
        for (;;) {
            skip_ws();
            if (p >= end || *p != '"') return fail("expected string key");
            std::string key;
            if (!parse_string_raw(key)) return false;
            skip_ws();
            if (p >= end || *p != ':') return fail("expected ':'");
            ++p;
            JsonValue child;
            if (!parse_value(child)) return false;
            v.obj.emplace_back(std::move(key), std::move(child));
            skip_ws();
            if (p >= end) return fail("unterminated object");
            if (*p == ',') { ++p; continue; }
            if (*p == '}') { ++p; return true; }
            return fail("expected ',' or '}'");
        }
    }

    bool parse_array(JsonValue &v) {
        v.type = JsonValue::Array;
        ++p; // [
        skip_ws();
        if (p < end && *p == ']') { ++p; return true; }
        for (;;) {
            JsonValue child;
            if (!parse_value(child)) return false;
            v.arr.push_back(std::move(child));
            skip_ws();
            if (p >= end) return fail("unterminated array");
            if (*p == ',') { ++p; continue; }
            if (*p == ']') { ++p; return true; }
            return fail("expected ',' or ']'");
        }
    }

    bool parse_string_value(JsonValue &v) {
        v.type = JsonValue::String;
        return parse_string_raw(v.str);
    }

    bool parse_string_raw(std::string &out) {
        ++p; // opening quote
        out.clear();
        while (p < end && *p != '"') {
            char c = *p++;
            if (c == '\\') {
                if (p >= end) return fail("bad escape");
                char e = *p++;
                switch (e) {
                    case '"':  out += '"';  break;
                    case '\\': out += '\\'; break;
                    case '/':  out += '/';  break;
                    case 'b':  out += '\b'; break;
                    case 'f':  out += '\f'; break;
                    case 'n':  out += '\n'; break;
                    case 'r':  out += '\r'; break;
                    case 't':  out += '\t'; break;
                    case 'u': {
                        if (end - p < 4) return fail("bad \\u escape");
                        int cp = 0;
                        for (int i = 0; i < 4; i++) {
                            char h = *p++;
                            cp <<= 4;
                            if      (h >= '0' && h <= '9') cp |= h - '0';
                            else if (h >= 'a' && h <= 'f') cp |= h - 'a' + 10;
                            else if (h >= 'A' && h <= 'F') cp |= h - 'A' + 10;
                            else return fail("bad hex in \\u escape");
                        }
                        // Minimal UTF-8 encode (BMP only).
                        if (cp < 0x80) {
                            out += (char)cp;
                        } else if (cp < 0x800) {
                            out += (char)(0xC0 | (cp >> 6));
                            out += (char)(0x80 | (cp & 0x3F));
                        } else {
                            out += (char)(0xE0 | (cp >> 12));
                            out += (char)(0x80 | ((cp >> 6) & 0x3F));
                            out += (char)(0x80 | (cp & 0x3F));
                        }
                        break;
                    }
                    default: return fail("bad escape char");
                }
            } else {
                out += c;
            }
        }
        if (p >= end) return fail("unterminated string");
        ++p; // closing quote
        return true;
    }

    bool parse_number(JsonValue &v) {
        const char *start = p;
        if (p < end && (*p == '-' || *p == '+')) ++p;
        bool any = false;
        while (p < end && (std::isdigit((unsigned char)*p) || *p == '.' ||
                           *p == 'e' || *p == 'E' || *p == '+' || *p == '-')) {
            ++p; any = true;
        }
        if (!any) return fail("invalid number");
        v.type = JsonValue::Number;
        v.num  = strtod(std::string(start, p).c_str(), nullptr);
        return true;
    }

    bool parse_bool(JsonValue &v) {
        if (end - p >= 4 && std::string(p, p + 4) == "true")  { p += 4; v.type = JsonValue::Bool; v.b = true;  return true; }
        if (end - p >= 5 && std::string(p, p + 5) == "false") { p += 5; v.type = JsonValue::Bool; v.b = false; return true; }
        return fail("invalid literal");
    }

    bool parse_null(JsonValue &v) {
        if (end - p >= 4 && std::string(p, p + 4) == "null") { p += 4; v.type = JsonValue::Null; return true; }
        return fail("invalid literal");
    }
};

void dump_string(const std::string &s, std::string &out) {
    out += '"';
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out += c;      break;
        }
    }
    out += '"';
}

void dump_value(const JsonValue &v, std::string &out) {
    switch (v.type) {
        case JsonValue::Null:   out += "null"; break;
        case JsonValue::Bool:   out += v.b ? "true" : "false"; break;
        case JsonValue::Number: {
            char buf[32];
            if (v.num == std::floor(v.num) && std::fabs(v.num) < 1e15)
                snprintf(buf, sizeof buf, "%lld", (long long)v.num);
            else
                snprintf(buf, sizeof buf, "%g", v.num);
            out += buf;
            break;
        }
        case JsonValue::String: dump_string(v.str, out); break;
        case JsonValue::Array: {
            out += '[';
            for (size_t i = 0; i < v.arr.size(); i++) {
                if (i) out += ',';
                dump_value(v.arr[i], out);
            }
            out += ']';
            break;
        }
        case JsonValue::Object: {
            out += '{';
            for (size_t i = 0; i < v.obj.size(); i++) {
                if (i) out += ',';
                dump_string(v.obj[i].first, out);
                out += ':';
                dump_value(v.obj[i].second, out);
            }
            out += '}';
            break;
        }
    }
}

} // namespace

bool json_parse(const std::string &text, JsonValue &out, std::string &err) {
    Parser ps{text.c_str(), text.c_str() + text.size(), {}};
    if (!ps.parse_value(out)) { err = ps.err; return false; }
    ps.skip_ws();
    if (ps.p != ps.end) { err = "trailing characters after JSON value"; return false; }
    return true;
}

std::string json_dump(const JsonValue &v) {
    std::string out;
    dump_value(v, out);
    return out;
}
