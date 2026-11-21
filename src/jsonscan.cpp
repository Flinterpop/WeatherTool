#include "jsonscan.h"

#include <windows.h>

#include <cassert>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace wt::json {
namespace {

bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

void SkipSpace(std::string_view j, size_t& pos) {
    while (pos < j.size() && IsSpace(j[pos])) { ++pos; }
}

int HexVal(char c) {
    if (c >= '0' && c <= '9') { return c - '0'; }
    if (c >= 'a' && c <= 'f') { return c - 'a' + 10; }
    if (c >= 'A' && c <= 'F') { return c - 'A' + 10; }
    return -1;
}

void AppendUtf8(std::string& s, unsigned cp) {
    if (cp < 0x80) {
        s += static_cast<char>(cp);
    } else if (cp < 0x800) {
        s += static_cast<char>(0xC0 | (cp >> 6));
        s += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        s += static_cast<char>(0xE0 | (cp >> 12));
        s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        s += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        s += static_cast<char>(0xF0 | (cp >> 18));
        s += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        s += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

bool ReadHex4(std::string_view j, size_t pos, unsigned& out) {
    if (pos + 4 > j.size()) { return false; }
    out = 0;
    for (size_t k = 0; k < 4; ++k) {
        const int h = HexVal(j[pos + k]);
        if (h < 0) { return false; }
        out = out * 16u + static_cast<unsigned>(h);
    }
    return true;
}

std::wstring Widen(const std::string& utf8) {
    if (utf8.empty()) { return std::wstring(); }
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (n <= 0) { return std::wstring(); }
    std::wstring w(static_cast<size_t>(n), L'\0');
    const int m = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), w.data(), n);
    (void)m;
    assert(m == n);
    return w;
}

} // namespace

size_t FindKey(std::string_view j, std::string_view key, size_t from, size_t to) {
    assert(!key.empty());
    to = (to > j.size()) ? j.size() : to;
    size_t at = from;
    for (int guard = 0; guard < 1'000'000 && at < to; ++guard) {
        const size_t q = j.find(key, at);
        if (q == npos || q + key.size() >= to) { return npos; }
        at = q + 1;
        // Must be a whole quoted key: "key" then optional space and a colon.
        if (q == 0 || j[q - 1] != '"' || j[q + key.size()] != '"') { continue; }
        if (q >= 2 && j[q - 2] == '\\') { continue; }
        size_t p = q + key.size() + 1;
        SkipSpace(j, p);
        if (p >= to || j[p] != ':') { continue; }
        ++p;
        SkipSpace(j, p);
        return p < to ? p : npos;
    }
    return npos;
}

bool ReadNumber(std::string_view j, size_t& pos, double& out) {
    SkipSpace(j, pos);
    if (pos >= j.size()) { return false; }
    if (j.substr(pos, 4) == "null") {
        out = std::numeric_limits<double>::quiet_NaN();
        pos += 4;
        return true;
    }
    size_t end = pos;
    while (end < j.size() && end - pos < 40) {
        const char c = j[end];
        const bool numChar = (c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E';
        if (!numChar) { break; }
        ++end;
    }
    if (end == pos || end - pos >= 40) { return false; }
    char buf[48] = {};
    j.copy(buf, end - pos, pos);
    char* stop = nullptr;
    const double v = std::strtod(buf, &stop);
    if (stop != buf + (end - pos)) { return false; }
    out = v;
    pos = end;
    return true;
}

bool ReadBool(std::string_view j, size_t& pos, bool& out) {
    SkipSpace(j, pos);
    if (j.substr(pos, 4) == "true")  { out = true;  pos += 4; return true; }
    if (j.substr(pos, 5) == "false") { out = false; pos += 5; return true; }
    return false;
}

bool ReadString(std::string_view j, size_t& pos, std::wstring& out) {
    SkipSpace(j, pos);
    if (pos >= j.size() || j[pos] != '"') { return false; }
    std::string bytes;
    size_t p = pos + 1;
    constexpr size_t kMaxLen = 4096;
    while (p < j.size() && bytes.size() < kMaxLen) {
        const char c = j[p];
        if (c == '"') {
            out = Widen(bytes);
            pos = p + 1;
            return true;
        }
        if (c != '\\') { bytes += c; ++p; continue; }
        if (p + 1 >= j.size()) { return false; }
        const char e = j[p + 1];
        p += 2;
        switch (e) {
        case 'n': bytes += '\n'; break;
        case 't': bytes += '\t'; break;
        case 'r': bytes += '\r'; break;
        case 'b': bytes += '\b'; break;
        case 'f': bytes += '\f'; break;
        case 'u': {
            unsigned cp = 0;
            if (!ReadHex4(j, p, cp)) { return false; }
            p += 4;
            unsigned lo = 0;
            const bool pair = cp >= 0xD800 && cp < 0xDC00 && p + 6 <= j.size() && j[p] == '\\' && j[p + 1] == 'u' &&
                              ReadHex4(j, p + 2, lo) && lo >= 0xDC00 && lo < 0xE000;
            if (pair) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                p += 6;
            }
            AppendUtf8(bytes, cp);
            break;
        }
        default: bytes += e; break;   // \" \\ \/
        }
    }
    return false;
}

size_t SkipContainer(std::string_view j, size_t pos) {
    if (pos >= j.size() || (j[pos] != '{' && j[pos] != '[')) { return npos; }
    int depth = 0;
    bool inStr = false;
    for (size_t p = pos; p < j.size(); ++p) {
        const char c = j[p];
        if (inStr) {
            if (c == '\\') { ++p; }
            else if (c == '"') { inStr = false; }
            continue;
        }
        if (c == '"') { inStr = true; }
        else if (c == '{' || c == '[') { ++depth; }
        else if (c == '}' || c == ']') {
            --depth;
            if (depth == 0) { return p + 1; }
        }
    }
    return npos;
}

ArrayReader::ArrayReader(std::string_view j, size_t pos) : j_(j), pos_(pos) {
    SkipSpace(j_, pos_);
    if (pos_ >= j_.size() || j_[pos_] != '[') {
        ok_   = false;
        done_ = true;
        return;
    }
    ++pos_;
}

bool ArrayReader::Advance() {
    if (done_) { return false; }
    SkipSpace(j_, pos_);
    if (pos_ >= j_.size()) { ok_ = false; done_ = true; return false; }
    if (j_[pos_] == ']') { ++pos_; done_ = true; return false; }
    if (!first_) {
        if (j_[pos_] != ',') { ok_ = false; done_ = true; return false; }
        ++pos_;
        SkipSpace(j_, pos_);
    }
    first_ = false;
    return true;
}

bool ArrayReader::NextNumber(double& out) {
    if (!Advance()) { return false; }
    if (!ReadNumber(j_, pos_, out)) { ok_ = false; done_ = true; return false; }
    return true;
}

bool ArrayReader::NextString(std::wstring& out) {
    if (!Advance()) { return false; }
    if (!ReadString(j_, pos_, out)) { ok_ = false; done_ = true; return false; }
    return true;
}

bool ArrayReader::NextElement(size_t& elementPos) {
    if (!Advance()) { return false; }
    elementPos = pos_;
    const char c = j_[pos_];
    if (c == '{' || c == '[') {
        const size_t end = SkipContainer(j_, pos_);
        if (end == npos) { ok_ = false; done_ = true; return false; }
        pos_ = end;
        return true;
    }
    // Scalar: step over it to the next ',' or ']' outside a string.
    bool inStr = false;
    for (; pos_ < j_.size(); ++pos_) {
        const char d = j_[pos_];
        if (inStr) {
            if (d == '\\') { ++pos_; }
            else if (d == '"') { inStr = false; }
            continue;
        }
        if (d == '"') { inStr = true; continue; }
        if (d == ',' || d == ']') { return true; }
    }
    ok_ = false;
    done_ = true;
    return false;
}

} // namespace wt::json
