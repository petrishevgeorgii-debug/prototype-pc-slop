// SPDX-License-Identifier: GPL-2.0-or-later
// A small JSON value: enough for the launcher's messages (UTF-8 strings, numbers, nested data).
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

struct Json {
    enum Kind { Null, Bool, Number, String, Array, Object } kind = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Json> a;
    std::vector<std::pair<std::string, Json>> o;  // keeps the order

    Json() = default;
    Json(bool v) : kind(Bool), b(v) {}
    Json(int v) : kind(Number), n(v) {}
    Json(double v) : kind(Number), n(v) {}
    Json(const char *v) : kind(String), s(v) {}
    Json(std::string v) : kind(String), s(std::move(v)) {}
    static Json object() { Json j; j.kind = Object; return j; }
    static Json array() { Json j; j.kind = Array; return j; }

    bool is_null() const { return kind == Null; }
    bool truthy() const {
        switch (kind) {
        case Bool: return b;
        case Number: return n != 0;
        case String: return !s.empty();
        case Array: case Object: return true;
        default: return false;
        }
    }
    double num(double fallback = 0) const { return kind == Number ? n : kind == Bool ? (b ? 1 : 0) : fallback; }
    int integer(int fallback = 0) const { return kind == Number ? (int)n : fallback; }
    const std::string &str() const { static const std::string empty; return kind == String ? s : empty; }
    size_t size() const { return kind == Array ? a.size() : kind == Object ? o.size() : 0; }

    const Json &operator[](const char *key) const {
        static const Json none;
        if (kind != Object) return none;
        for (auto &kv : o)
            if (kv.first == key) return kv.second;
        return none;
    }
    const Json &operator[](const std::string &key) const { return (*this)[key.c_str()]; }
    const Json &operator[](size_t i) const {
        static const Json none;
        return kind == Array && i < a.size() ? a[i] : none;
    }
    const Json &operator[](int i) const { return (*this)[(size_t)i]; }
    Json &set(const std::string &key, Json value) {
        kind = Object;
        for (auto &kv : o)
            if (kv.first == key) { kv.second = std::move(value); return kv.second; }
        o.emplace_back(key, std::move(value));
        return o.back().second;
    }
    void push(Json value) { kind = Array; a.push_back(std::move(value)); }
    bool contains_string(const std::string &v) const {
        for (auto &x : a)
            if (x.kind == String && x.s == v) return true;
        return false;
    }
    // A dotted path: "rows.res".
    const Json &path(const char *dotted) const {
        const Json *j = this;
        std::string part;
        for (const char *p = dotted;; ++p) {
            if (*p == '.' || !*p) {
                j = &(*j)[part];
                part.clear();
                if (!*p) break;
            } else part += *p;
        }
        return *j;
    }

    // ---- text
    static void put_string(std::string &out, const std::string &v) {
        out += '"';
        for (unsigned char c : v) {
            switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) { char buf[8]; snprintf(buf, sizeof buf, "\\u%04x", c); out += buf; }
                else out += (char)c;
            }
        }
        out += '"';
    }
    void dump(std::string &out) const {
        switch (kind) {
        case Null: out += "null"; break;
        case Bool: out += b ? "true" : "false"; break;
        case Number: {
            char buf[32];
            if (n == (double)(int64_t)n && n < 1e15 && n > -1e15) snprintf(buf, sizeof buf, "%lld", (long long)n);
            else snprintf(buf, sizeof buf, "%.17g", n);
            out += buf;
            break;
        }
        case String: put_string(out, s); break;
        case Array:
            out += '[';
            for (size_t i = 0; i < a.size(); ++i) { if (i) out += ','; a[i].dump(out); }
            out += ']';
            break;
        case Object:
            out += '{';
            for (size_t i = 0; i < o.size(); ++i) {
                if (i) out += ',';
                put_string(out, o[i].first);
                out += ':';
                o[i].second.dump(out);
            }
            out += '}';
            break;
        }
    }
    std::string dump() const { std::string out; dump(out); return out; }

    static Json parse(const std::string &text, bool *ok = nullptr) {
        const char *p = text.c_str(), *end = p + text.size();
        Json j;
        bool good = parse_value(p, end, j, 0);
        skip(p, end);
        if (ok) *ok = good && p == end;
        return good ? j : Json();
    }

private:
    static void skip(const char *&p, const char *end) {
        while (p < end && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) ++p;
    }
    static void put_utf8(std::string &out, uint32_t c) {
        if (c < 0x80) out += (char)c;
        else if (c < 0x800) { out += (char)(0xc0 | c >> 6); out += (char)(0x80 | (c & 0x3f)); }
        else if (c < 0x10000) { out += (char)(0xe0 | c >> 12); out += (char)(0x80 | (c >> 6 & 0x3f)); out += (char)(0x80 | (c & 0x3f)); }
        else { out += (char)(0xf0 | c >> 18); out += (char)(0x80 | (c >> 12 & 0x3f)); out += (char)(0x80 | (c >> 6 & 0x3f)); out += (char)(0x80 | (c & 0x3f)); }
    }
    static bool hex4(const char *&p, const char *end, uint32_t &v) {
        if (end - p < 4) return false;
        v = 0;
        for (int i = 0; i < 4; ++i, ++p) {
            char c = *p;
            v <<= 4;
            if (c >= '0' && c <= '9') v |= c - '0';
            else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
            else return false;
        }
        return true;
    }
    static bool parse_string(const char *&p, const char *end, std::string &out) {
        if (p >= end || *p != '"') return false;
        ++p;
        while (p < end && *p != '"') {
            if (*p == '\\') {
                if (++p >= end) return false;
                char e = *p++;
                switch (e) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'u': {
                    uint32_t c;
                    if (!hex4(p, end, c)) return false;
                    if (c >= 0xd800 && c < 0xdc00 && end - p >= 6 && p[0] == '\\' && p[1] == 'u') {
                        p += 2;
                        uint32_t lo;
                        if (!hex4(p, end, lo)) return false;
                        c = 0x10000 + ((c - 0xd800) << 10) + (lo - 0xdc00);
                    }
                    put_utf8(out, c);
                    break;
                }
                default: out += e;
                }
            } else out += *p++;
        }
        if (p >= end) return false;
        ++p;
        return true;
    }
    static bool parse_value(const char *&p, const char *end, Json &j, int depth) {
        if (depth > 64) return false;
        skip(p, end);
        if (p >= end) return false;
        char c = *p;
        if (c == '{') {
            j.kind = Object;
            ++p;
            skip(p, end);
            if (p < end && *p == '}') { ++p; return true; }
            for (;;) {
                skip(p, end);
                std::string key;
                if (!parse_string(p, end, key)) return false;
                skip(p, end);
                if (p >= end || *p != ':') return false;
                ++p;
                Json v;
                if (!parse_value(p, end, v, depth + 1)) return false;
                j.o.emplace_back(std::move(key), std::move(v));
                skip(p, end);
                if (p < end && *p == ',') { ++p; continue; }
                if (p < end && *p == '}') { ++p; return true; }
                return false;
            }
        }
        if (c == '[') {
            j.kind = Array;
            ++p;
            skip(p, end);
            if (p < end && *p == ']') { ++p; return true; }
            for (;;) {
                Json v;
                if (!parse_value(p, end, v, depth + 1)) return false;
                j.a.push_back(std::move(v));
                skip(p, end);
                if (p < end && *p == ',') { ++p; continue; }
                if (p < end && *p == ']') { ++p; return true; }
                return false;
            }
        }
        if (c == '"') { j.kind = String; return parse_string(p, end, j.s); }
        if (end - p >= 4 && !strncmp(p, "true", 4)) { j = Json(true); p += 4; return true; }
        if (end - p >= 5 && !strncmp(p, "false", 5)) { j = Json(false); p += 5; return true; }
        if (end - p >= 4 && !strncmp(p, "null", 4)) { j = Json(); p += 4; return true; }
        char *stop = nullptr;
        double v = strtod(p, &stop);
        if (stop == p) return false;
        j = Json(v);
        p = stop;
        return true;
    }
};
