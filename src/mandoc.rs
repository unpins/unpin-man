//! Render roff manual source to styled terminal text, in-process.
//!
//! This calls the vendored mandoc render-only subset (`vendor/mandoc`, compiled
//! by `build.rs`) through a one-function C bridge: roff bytes + a width go in,
//! the rendered UTF-8/ANSI comes back. mandoc parses with its own parser and
//! runs its terminal formatter, whose output is captured in-process — the
//! formatter appends each byte to a growable buffer on the `termp` (`obuf`),
//! which the bridge hands straight back, so there are no temp files or fds. No
//! subprocess, no cosmo: the same path works on a native Windows build.

use std::os::raw::{c_char, c_int};

unsafe extern "C" {
    /// Render `roff[..rofflen]` at `width` columns (≤0 ⇒ mandoc default) to a
    /// freshly malloc'd UTF-8 buffer; sets `*outlen` to its byte length. Returns
    /// null on a parse/IO failure. Free with [`unpin_man_free_str`].
    fn unpin_man_render(
        roff: *const c_char,
        rofflen: usize,
        width: c_int,
        outlen: *mut usize,
    ) -> *mut c_char;

    /// Free a buffer returned by [`unpin_man_render`].
    fn unpin_man_free_str(s: *mut c_char);
}

/// Render `roff` to terminal text at `width` columns (0 ⇒ mandoc's default).
/// Lines carry ANSI SGR escapes (bold/underline) and UTF-8. On a parse failure
/// (the input isn't valid man/mdoc) the raw roff is returned, so the pager still
/// shows *something* rather than an empty screen.
pub fn render(roff: &str, width: u16) -> String {
    let raw = match render_raw(roff, width) {
        Some(s) => s,
        None => return roff.to_owned(),
    };
    // mandoc's terminal device marks bold/underline with nroff backspace-
    // overstrike (`c\bc`, `_\bc`), not SGR — convert each line to ANSI.
    let mut out = String::with_capacity(raw.len());
    for line in raw.lines() {
        out.push_str(&overstrike_to_ansi(line));
        out.push('\n');
    }
    out
}

/// The unconverted mandoc render (still nroff backspace-overstrike), or `None`
/// on a parse failure.
fn render_raw(roff: &str, width: u16) -> Option<String> {
    // SAFETY: we pass a valid (ptr, len) pair from `roff`; the bridge only reads
    // it during the call and never retains it. On success it returns a malloc'd,
    // NUL-terminated buffer of `outlen` bytes that we copy out and then free with
    // the matching `unpin_man_free_str`.
    unsafe {
        let mut outlen: usize = 0;
        let ptr = unpin_man_render(
            roff.as_ptr().cast::<c_char>(),
            roff.len(),
            c_int::from(width),
            &raw mut outlen,
        );
        if ptr.is_null() {
            return None;
        }
        let bytes = std::slice::from_raw_parts(ptr.cast::<u8>(), outlen);
        let text = String::from_utf8_lossy(bytes).into_owned();
        unpin_man_free_str(ptr);
        Some(text)
    }
}

/// Reduce one rendered line's nroff overstrike to ANSI SGR. mandoc emits, per
/// output column: bold as `c\bc`, underline as `_\bc`, bold+underline as
/// `_\bc\bc`. Per column: a real glyph appearing twice or more ⇒ bold; an
/// underscore overstruck with a glyph ⇒ underline; a lone underscore stays
/// literal. Ported from the C front-end's `process_line` (unpin_man.c).
fn overstrike_to_ansi(line: &str) -> String {
    let chars: Vec<char> = line.chars().collect();
    let mut out = String::with_capacity(line.len());
    let (mut cur_bold, mut cur_under) = (false, false);
    let mut i = 0;

    while i < chars.len() {
        // A backspace with no base glyph before it — emit it literally.
        if chars[i] == '\u{8}' {
            out.push('\u{8}');
            i += 1;
            continue;
        }
        let mut disp = chars[i];
        let mut n_under = u32::from(chars[i] == '_');
        let mut n_glyph = u32::from(chars[i] != '_');
        i += 1;
        // Absorb the `\b<glyph>` overstrikes that target this same column.
        while i + 1 < chars.len() && chars[i] == '\u{8}' {
            let g = chars[i + 1];
            if g == '_' {
                n_under += 1;
            } else {
                n_glyph += 1;
                disp = g;
            }
            i += 2;
        }

        let (bold, under) = if n_glyph == 0 {
            disp = '_';
            (n_under >= 2, false)
        } else {
            (n_glyph >= 2, n_under >= 1)
        };

        if bold != cur_bold || under != cur_under {
            if cur_bold || cur_under {
                out.push_str("\x1b[0m");
            }
            if bold || under {
                out.push_str("\x1b[");
                if bold {
                    out.push('1');
                }
                if bold && under {
                    out.push(';');
                }
                if under {
                    out.push('4');
                }
                out.push('m');
            }
            cur_bold = bold;
            cur_under = under;
        }
        out.push(disp);
    }
    if cur_bold || cur_under {
        out.push_str("\x1b[0m");
    }
    out
}

#[cfg(test)]
mod tests {
    use super::overstrike_to_ansi;

    #[test]
    fn bold_underline_and_plain() {
        assert_eq!(overstrike_to_ansi("N\u{8}NAME"), "\x1b[1mN\x1b[0mAME");
        assert_eq!(overstrike_to_ansi("_\u{8}c"), "\x1b[4mc\x1b[0m");
        assert_eq!(overstrike_to_ansi("plain"), "plain");
        // a lone underscore is not underline
        assert_eq!(overstrike_to_ansi("a_b"), "a_b");
    }
}
