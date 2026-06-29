// json.h — tiny, dependency-free JSON parser used by config + control API.
// Supports objects, arrays, strings, numbers, bools, null. Good enough for our
// schema and the smplOS settings-app integration; not a full RFC-8259 validator.
#pragma once

#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

struct JsonValue {
    enum Type { Null, Bool, Number, String, Array, Object } type = Null;

    bool        b   = false;
    double      num = 0.0;
    std::string str;
    std::vector<JsonValue>                          arr;
    std::vector<std::pair<std::string, JsonValue>>  obj;

    const JsonValue *find(const std::string &key) const {
        if (type != Object) return nullptr;
        for (const auto &kv : obj)
            if (kv.first == key) return &kv.second;
        return nullptr;
    }

    double      num_or(double d)               const { return type == Number ? num : d; }
    int         int_or(int d)                  const { return type == Number ? (int)num : d; }
    bool        bool_or(bool d)                const { return type == Bool   ? b   : d; }
    std::string str_or(const std::string &d)   const { return type == String ? str : d; }
};

// Parse `text` into `out`. Returns false and fills `err` on syntax error.
bool json_parse(const std::string &text, JsonValue &out, std::string &err);

// Serialise `v` to compact JSON.
std::string json_dump(const JsonValue &v);

// ─── Builder helpers (for assembling control-API responses) ───────────────────
inline JsonValue jnum(double d)  { JsonValue v; v.type = JsonValue::Number; v.num = d; return v; }
inline JsonValue jstr(std::string s) { JsonValue v; v.type = JsonValue::String; v.str = std::move(s); return v; }
inline JsonValue jbool(bool b)   { JsonValue v; v.type = JsonValue::Bool; v.b = b; return v; }
inline JsonValue jarr()          { JsonValue v; v.type = JsonValue::Array; return v; }
inline JsonValue jobj(std::initializer_list<std::pair<std::string, JsonValue>> kv) {
    JsonValue v; v.type = JsonValue::Object;
    for (const auto &p : kv) v.obj.push_back(p);
    return v;
}
