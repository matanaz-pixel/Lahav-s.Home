#pragma once
#include <Arduino.h>
#include <vector>

// ---------------------------------------------------------------------------
// rtlVisual(): converts a LOGICAL UTF-8 string into VISUAL order so a plain
// left-to-right glyph renderer shows Hebrew correctly.
//  - Hebrew runs are reversed (character order) and run order is reversed
//  - Latin letters / digits stay LTR ("Wi-Fi", "12:30", "75 dB", "4.5g")
//  - Neutral chars (space, punctuation) between two LTR chars join the LTR run,
//    otherwise they join the RTL run (base direction is RTL)
//  - "%" and degree sign stick to the preceding number, a "-"/"+" sticks to a following digit
//  - Brackets are mirrored inside RTL runs
// Call it per LINE (after word-wrapping the logical text).
// ---------------------------------------------------------------------------
namespace rtl {

inline bool isHeb(uint32_t c) { return c >= 0x0590 && c <= 0x05FF; }

inline bool hasHebrew(const char* s) {
  for (; *s; ++s) if ((uint8_t)*s == 0xD6 || (uint8_t)*s == 0xD7) return true;  // U+0580..U+05FF lead bytes
  return false;
}

inline void decode(const char* s, std::vector<uint32_t>& out) {
  const uint8_t* p = (const uint8_t*)s;
  while (*p) {
    uint32_t c; int n;
    if (*p < 0x80) { c = *p; n = 1; }
    else if ((*p & 0xE0) == 0xC0) { c = *p & 0x1F; n = 2; }
    else if ((*p & 0xF0) == 0xE0) { c = *p & 0x0F; n = 3; }
    else if ((*p & 0xF8) == 0xF0) { c = *p & 0x07; n = 4; }
    else { ++p; continue; }
    ++p;
    for (int i = 1; i < n; i++) {
      if ((*p & 0xC0) != 0x80) break;
      c = (c << 6) | (*p & 0x3F);
      ++p;
    }
    out.push_back(c);
  }
}

inline void encode(uint32_t c, String& s) {
  if (c < 0x80) s += (char)c;
  else if (c < 0x800) { s += (char)(0xC0 | (c >> 6)); s += (char)(0x80 | (c & 0x3F)); }
  else if (c < 0x10000) { s += (char)(0xE0 | (c >> 12)); s += (char)(0x80 | ((c >> 6) & 0x3F)); s += (char)(0x80 | (c & 0x3F)); }
  else { s += (char)(0xF0 | (c >> 18)); s += (char)(0x80 | ((c >> 12) & 0x3F)); s += (char)(0x80 | ((c >> 6) & 0x3F)); s += (char)(0x80 | (c & 0x3F)); }
}

inline uint32_t mirror(uint32_t c) {
  switch (c) {
    case '(': return ')'; case ')': return '(';
    case '[': return ']'; case ']': return '[';
    case '{': return '}'; case '}': return '{';
    case '<': return '>'; case '>': return '<';
    default: return c;
  }
}

}  // namespace rtl

inline String rtlVisual(const String& logical) {
  using namespace rtl;
  std::vector<uint32_t> cp;
  cp.reserve(logical.length());
  decode(logical.c_str(), cp);
  const int n = (int)cp.size();
  if (n == 0) return String();

  enum : int8_t { N = 0, H = 1, L = 2 };
  std::vector<int8_t> cls(n, N);
  for (int i = 0; i < n; i++) {
    uint32_t c = cp[i];
    if (isHeb(c)) cls[i] = H;
    else if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= 0xC0 && c != 0xD7 && !isHeb(c) && c < 0x2000)) cls[i] = L;
  }
  // sticky neutrals
  for (int i = 0; i < n; i++) {
    uint32_t c = cp[i];
    if (cls[i] != N) continue;
    if ((c == '%' || c == 0xB0) && i > 0 && cls[i - 1] == L) cls[i] = L;
    else if ((c == '-' || c == '+') && i + 1 < n && cp[i + 1] >= '0' && cp[i + 1] <= '9' && (i == 0 || cls[i - 1] != L)) cls[i] = L;
  }
  // resolve neutral runs
  for (int i = 0; i < n;) {
    if (cls[i] != N) { i++; continue; }
    int j = i;
    while (j < n && cls[j] == N) j++;
    int8_t left = (i > 0) ? cls[i - 1] : H;
    int8_t right = (j < n) ? cls[j] : H;
    int8_t r = (left == L && right == L) ? L : H;
    for (int k = i; k < j; k++) cls[k] = r;
    i = j;
  }
  // runs
  struct Run { int a, b; int8_t c; };
  std::vector<Run> runs;
  for (int i = 0; i < n;) {
    int j = i;
    while (j < n && cls[j] == cls[i]) j++;
    runs.push_back({i, j, cls[i]});
    i = j;
  }
  String out;
  out.reserve(logical.length() + 4);
  for (int r = (int)runs.size() - 1; r >= 0; r--) {
    const Run& run = runs[r];
    if (run.c == H) {
      for (int k = run.b - 1; k >= run.a; k--) encode(mirror(cp[k]), out);
    } else {
      for (int k = run.a; k < run.b; k++) encode(cp[k], out);
    }
  }
  return out;
}
