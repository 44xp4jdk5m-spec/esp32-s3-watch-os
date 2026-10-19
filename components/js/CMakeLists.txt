# ── Watch OS JavaScript runtime component ────────────────────────────
# Vendored QuickJS (see quickjs/VERSION for the pinned upstream commit),
# the PSRAM-backed engine wrapper (js_engine.c) and a minimal set of std
# helpers (js_std.c).
#
# Only the QuickJS core (quickjs.c, dtoa.c, libregexp.c, libunicode.c,
# cutils.c) is compiled.  Math/JSON/Date/Map/Set/Promise/... are all part of
# the core engine, so quickjs-libc.c — which bundles a POSIX "os" module that
# has no place on a FreeRTOS sandbox — is intentionally NOT compiled.

idf_component_register(
    SRCS
        "quickjs/quickjs.c"
        "quickjs/dtoa.c"
        "quickjs/libregexp.c"
        "quickjs/libunicode.c"
        "quickjs/cutils.c"
        "js_engine.c"
        "js_std.c"
    INCLUDE_DIRS
        "."
        "quickjs"
    REQUIRES
        heap
        esp_timer
        newlib
)

# CONFIG_VERSION is used verbatim by JS_DumpMemoryUsage(); use a compile
# definition so CMake escapes the string literal correctly for the build
# generator (putting it in COMPILE_FLAGS breaks quoting on Windows).
target_compile_definitions(${COMPONENT_LIB} PRIVATE CONFIG_VERSION=\"2026-06-04\")

# Keep QuickJS warnings quiet in the build log (it is third-party code).
#
# Portability flags required by the xtensa-esp32s3 GCC 14 / newlib toolchain:
#   -D__TM_GMTOFF=tm_gmtoff   enable tm_gmtoff/tm_zone members in newlib's
#   -D__TM_ZONE=tm_zone       struct tm (upstream uses tm_gmtoff for the
#                             Date timezone offset).
#   -Wno-incompatible-pointer-types
#                             int vs int32_t are distinct types on xtensa
#                             newlib (int32_t == long); upstream assumes they
#                             are identical, so demote that from error to off.
set_source_files_properties(
    quickjs/quickjs.c
    quickjs/dtoa.c
    quickjs/libregexp.c
    quickjs/libunicode.c
    quickjs/cutils.c
    PROPERTIES
        COMPILE_FLAGS "-Wno-unused-function -Wno-unused-variable -Wno-sign-compare -Wno-missing-field-initializers -Wno-char-subscripts -Wno-incompatible-pointer-types -D__TM_GMTOFF=tm_gmtoff -D__TM_ZONE=tm_zone"
)
