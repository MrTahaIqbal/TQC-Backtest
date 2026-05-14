cmake_minimum_required(VERSION 3.22)

project(bigboyagent_backtest VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# FIX-COMPDB: generate compile_commands.json for clangd / clang-tidy / IDE LSP.
# Without this, header resolution for C++20 concepts, <span>, and <charconv>
# fails silently in every editor that uses a language server.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# ── Sources ───────────────────────────────────────────────────────────────────
set(SOURCES
    main.cpp
    http_server.cpp
    backtest_engine.cpp
    stats_engine.cpp
    config.cpp
)

add_executable(backtest ${SOURCES})

target_include_directories(backtest PRIVATE ${CMAKE_SOURCE_DIR})

# ── Compiler flags ────────────────────────────────────────────────────────────
target_compile_options(backtest PRIVATE

    # ── Release optimisations ─────────────────────────────────────────────────
    $<$<CONFIG:Release>:-O3>
    $<$<CONFIG:Release>:-DNDEBUG>

    # FIX-LTO: -flto must be in compile options AND link options for GCC to emit
    # GIMPLE IR during compilation.  With only a link flag the compiler produces
    # ordinary .o files and LTO silently degrades to a conventional link pass.
    $<$<CONFIG:Release>:-flto=auto>

    # FIX-ARCH: AVX2/FMA flags are now Release-only.
    # Debug builds use the portable x86-64 baseline so:
    #   (a) developers without AVX2 hardware can run Debug / ASan builds, and
    #   (b) AddressSanitizer alignment-fault detection is not masked by SIMD.
    # HuggingFace Spaces production hardware is Intel Xeon (AVX2 guaranteed),
    # so Release continues to use x86-64-v3.
    $<$<CONFIG:Release>:-march=x86-64-v3>
    $<$<CONFIG:Release>:-mavx2>
    $<$<CONFIG:Release>:-mfma>
    $<$<CONFIG:Release>:-mbmi2>   # implicit in x86-64-v3; explicit for clarity

    # FIX-UNROLL / FIX-FP: Release-only.
    # -funroll-loops distorts debugger line-to-instruction mapping.
    # -fomit-frame-pointer prevents stack unwinding in crash dumps and
    # disables perf call-graph collection.  Both are harmful in Debug.
    $<$<CONFIG:Release>:-funroll-loops>
    $<$<CONFIG:Release>:-fomit-frame-pointer>

    # Floating-point: errno and trapping are unnecessary overhead for this
    # workload.  Applied to all configs — these affect correctness only for
    # code that tests errno after math calls or relies on FP traps, which
    # this codebase does not.
    -fno-math-errno
    -fno-trapping-math

    # Hardening: _FORTIFY_SOURCE=2 requires -O1 or higher to function.
    # It is Release-only here; -O1 in Debug would satisfy the requirement
    # but FORTIFY adds little value when ASan already instruments all
    # buffer accesses.
    $<$<CONFIG:Release>:-D_FORTIFY_SOURCE=2>
    -fstack-protector-strong

    # ── Debug build ───────────────────────────────────────────────────────────
    # FIX-ARCH: portable baseline for Debug — no AVX2 requirement on dev machines.
    $<$<CONFIG:Debug>:-march=x86-64>
    $<$<CONFIG:Debug>:-O1>
    $<$<CONFIG:Debug>:-g3>
    $<$<CONFIG:Debug>:-fsanitize=address,undefined>
    $<$<CONFIG:Debug>:-fno-omit-frame-pointer>

    # ── Diagnostics (all build types) ─────────────────────────────────────────
    -Wall
    -Wextra
    -Wshadow
    -Wno-unused-parameter

    # FIX-WARN: -Wconversion catches implicit narrowing (e.g. size_t → int),
    # exactly the class of bug that produced the n_trades counter ambiguity and
    # the strncmp limit-15 errors.  -Wpedantic enforces strict ISO C++20.
    -Wconversion
    -Wpedantic
)

# ── Link options ──────────────────────────────────────────────────────────────
target_link_options(backtest PRIVATE
    # FIX-LTO: must match the compile-time -flto=auto flag.
    $<$<CONFIG:Release>:-flto=auto>

    # -s strips the symbol table from the final binary, reducing image size.
    # Trade-off: crash analysis requires a separately preserved unstripped binary
    # or debug-info package.  Acceptable for this deployment model (HF Spaces)
    # where the build artefact is ephemeral and logs are the primary diagnostic.
    $<$<CONFIG:Release>:-s>

    $<$<CONFIG:Debug>:-fsanitize=address,undefined>
)

target_link_libraries(backtest PRIVATE pthread)

# ── Install rule (enables cmake --install for CI artefact packaging) ──────────
install(TARGETS backtest RUNTIME DESTINATION bin)

message(STATUS "──────────────────────────────────────────────────────────────")
message(STATUS "Project:   BigBoyAgent-Backtest v1 | Taha Iqbal")
message(STATUS "Build:     ${CMAKE_BUILD_TYPE}")
message(STATUS "Compiler:  ${CMAKE_CXX_COMPILER}")
message(STATUS "Standard:  C++20")
if(CMAKE_BUILD_TYPE STREQUAL "Release")
message(STATUS "Arch:      x86-64-v3 (AVX2 + FMA + BMI2) — Release only")
else()
message(STATUS "Arch:      x86-64 baseline (Debug/ASan — no AVX2 requirement)")
endif()
message(STATUS "LTO:       enabled (compile + link, Release only)")
message(STATUS "CompileDB: ${CMAKE_BINARY_DIR}/compile_commands.json")
message(STATUS "──────────────────────────────────────────────────────────────")
