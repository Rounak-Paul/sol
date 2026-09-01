cmake_minimum_required(VERSION 3.20)

if(APPLE)
    project(QuasarEngine LANGUAGES C CXX OBJC)
else()
    project(QuasarEngine LANGUAGES C CXX)
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE STREQUAL "Release")
    # Plain enable_testing() does NOT define BUILD_TESTING (that's a
    # CTest-module convention, and this project never include(CTest)) —
    # only include(CTest) or an explicit option() does. sol's and
    # causality's own test targets are gated on if(BUILD_TESTING), so
    # without this they silently never register, and libssh2 (further
    # below) both reads and — needing to be forced OFF for its own,
    # unwanted test suite — writes this same well-known CMake variable
    # name.
    #
    # SOL_WANT_TESTING (not BUILD_TESTING itself) is what libssh2's
    # save/restore snapshots and restores after its subdirectory runs.
    # BUILD_TESTING cannot serve as its own "remembered intent": on any
    # reconfigure after the first, this project's own
    # `set(BUILD_TESTING ... CACHE BOOL "" FORCE)` calls have already
    # left BUILD_TESTING sitting in the cache as OFF (whatever libssh2's
    # subdirectory last forced it to) — option() below only supplies a
    # default for a variable that ISN'T already cached, so on that second
    # run it would silently accept the stale OFF instead of ON, and
    # "restoring" that captured OFF would then re-force it OFF forever,
    # permanently hiding every causality_*/sol_* CTest target after the
    # very first configure. SOL_WANT_TESTING has no other writer anywhere
    # in this tree, so it can't suffer the same corruption.
    set(SOL_WANT_TESTING ON CACHE BOOL
        "Build test suites for this project and its vendored libraries")
    set(BUILD_TESTING ${SOL_WANT_TESTING} CACHE BOOL "" FORCE)
    enable_testing()
endif()

# MSVC runtime library — always the Release DLL CRT, even in Debug config.
# The LunarG Vulkan SDK ships shaderc_combined.lib built only against the
# Release CRT, with no debug variant; a Debug-CRT (/MDd) build fails to link
# with LNK2038 CRT mismatches against it (and against causality.lib, which
# forces the same override — see vendors/causality/causality/CMakeLists.txt).
# Debug configs still compile at -Od with full debug symbols — only /MDd's
# extra iterator/heap-debug checks are unavailable.
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)

    # Match POSIX's much larger default thread stack (commonly 8 MiB on
    # Linux/macOS) instead of MSVC's 1 MiB default. Unoptimized (/Od) Debug
    # builds give every local its own stack slot rather than reusing space
    # across sibling scopes, so a function with many locals or many
    # expansions of a macro that declares one (e.g. the test suites' SOL_RUN)
    # can end up with a multi-hundred-KB single-frame prologue allocation
    # that is routine under Linux/macOS's default but overflows Windows'.
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} /STACK:8388608")
endif()

# Output directories
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)

# Dependencies
find_package(Vulkan REQUIRED)
find_package(Threads REQUIRED)

# Platform definitions
if(APPLE)
    add_compile_definitions(QS_PLATFORM_APPLE)
elseif(WIN32)
    add_compile_definitions(QS_PLATFORM_WINDOWS NOMINMAX WIN32_LEAN_AND_MEAN)
elseif(UNIX)
    add_compile_definitions(QS_PLATFORM_LINUX)
endif()

# Vendors
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)

set(GLM_BUILD_LIBRARY OFF CACHE BOOL "" FORCE)
set(GLM_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLM_BUILD_INSTALL OFF CACHE BOOL "" FORCE)

# mbedTLS — crypto/TLS backend for libssh2 below. Vendored in source so the
# final SSH2 client has zero system-library dependency (no OpenSSL needed),
# matching this project's "fully self-contained, portable binary" goal.
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
set(USE_SHARED_MBEDTLS_LIBRARY OFF CACHE BOOL "" FORCE)
set(USE_STATIC_MBEDTLS_LIBRARY ON CACHE BOOL "" FORCE)

add_subdirectory(vendors/mbedtls)

# libssh2's CMakeLists locates mbedTLS via find_package(MbedTLS), which uses
# cmake/FindMbedTLS.cmake — a module written for an already-installed
# mbedTLS (pkg-config, or find_path/find_library against install paths),
# with no awareness of an in-tree add_subdirectory build. Pre-defining the
# exact singular input variables that module's find_library branch reads
# (MBEDTLS_INCLUDE_DIR, MBEDCRYPTO_LIBRARY — see "Input variables" at the
# top of that file) makes find_path/find_library resolve immediately
# against the in-tree build instead of searching the filesystem for a
# system install. The pkg-config branch above this still runs first and
# fails harmlessly (no system mbedcrypto.pc) before falling through here.
set(MBEDTLS_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/vendors/mbedtls/include CACHE PATH "" FORCE)
set(MBEDCRYPTO_LIBRARY mbedcrypto CACHE STRING "" FORCE)

# libssh2 — SSH2 transport, SFTP, and exec-channel support with zero system
# dependency (built against the vendored mbedTLS above). Used for Sol's raw
# SSH remote-terminal/file/search/git features — no agent or extra software
# is installed on the remote side; only the SSH server it already runs.
#
# BUILD_TESTING and BUILD_SHARED_LIBS are CMake's own global convention
# variables (CTest's enable_testing()/if(BUILD_TESTING) gate — used by
# vendors/causality's own test suite too — and the default library-type
# switch every add_library() without an explicit STATIC/SHARED respects).
# Forcing them as CACHE variables here would leak past this subdirectory
# and silently disable Causality's causality_* test targets alongside
# libssh2's — snapshot and restore immediately after add_subdirectory
# instead of leaving them permanently overwritten.
#
# Restoring to SOL_WANT_TESTING rather than a freshly-captured
# ${BUILD_TESTING} is deliberate — see the comment where SOL_WANT_TESTING
# is defined above: BUILD_TESTING's cached value is not trustworthy as
# its own snapshot source across repeated configures.
set(_SOL_SAVED_BUILD_SHARED_LIBS ${BUILD_SHARED_LIBS})
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(CRYPTO_BACKEND "mbedTLS" CACHE STRING "" FORCE)

# libssh2's own src/CMakeLists.txt ("Package config" section) needs a
# small patch to skip export()/install(EXPORT) when not built as the
# top-level project — that section otherwise fails: it requires every
# transitively linked target (our in-tree "mbedcrypto", linked by name
# rather than via find_package()) to belong to a CMake export set, which
# an in-tree add_subdirectory() dependency never does.
#
# Applied to a COPY of the submodule under the build directory, never to
# vendors/libssh2 itself — two reasons: the submodule tracks upstream
# libssh2 as a plain git checkout (unlike causality/tree-sitter, which
# are Rounak-Paul/* forks this repo controls), so a local edit inside it
# can never be pushed; and patching the real submodule in place left it
# showing as locally modified after every single build, which is exactly
# the workflow annoyance this copy-then-patch approach exists to avoid.
# `git status` on vendors/libssh2 now stays clean no matter how many
# times this project is configured.
#
# The copy is skipped once it already exists and matches the submodule's
# current commit (tracked via a stamp file holding that commit hash) —
# re-copying + re-patching hundreds of files on every configure would be
# needless I/O for a source tree that essentially never changes commit.
set(SOL_LIBSSH2_VENDOR_COPY ${CMAKE_BINARY_DIR}/_vendor/libssh2)
set(SOL_LIBSSH2_STAMP ${CMAKE_BINARY_DIR}/_vendor/libssh2.commit-stamp)

execute_process(
    COMMAND git -C ${CMAKE_CURRENT_SOURCE_DIR}/vendors/libssh2 rev-parse HEAD
    OUTPUT_VARIABLE SOL_LIBSSH2_CURRENT_COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

set(SOL_LIBSSH2_STAMPED_COMMIT "")
if(EXISTS ${SOL_LIBSSH2_STAMP})
    file(READ ${SOL_LIBSSH2_STAMP} SOL_LIBSSH2_STAMPED_COMMIT)
    string(STRIP "${SOL_LIBSSH2_STAMPED_COMMIT}" SOL_LIBSSH2_STAMPED_COMMIT)
endif()

if(NOT EXISTS ${SOL_LIBSSH2_VENDOR_COPY} OR
   NOT SOL_LIBSSH2_STAMPED_COMMIT STREQUAL SOL_LIBSSH2_CURRENT_COMMIT)
    file(REMOVE_RECURSE ${SOL_LIBSSH2_VENDOR_COPY})
    file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/vendors/libssh2/
        DESTINATION ${SOL_LIBSSH2_VENDOR_COPY}
        PATTERN ".git" EXCLUDE)

    # `git apply` is run from THIS repo (CMAKE_CURRENT_SOURCE_DIR), not
    # from inside the copy — the copy has no .git of its own (excluded
    # above; a submodule's .git is just a gitdir-pointer file that would
    # resolve to the wrong path if copied anyway), and git apply run
    # with no repository context at all does not fail, it just silently
    # treats the patch as a no-op ("0 files changed") while still
    # exiting 0. --directory redirects where the patch's a/src/... path
    # lands without needing the target to be its own repository — but
    # git rejects an absolute --directory value ("invalid path"), so it
    # must be expressed relative to CMAKE_CURRENT_SOURCE_DIR (the
    # WORKING_DIRECTORY below) rather than passing SOL_LIBSSH2_VENDOR_COPY
    # (absolute) directly.
    file(RELATIVE_PATH SOL_LIBSSH2_VENDOR_COPY_REL
        ${CMAKE_CURRENT_SOURCE_DIR} ${SOL_LIBSSH2_VENDOR_COPY})
    execute_process(
        COMMAND git apply --directory=${SOL_LIBSSH2_VENDOR_COPY_REL} vendors/libssh2.patch
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        RESULT_VARIABLE SOL_LIBSSH2_PATCH_RESULT
    )
    # Belt-and-suspenders beyond checking the exit code: the silent
    # no-op failure mode above also exits 0, so this build previously
    # passed configure while producing an unpatched copy and only failed
    # much later, confusingly, deep in libssh2's own install(EXPORT)
    # logic. Grepping the actual result for the guard the patch inserts
    # catches that failure mode at the source instead.
    if(SOL_LIBSSH2_PATCH_RESULT EQUAL 0)
        file(STRINGS ${SOL_LIBSSH2_VENDOR_COPY}/src/CMakeLists.txt
            SOL_LIBSSH2_PATCH_MARKER REGEX "PROJECT_IS_TOP_LEVEL")
    endif()
    if(NOT SOL_LIBSSH2_PATCH_RESULT EQUAL 0 OR NOT SOL_LIBSSH2_PATCH_MARKER)
        message(FATAL_ERROR
            "Failed to apply vendors/libssh2.patch to the build-tree copy of "
            "libssh2/src/CMakeLists.txt — the submodule may be at an unexpected "
            "commit. See vendors/libssh2.patch.")
    endif()

    file(WRITE ${SOL_LIBSSH2_STAMP} "${SOL_LIBSSH2_CURRENT_COMMIT}")
endif()

add_subdirectory(${SOL_LIBSSH2_VENDOR_COPY} ${CMAKE_BINARY_DIR}/_vendor/libssh2-build)
set(BUILD_TESTING ${SOL_WANT_TESTING} CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS ${_SOL_SAVED_BUILD_SHARED_LIBS} CACHE BOOL "" FORCE)

add_subdirectory(vendors/causality)

# causality is a static library — its PRIVATE dependencies don't propagate
# through CMake's dependency graph, so the final executable must link
# shaderc and its deps directly on Linux.
if(UNIX AND NOT APPLE)
    find_library(SHADERC_LIB     NAMES shaderc_combined        PATHS /usr/lib/x86_64-linux-gnu /usr/local/lib REQUIRED)
    find_library(GLSLANG_LIB     NAMES glslang                 PATHS /usr/lib/x86_64-linux-gnu /usr/local/lib REQUIRED)
    find_library(SPIRV_LIB       NAMES SPIRV                   PATHS /usr/lib/x86_64-linux-gnu /usr/local/lib REQUIRED)
    find_library(SPIRV_TOOLS_LIB NAMES SPIRV-Tools             PATHS /usr/lib/x86_64-linux-gnu /usr/local/lib REQUIRED)
    find_library(SPIRV_TOOLS_OPT NAMES SPIRV-Tools-opt         PATHS /usr/lib/x86_64-linux-gnu /usr/local/lib REQUIRED)
    find_library(GLSLANG_RES_LIB NAMES glslang-default-resource-limits PATHS /usr/lib/x86_64-linux-gnu /usr/local/lib REQUIRED)
    set(CAUSALITY_LINUX_DEPS
        ${SHADERC_LIB} ${GLSLANG_LIB} ${SPIRV_LIB}
        ${SPIRV_TOOLS_OPT} ${SPIRV_TOOLS_LIB} ${GLSLANG_RES_LIB})
endif()

# ---- tree-sitter runtime -------------------------------------------
# A single static library built from tree-sitter's unity-build entry
# point.  Linked into sol so it can invoke the parser at runtime.
add_library(tree_sitter_runtime STATIC
    ${CMAKE_SOURCE_DIR}/vendors/tree-sitter/lib/src/lib.c
)
target_include_directories(tree_sitter_runtime
    PUBLIC  ${CMAKE_SOURCE_DIR}/vendors/tree-sitter/lib/include
    PRIVATE ${CMAKE_SOURCE_DIR}/vendors/tree-sitter/lib/src
)
set_target_properties(tree_sitter_runtime PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)

# Application
add_subdirectory(sol)

# Plugins (tree-sitter language plugins, built as .dylib/.so MODULEs)
add_subdirectory(plugins)
