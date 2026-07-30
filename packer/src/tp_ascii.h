#ifndef TP_ASCII_H
#define TP_ASCII_H

/*
 * Shared ASCII byte classification/folding with no locale input. ONE definition
 * for every byte-level decision that participates in identity: drive-letter
 * classification, case-insensitive path-root/component comparison, and any
 * future grammar over a fixed ASCII alphabet.
 *
 * <ctype.h> may not be used for those decisions. isalpha/tolower consult the
 * active LC_CTYPE, so the same path text would land in different equivalence
 * classes depending on the locale the process happens to run under -- which
 * makes path identity, and everything derived from it, process-dependent. Spec
 * §4.3 forbids that: no locale-dependent input may affect persistent output.
 *
 * Bytes >= 0x80 are never letters and never fold. Multi-byte UTF-8 sequences
 * therefore pass through byte-identical, which is what the UTF-8-admitted,
 * '/'-normalized path model requires.
 *
 * Header-only static inline (like tp_hex.h) -- a private packer/src header,
 * NOT a public tp_core API.
 */

#include <stdbool.h>

/* True for 'A'-'Z' and 'a'-'z' only. */
static inline bool tp_ascii_is_alpha(unsigned char c) {
    return (c >= (unsigned char)'A' && c <= (unsigned char)'Z') ||
           (c >= (unsigned char)'a' && c <= (unsigned char)'z');
}

/* 'A'-'Z' -> 'a'-'z'. Every other byte is returned unchanged. */
static inline unsigned char tp_ascii_tolower(unsigned char c) {
    return (c >= (unsigned char)'A' && c <= (unsigned char)'Z')
               ? (unsigned char)(c - (unsigned char)'A' + (unsigned char)'a')
               : c;
}

#endif /* TP_ASCII_H */
