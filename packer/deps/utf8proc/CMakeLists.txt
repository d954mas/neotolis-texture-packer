# utf8proc (vendored in-repo, packer/deps, MIT "expat" licence) -- Unicode NFC
# normalization + case-folding for source-key canonicalization (master spec §5.3,
# decision docs/decisions/0002-utf8proc-nfc.md). Pinned release v2.9.0; see VENDOR.md
# for source URL and checksum. Engine submodule is untouched (hard invariant).
#
# Static lib compiled -w so our -Werror never applies to third-party code (same
# pattern as apps/gui/deps/tinyfiledialogs). UTF8PROC_STATIC is PUBLIC so consumers
# drop the dllimport decoration. utf8proc.c #includes utf8proc_data.c, so only
# utf8proc.c is a translation unit here.
add_library(utf8proc STATIC utf8proc.c)
target_include_directories(utf8proc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(utf8proc PUBLIC UTF8PROC_STATIC)
target_compile_definitions(utf8proc PRIVATE _CRT_SECURE_NO_WARNINGS)
target_compile_options(utf8proc PRIVATE -w)   # vendored: exempt from our -Werror
