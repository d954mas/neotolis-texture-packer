# C0-01 contract-spike primitives (master spec §5-5.6, §15-16, §59 items 1-8).
# These are the reference primitives F1 will promote into production tp_core; the
# spike introduces NO on-disk schema and is not linked into tp_core/tp_build/apps.
#
# tp_c0 mirrors tp_core's flag setup (warnings + sanitizers, default CRT). The
# headers include the header-only tp_core/tp_error.h; the include path is pulled
# from tp_core's interface (no link dependency on tp_core). utf8proc (vendored,
# -w) is a PRIVATE dep: only tp_c0_srckey.c includes it, and its allocations are
# consumed/freed inside that TU (no cross-CRT handoff to test exes).
add_library(tp_c0 STATIC
    src/tp_c0_error.c
    src/tp_c0_id.c
    src/tp_c0_path.c
    src/tp_c0_srckey.c
    src/tp_c0_legacy.c
    # C0-02 operation contract spike: catalog, selectors, semantic-state table.
    src/tp_c0_op_catalog.c
    src/tp_c0_selector.c
    src/tp_c0_semantic.c
    # C0-02 transaction contract spike (no engine): shared decode util, request
    # parse, validate, encode (request+result), result decode.
    src/tp_c0_txn_util.c
    src/tp_c0_txn_parse.c
    src/tp_c0_txn_validate.c
    src/tp_c0_txn_encode.c
    src/tp_c0_txn_result.c
    # C0-03 journal / ownership / cache policy spikes (no engine, no OS claim, no
    # journal IO): record framing + recovery, commit-ack state machine, authority
    # vocabulary, memory cache interface, Pack supersession state machine.
    src/tp_c0_journal_frame.c
    src/tp_c0_journal_recover.c
    src/tp_c0_ack.c
    src/tp_c0_authority.c
    src/tp_c0_cache_mem.c
    src/tp_c0_pack_super.c)
target_include_directories(tp_c0 PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    $<TARGET_PROPERTY:tp_core,INTERFACE_INCLUDE_DIRECTORIES>)
# utf8proc (C0-01 srckey) + cJSON (C0-02 txn parse) are PRIVATE deps: neither
# leaks onto tp_c0's public headers (txn structs are hand-typed; cJSON stays in
# the .c files). cjson is the engine-vendored MIT target tp_core already uses.
target_include_directories(tp_c0 PRIVATE
    $<TARGET_PROPERTY:cjson,INTERFACE_INCLUDE_DIRECTORIES>)
target_link_libraries(tp_c0 PRIVATE utf8proc cjson)
target_compile_definitions(tp_c0 PRIVATE _CRT_SECURE_NO_WARNINGS)
nt_set_warning_flags(tp_c0)   # AGENTS.md invariant
nt_set_sanitizer_flags(tp_c0)

# Table-driven pure tests, wired into ctest (packer/tests convention: raw
# add_executable + unity + -U_DLL, add_test registration).
function(tp_c0_add_test name)
    add_executable(${name} tests/${name}.c)
    target_link_libraries(${name} PRIVATE tp_c0 unity)
    target_compile_options(${name} PRIVATE -U_DLL)
    nt_set_warning_flags(${name})   # AGENTS.md invariant: our test TUs too
    add_test(NAME ${name} COMMAND ${name})
endfunction()

tp_c0_add_test(test_c0_error)
tp_c0_add_test(test_c0_id)
tp_c0_add_test(test_c0_shape_id)
tp_c0_add_test(test_c0_path)
tp_c0_add_test(test_c0_srckey)
tp_c0_add_test(test_c0_legacy)
tp_c0_add_test(test_c0_op)   # C0-02 catalog, verb mapping, selectors, semantic fields
tp_c0_add_test(test_c0_txn)  # C0-02 txn decode/encode round-trip, faults, diffs
tp_c0_add_test(test_c0_journal)     # C0-03 journal framing golden vectors + recovery faults
tp_c0_add_test(test_c0_ack)         # C0-03 commit ack ordering + rollback-on-append-fail
tp_c0_add_test(test_c0_authority)   # C0-03 authority vocabulary + permission table
tp_c0_add_test(test_c0_cache)       # C0-03 memory cache interface + budget LRU
tp_c0_add_test(test_c0_pack_super)  # C0-03 Pack supersession / preview selection (decision 0004)
