/**
 * AirGradient Go — go_text_wrap host tests
 *
 * Drives compute_wrapped_lines() with a deterministic fixed6 width
 * function so wrap math is unit-tested without a real u8g2 context.
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <string>

#include "go_text_wrap.h"

namespace {

// Fixed-width font stub: every character is 6 px wide.
int fixed6(const char *, size_t len, void *) { return static_cast<int>(len) * 6; }

std::string slice(const WrapLine &line) {
  return std::string(line.begin, line.begin + line.length);
}

constexpr size_t MAX_OUT = 16;

} // namespace

TEST_CASE("go_text_wrap: single short word fits on one line", "[go_text_wrap]") {
  WrapLine out[MAX_OUT];
  const char *text = "hello";
  const size_t n = compute_wrapped_lines(text, 60, fixed6, nullptr, out, MAX_OUT);
  REQUIRE(n == 1);
  REQUIRE(slice(out[0]) == "hello");
}

TEST_CASE("go_text_wrap: two words wrap at trigger space", "[go_text_wrap]") {
  WrapLine out[MAX_OUT];
  // "hello world" = 11 chars * 6 = 66 px > 60
  const size_t n = compute_wrapped_lines("hello world", 60, fixed6, nullptr, out, MAX_OUT);
  REQUIRE(n == 2);
  REQUIRE(slice(out[0]) == "hello");
  REQUIRE(slice(out[1]) == "world");
}

TEST_CASE("go_text_wrap: three-word input wraps at last fitting boundary", "[go_text_wrap]") {
  WrapLine out[MAX_OUT];
  // "foo bar baz" = 11 chars * 6 = 66 px. With width 50: "foo bar" = 42 fits, +" baz" -> 66.
  const size_t n = compute_wrapped_lines("foo bar baz", 50, fixed6, nullptr, out, MAX_OUT);
  REQUIRE(n == 2);
  REQUIRE(slice(out[0]) == "foo bar");
  REQUIRE(slice(out[1]) == "baz");
}

TEST_CASE("go_text_wrap: overlong word hard-breaks", "[go_text_wrap]") {
  WrapLine out[MAX_OUT];
  // max=36 → fits 6 chars per line.
  const size_t n = compute_wrapped_lines("abcdefghij", 36, fixed6, nullptr, out, MAX_OUT);
  REQUIRE(n == 2);
  REQUIRE(slice(out[0]) == "abcdef");
  REQUIRE(slice(out[1]) == "ghij");
}

TEST_CASE("go_text_wrap: overlong single character makes forward progress", "[go_text_wrap]") {
  WrapLine out[MAX_OUT];
  // max=3 px < single char width (6) — emit one char per line.
  const size_t n = compute_wrapped_lines("ABC", 3, fixed6, nullptr, out, MAX_OUT);
  REQUIRE(n == 3);
  REQUIRE(slice(out[0]) == "A");
  REQUIRE(slice(out[1]) == "B");
  REQUIRE(slice(out[2]) == "C");
}

TEST_CASE("go_text_wrap: explicit newline forces line break", "[go_text_wrap]") {
  WrapLine out[MAX_OUT];
  const size_t n = compute_wrapped_lines("hi\nthere", 600, fixed6, nullptr, out, MAX_OUT);
  REQUIRE(n == 2);
  REQUIRE(slice(out[0]) == "hi");
  REQUIRE(slice(out[1]) == "there");
}

TEST_CASE("go_text_wrap: mixed newline plus auto-wrap", "[go_text_wrap]") {
  WrapLine out[MAX_OUT];
  const size_t n = compute_wrapped_lines("hello world\nfoo", 60, fixed6, nullptr, out, MAX_OUT);
  REQUIRE(n == 3);
  REQUIRE(slice(out[0]) == "hello");
  REQUIRE(slice(out[1]) == "world");
  REQUIRE(slice(out[2]) == "foo");
}

TEST_CASE("go_text_wrap: multi-space run collapses at wrap point", "[go_text_wrap]") {
  WrapLine out[MAX_OUT];
  // "hello   world" — 13 chars * 6 = 78 px > 60.
  const size_t n = compute_wrapped_lines("hello   world", 60, fixed6, nullptr, out, MAX_OUT);
  REQUIRE(n == 2);
  REQUIRE(slice(out[0]) == "hello");
  REQUIRE(slice(out[1]) == "world");
}

TEST_CASE("go_text_wrap: narrow width wraps every word", "[go_text_wrap]") {
  WrapLine out[MAX_OUT];
  // max=10 — fits 1 char per line (6 px) but not "a b" (3*6=18).
  const size_t n = compute_wrapped_lines("a b c d e f", 10, fixed6, nullptr, out, MAX_OUT);
  REQUIRE(n == 6);
  for (size_t i = 0; i < n; ++i) {
    REQUIRE(out[i].length == 1);
  }
  REQUIRE(slice(out[0]) == "a");
  REQUIRE(slice(out[5]) == "f");
}

TEST_CASE("go_text_wrap: leading whitespace at input start is preserved", "[go_text_wrap]") {
  WrapLine out[MAX_OUT];
  const size_t n = compute_wrapped_lines("  hi", 60, fixed6, nullptr, out, MAX_OUT);
  REQUIRE(n == 1);
  REQUIRE(slice(out[0]) == "  hi");
}

TEST_CASE("go_text_wrap: leading whitespace after explicit newline is preserved",
          "[go_text_wrap]") {
  WrapLine out[MAX_OUT];
  const size_t n = compute_wrapped_lines("hi\n  there", 600, fixed6, nullptr, out, MAX_OUT);
  REQUIRE(n == 2);
  REQUIRE(slice(out[0]) == "hi");
  REQUIRE(slice(out[1]) == "  there");
}

TEST_CASE("go_text_wrap: internal double space preserved when line fits", "[go_text_wrap]") {
  WrapLine out[MAX_OUT];
  // "foo  bar" = 8 chars * 6 = 48 px <= 60.
  const size_t n = compute_wrapped_lines("foo  bar", 60, fixed6, nullptr, out, MAX_OUT);
  REQUIRE(n == 1);
  REQUIRE(slice(out[0]) == "foo  bar");
}

TEST_CASE("go_text_wrap: empty input returns zero lines", "[go_text_wrap]") {
  WrapLine out[MAX_OUT];
  REQUIRE(compute_wrapped_lines("", 60, fixed6, nullptr, out, MAX_OUT) == 0);
}

TEST_CASE("go_text_wrap: null input returns zero lines", "[go_text_wrap]") {
  WrapLine out[MAX_OUT];
  REQUIRE(compute_wrapped_lines(nullptr, 60, fixed6, nullptr, out, MAX_OUT) == 0);
}

TEST_CASE("go_text_wrap: respects out_cap and drops excess lines", "[go_text_wrap]") {
  WrapLine out[2];
  // 4 wrapped lines requested; only 2 slots.
  const size_t n = compute_wrapped_lines("a b c d", 10, fixed6, nullptr, out, 2);
  REQUIRE(n == 2);
  REQUIRE(slice(out[0]) == "a");
  REQUIRE(slice(out[1]) == "b");
}
