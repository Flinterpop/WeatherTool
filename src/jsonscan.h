// A small, allocation-light JSON scanner for the two Open-Meteo replies the
// app reads. It is not a general parser: it finds a key, then reads the
// scalar, string or flat array that follows. No recursion.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace wt::json {

constexpr size_t npos = std::string_view::npos;

// Offset of the value that follows `"key":` in [from, to), or npos.
size_t FindKey(std::string_view j, std::string_view key, size_t from, size_t to);

// Reads a number (or `null`, giving NaN) at `pos`, skipping leading
// whitespace; advances `pos` past it. False on anything else.
bool ReadNumber(std::string_view j, size_t& pos, double& out);

// Reads a string at `pos` (leading whitespace skipped), decoding escapes
// and UTF-8 into `out`; advances `pos` past the closing quote. False for a
// string longer than `maxBytes` (UTF-8, after unescaping).
bool ReadString(std::string_view j, size_t& pos, std::wstring& out, size_t maxBytes = 4096);

// Reads `true` / `false` at `pos`.
bool ReadBool(std::string_view j, size_t& pos, bool& out);

// Offset one past the end of the object or array that starts at `pos`
// (which must be `{` or `[`), or npos when it is unterminated.
size_t SkipContainer(std::string_view j, size_t pos);

// Walks a flat array element by element:
//   ArrayReader r(j, pos); double v; while (r.NextNumber(v)) { ... }
// `ok()` is false afterwards when the array was malformed.
class ArrayReader {
public:
    ArrayReader(std::string_view j, size_t pos);
    bool NextNumber(double& out);
    bool NextString(std::wstring& out);
    // Positions at the next element without reading it; false at the end.
    bool NextElement(size_t& elementPos);
    bool ok() const { return ok_; }
    size_t pos() const { return pos_; }

private:
    bool Advance();   // past ',' or onto ']'; false at the end
    std::string_view j_;
    size_t pos_   = 0;
    bool   ok_    = true;
    bool   first_ = true;
    bool   done_  = false;
};

} // namespace wt::json
