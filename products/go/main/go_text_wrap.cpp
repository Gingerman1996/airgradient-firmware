/**
 * AirGradient Go — Text wrap helper implementation
 *
 * Pure host-testable word-wrap math.  Splits on '\n' for hard breaks and
 * word-wraps each paragraph at space boundaries.  See go_text_wrap.h for the
 * contract.
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "go_text_wrap.h"

namespace {

/// Hard-break a paragraph atom that does not fit on an empty line.  Finds
/// the longest prefix [begin, begin+k) that fits in `max_width_px`,
/// guaranteeing at least one character of forward progress so a single
/// overlong character cannot infinite-loop.
size_t longest_fitting_prefix(const char *begin, size_t max_len, int max_width_px,
                              StrWidthFn width_fn, void *ctx) {
  // Forward-progress: always emit at least one character even when even
  // that one character exceeds max_width_px.
  size_t k = 1;
  while (k < max_len) {
    const int w = width_fn(begin, k + 1, ctx);
    if (w > max_width_px) {
      break;
    }
    ++k;
  }
  return k;
}

} // namespace

size_t compute_wrapped_lines(const char *text, int max_width_px, StrWidthFn width_fn,
                             void *width_ctx, WrapLine *out, size_t out_cap) {
  if (text == nullptr || out == nullptr || out_cap == 0 || max_width_px <= 0 ||
      width_fn == nullptr) {
    return 0;
  }

  size_t line_count = 0;
  auto emit = [&](const char *begin, size_t length) -> bool {
    if (line_count >= out_cap) {
      return false;
    }
    out[line_count].begin = begin;
    out[line_count].length = length;
    ++line_count;
    return true;
  };

  const char *p = text;

  while (*p != '\0') {
    // Find end of current paragraph.
    const char *para_end = p;
    while (*para_end != '\0' && *para_end != '\n') {
      ++para_end;
    }

    // Empty paragraph (e.g. "\n\n" boundary) — emit a blank line so the
    // caller's vertical layout stays consistent.
    if (para_end == p) {
      if (!emit(p, 0)) {
        return line_count;
      }
      ++p; // consume the '\n'
      continue;
    }

    const char *line_start = p;
    const char *line_committed_end = p;

    while (p < para_end) {
      // Walk one "atom": zero or more spaces followed by a non-space run.
      // For the first atom of the line this captures leading whitespace
      // (preserved at the very start of a paragraph; consumed by the
      // auto-wrap path on subsequent lines because we restart at
      // word_start there).
      while (p < para_end && *p == ' ') {
        ++p;
      }
      const char *word_start = p;
      while (p < para_end && *p != ' ') {
        ++p;
      }
      const char *atom_end = p;

      if (word_start == atom_end) {
        // Atom was trailing whitespace at paragraph end — consume it (it
        // would be trimmed at the line boundary anyway).
        break;
      }

      const int width = width_fn(line_start, static_cast<size_t>(atom_end - line_start), width_ctx);
      if (width <= max_width_px) {
        line_committed_end = atom_end;
        continue;
      }

      // Atom does not fit.
      if (line_committed_end == line_start) {
        // Nothing committed on this line — the atom itself (or even its
        // first character) overflows.  Hard-break inside the atom.
        const size_t max_len = static_cast<size_t>(atom_end - line_start);
        const size_t k =
            longest_fitting_prefix(line_start, max_len, max_width_px, width_fn, width_ctx);
        if (!emit(line_start, k)) {
          return line_count;
        }
        p = line_start + k;
        line_start = p;
        line_committed_end = p;
        continue;
      }

      // Some words committed; this atom does not fit.  Emit committed
      // portion, then continue the same word on a fresh line.  Spaces
      // between the previous word and this one are consumed at the wrap
      // (we restart at word_start, not atom_start).
      if (!emit(line_start, static_cast<size_t>(line_committed_end - line_start))) {
        return line_count;
      }
      p = word_start;
      line_start = p;
      line_committed_end = p;
    }

    // Emit any residual content from this paragraph.
    if (line_committed_end > line_start) {
      if (!emit(line_start, static_cast<size_t>(line_committed_end - line_start))) {
        return line_count;
      }
    }

    p = para_end;
    if (*p == '\n') {
      ++p; // consume the paragraph separator
    }
  }

  return line_count;
}
