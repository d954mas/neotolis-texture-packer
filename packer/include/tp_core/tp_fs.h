#ifndef TP_FS_H
#define TP_FS_H

#ifdef _WIN32
#include <stdbool.h>
#include <stddef.h>
#include <wchar.h>

/* Caller-buffer-only access to the core's strict UTF-8 Win32 path policy.
 * No FILE*, descriptor, or heap ownership crosses a CRT boundary. Long
 * absolute paths receive a controlled extended prefix; caller-supplied device
 * or verbatim namespaces are rejected. Failure sets errno. */
bool tp_fs_win32_path_copy(const char *path_utf8, wchar_t *out, size_t cap);
#endif

#endif /* TP_FS_H */
