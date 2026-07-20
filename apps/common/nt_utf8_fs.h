#ifndef NTPACKER_UTF8_FS_H
#define NTPACKER_UTF8_FS_H

#include <stdio.h>

#if defined(_WIN32)
#include <stdbool.h>
#include <stddef.h>
#include <wchar.h>

bool nt_utf8_path_to_utf16(const char *path_utf8, wchar_t *out,
                           size_t output_capacity);
#endif

/* CRT-local adapters over the shared core path policy. */
FILE *nt_utf8_fopen(const char *path_utf8, const char *mode);
int nt_utf8_remove(const char *path_utf8);
int nt_utf8_rename(const char *source_utf8, const char *destination_utf8);

#endif
