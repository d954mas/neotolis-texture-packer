#ifndef NTPACKER_UTF8_FS_H
#define NTPACKER_UTF8_FS_H

#include <stdio.h>

#if defined(_WIN32)
#include <stdbool.h>
#include <stddef.h>
#include <wchar.h>

/* Convert one strict UTF-8 filesystem path to the Win32 UTF-16 form used by
 * the wide file APIs. Long absolute paths receive a controlled extended-path
 * prefix; caller-supplied device/verbatim namespaces are rejected. */
bool nt_utf8_path_to_utf16(const char *path_utf8, wchar_t *out,
                           size_t output_capacity);
#endif

/* UTF-8 filesystem operations for app/client code. On POSIX these are exact
 * libc pass-throughs; on Windows they cross the boundary once and use wide
 * CRT operations. */
FILE *nt_utf8_fopen(const char *path_utf8, const char *mode);
int nt_utf8_remove(const char *path_utf8);
int nt_utf8_rename(const char *source_utf8, const char *destination_utf8);

#endif
