/**
 * AirGradient Go — Text wrap helper
 *
 * Pure, host-testable word-wrap math used by the Info screen renderer.
 * Has no u8g2 dependency; the caller injects a pixel-width function so the
 * helper can be exercised on host without a real rendering context.
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef GO_TEXT_WRAP_H
#define GO_TEXT_WRAP_H

#include <cstddef>

/// One wrapped output line.  Points into the caller's source string and
/// carries an explicit byte length (the source is not modified or
/// re-emitted, so the slice is not NUL-terminated).
struct WrapLine {
  const char *begin = nullptr; ///< Pointer into the caller's source string.
  size_t length = 0;           ///< Number of bytes from `begin`.
};

/// Pixel-width function injected by the caller.  Returns the rendered
/// pixel width of [text, text+len) for the caller's font context.  The
/// host test passes a deterministic stub (e.g. fixed per-character width)
/// and the display-side caller wraps u8g2_GetStrWidth.
using StrWidthFn = int (*)(const char *text, size_t len, void *ctx);

/// Wrap `text` (ASCII; may contain '\n') into output lines that each fit
/// in `max_width_px` when measured by `width_fn`.
///
/// Splits on '\n' for hard breaks, then word-wraps each paragraph at the
/// last space boundary that fits.  Words wider than `max_width_px`
/// hard-break at the last character whose cumulative width fits and
/// continue on the next line.
///
/// Degenerate overlong-single-character: if even one character measures
/// wider than `max_width_px`, the line emits exactly that one character
/// and the next line continues from the next character.  This guarantees
/// forward progress and avoids infinite loops on pathological inputs.
///
/// Whitespace handling at wrap boundaries:
///  - The space that triggers a wrap is consumed (not on either line).
///  - Multiple consecutive spaces at a wrap point collapse to a single
///    break: any leading whitespace introduced by an auto-wrap is
///    skipped.
///  - Spaces inside a line that does not wrap are preserved.
///  - Explicit '\n' is consumed at the break.
///  - Leading whitespace at the very start of the input (or the start of
///    a paragraph after '\n') is preserved on the first line.
///
/// Returns the number of lines written, capped at `out_cap`.  Excess
/// lines are dropped without writing past the buffer.  Null or empty
/// `text` returns 0.
size_t compute_wrapped_lines(const char *text, int max_width_px, StrWidthFn width_fn,
                             void *width_ctx, WrapLine *out, size_t out_cap);

#endif // GO_TEXT_WRAP_H
