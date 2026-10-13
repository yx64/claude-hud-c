# claude-hud-c. Configure with clang (the GNU-style driver, not clang-cl) and build out of the source tree:
#   cmake -S <SourceDir> -B <BuildDir> -G Ninja -DCMAKE_C_COMPILER=<Clang>
#   cmake --build <BuildDir>                  also runs the tests, see HUD_TEST_AFTER_BUILD at the end
#   ctest --test-dir <BuildDir>               runs them on their own
# Outputs in <BuildDir>, with .exe on Windows; <ProjectName> is the project's name, see the targets at the end:
# release/<ProjectName> (install this one), trace/<ProjectName> (not built by default), <ProjectName>-test and
# <ProjectName>-test-nostate (for test/), fake-jj/jj and test-tools/bytes (test helpers). Windows, Linux and macOS;
# the platform layer is chosen below.
cmake_minimum_required(VERSION 3.28)
# CMake's Windows support for clang insists on a resource compiler although nothing here uses one; take LLVM's,
# from beside the given clang, when none is named.
if(CMAKE_HOST_WIN32 AND CMAKE_C_COMPILER AND NOT CMAKE_RC_COMPILER)
  get_filename_component(_llvm_bin ${CMAKE_C_COMPILER} DIRECTORY)
  if(EXISTS ${_llvm_bin}/llvm-rc.exe)
    set(CMAKE_RC_COMPILER ${_llvm_bin}/llvm-rc.exe CACHE FILEPATH "Resource compiler")
  endif()
endif()
project(claude-hud-c LANGUAGES C)

# The sources use C23 (nullptr, constexpr, typeof) that MSVC's cl does not accept, and every flag below is
# clang's GNU-style spelling.
if(NOT CMAKE_C_COMPILER_ID STREQUAL "Clang" OR NOT CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "GNU")
  message(FATAL_ERROR "claude-hud-c builds with clang's GNU-style driver; this is ${CMAKE_C_COMPILER_ID} (${CMAKE_C_COMPILER_FRONTEND_VARIANT})")
endif()

# Every flag is chosen below, per target. CMake's per-configuration defaults (-O0 -g for Debug, -DNDEBUG for
# Release), its default Windows libraries and its runtime-library selection would all change the program.
foreach(v CMAKE_C_FLAGS CMAKE_EXE_LINKER_FLAGS CMAKE_C_STANDARD_LIBRARIES)
  set(${v} "")
endforeach()
foreach(config DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
  set(CMAKE_C_FLAGS_${config} "")
  set(CMAKE_EXE_LINKER_FLAGS_${config} "")
endforeach()
set(CMAKE_MSVC_RUNTIME_LIBRARY "")
set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "")
# The default rule also embeds a manifest and asks for an import library and a PDB; none of them is wanted.
set(CMAKE_C_LINK_EXECUTABLE "<CMAKE_C_COMPILER> <FLAGS> <CMAKE_C_LINK_FLAGS> <LINK_FLAGS> <OBJECTS> -o <TARGET> <LINK_LIBRARIES>")

# compile_flags.txt is what the editor (clangd) parses with: the language standard and the flags that change how
# the sources parse. The build reads the same file, so the two cannot drift.
file(STRINGS compile_flags.txt HUD_PARSE_FLAGS)
list(TRANSFORM HUD_PARSE_FLAGS REPLACE "^-I(.+)$" "-I${CMAKE_CURRENT_SOURCE_DIR}/\\1")

# -O3      runs on every status line refresh, so speed first; no -march=native, the executable must run on
#          other machines
# -Werror  warnings are dealt with at build time, never installed
set(HUD_WARN -Wall -Wextra -Werror -Wno-unused-parameter -Wno-missing-field-initializers)
set(HUD_COMPILE ${HUD_PARSE_FLAGS} -O3 ${HUD_WARN})

set(HUD_SOURCES
  src/base.c src/config.c src/data.c src/transcript.c src/transcript-state.c src/render.c src/vcs.c src/hud.c
  vendor/yyjson.c vendor/utf8proc.c)

if(WIN32)
  # No C runtime: its start-up costs every refresh about 0.8 ms. src/crt.c supplies the C library functions
  # the program, yyjson, utf8proc and compiler-generated code call (memcpy, memset, __chkstk and so on); the
  # entry point is hud_start in os-win32.c.
  #
  # crt.c is compiled on its own to assembly, the assembly is checked, and that same file is what gets linked:
  #   -ffreestanding  the compiler no longer recognises loops as library calls; otherwise the zeroing loop in
  #                   memset could become a call to memset itself, an endless loop that only shows once some
  #                   change makes the compiler emit it
  #   -fno-lto        the checked assembly must be the final code, not something link-time optimisation rewrites
  # The check (cmake/CheckCrtAsm.cmake): every call and jump, x86-64 or ARM64, goes to a local label or through
  # __imp_ to a system DLL. Any other target could lead back to the function itself, and a tail call is a jump
  # too, so "does not jump to itself" would not be enough.
  set(HUD_CRT_ASM ${CMAKE_CURRENT_BINARY_DIR}/crt.s)
  # A cross build (CMAKE_C_COMPILER_TARGET) has to reach this hand-written compile too, or crt.s comes out
  # for the host.
  set(HUD_CRT_TARGET "")
  if(CMAKE_C_COMPILER_TARGET)
    set(HUD_CRT_TARGET --target=${CMAKE_C_COMPILER_TARGET})
  endif()
  add_custom_command(OUTPUT ${HUD_CRT_ASM}
    COMMAND ${CMAKE_C_COMPILER} ${HUD_CRT_TARGET} ${HUD_PARSE_FLAGS} -O3 -Wall -Wextra -Werror -ffreestanding -fno-lto -S ${CMAKE_CURRENT_SOURCE_DIR}/src/crt.c -o ${HUD_CRT_ASM}
    COMMAND ${CMAKE_COMMAND} -DASM=${HUD_CRT_ASM} -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckCrtAsm.cmake
    DEPENDS src/crt.c ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckCrtAsm.cmake
    COMMAND_EXPAND_LISTS VERBATIM)
  set_source_files_properties(${HUD_CRT_ASM} PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
  set(HUD_PLATFORM_SOURCES src/os-win32.c ${HUD_CRT_ASM})

  # -nostdlib -Wl,/nodefaultlib  no C runtime. -nostdlib alone is not enough: clang records libcmt as a default
  #              library in every object and the linker would still pull it in; /nodefaultlib makes it ignore
  #              all of them, so a function crt.c lacks is a link error instead of being filled in from the CRT
  # -lkernel32   -nostdlib drops kernel32's import library too. bcrypt is not linked: the output speed cache
  #              names its files by SHA-256, and base.c loads bcrypt on the first hash; loading it at start-up
  #              would cost every refresh about 0.7 ms
  # /entry:hud_start      without the CRT there is no CRT start-up function
  # /subsystem:console    the linker cannot infer it when the entry point is not main; stdin and stdout need a
  #                       console program's standard handles
  set(HUD_PLATFORM_LINK -nostdlib LINKER:/nodefaultlib LINKER:/entry:hud_start LINKER:/subsystem:console)
  set(HUD_PLATFORM_LIBS kernel32)

  # Each executable may import from KERNEL32 only, with no delay imports: every further DLL is one more load on
  # every start, silently.
  get_filename_component(HUD_LLVM_BIN ${CMAKE_C_COMPILER} DIRECTORY)
  find_program(HUD_LLVM_READOBJ llvm-readobj HINTS ${HUD_LLVM_BIN} NO_DEFAULT_PATH REQUIRED)
  set(HUD_IMPORT_CHECK ${CMAKE_COMMAND} -DREADOBJ=${HUD_LLVM_READOBJ} -DALLOWED=KERNEL32.dll)
elseif(UNIX)
  # Linux and macOS, on the C library: there its start-up is cheap, and this side aims at working
  # output rather than at the Windows side's start-up time. No import check: the libraries are the
  # system's own.
  # -lm  floor, fmod and the like live in libm on Linux
  set(HUD_PLATFORM_SOURCES src/os-posix.c)
  set(HUD_PLATFORM_LINK "")
  set(HUD_PLATFORM_LIBS m)
  set(HUD_IMPORT_CHECK "")
else()
  message(FATAL_ERROR "claude-hud-c has a platform layer for Windows (src/os-win32.c) and POSIX (src/os-posix.c) only")
endif()

# A saved parse state is taken up only by the build that wrote it, told apart by the time
# transcript-state.c was compiled. An incremental build has to recompile that file whenever any source
# changes, or the new build would take up states the old one saved.
file(GLOB HUD_HEADERS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.h ${CMAKE_CURRENT_SOURCE_DIR}/vendor/*.h)
set(HUD_ALL_SOURCES ${HUD_SOURCES} ${HUD_PLATFORM_SOURCES})
list(FILTER HUD_ALL_SOURCES INCLUDE REGEX "\\.c$")
list(TRANSFORM HUD_ALL_SOURCES PREPEND ${CMAKE_CURRENT_SOURCE_DIR}/)
set_source_files_properties(src/transcript-state.c PROPERTIES OBJECT_DEPENDS "${HUD_ALL_SOURCES};${HUD_HEADERS}")

# One executable per variant, from the same sources, differing only in compile-time macros:
#   HUD_C_TEST                 test build: frozen clock and memory figures, no run time on the last line,
#                              unreproduced input reported with exit code 3
#   HUD_C_TRACE                trace build: also logs every refresh and captures its stdin beside itself
#   HUD_C_NO_TRANSCRIPT_STATE  no saved parse state, every refresh parses from the start; compiled into one
#                              test build so the macro nobody uses day to day cannot rot
# -flto -fuse-ld=lld  cross-file inlining at link time (base.c's small string and JSON helpers are called
#                     character by character everywhere); clang's LTO needs lld
function(hud_executable target output_name output_dir)
  add_executable(${target} ${HUD_SOURCES} ${HUD_PLATFORM_SOURCES})
  target_compile_options(${target} PRIVATE ${HUD_COMPILE} -flto)
  target_compile_definitions(${target} PRIVATE ${ARGN})
  target_link_options(${target} PRIVATE -flto -fuse-ld=lld ${HUD_PLATFORM_LINK})
  target_link_libraries(${target} PRIVATE ${HUD_PLATFORM_LIBS})
  set_target_properties(${target} PROPERTIES OUTPUT_NAME ${output_name} RUNTIME_OUTPUT_DIRECTORY ${output_dir})
  if(HUD_IMPORT_CHECK)
    add_custom_command(TARGET ${target} POST_BUILD COMMAND ${HUD_IMPORT_CHECK} -DEXE=$<TARGET_FILE:${target}> -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckImports.cmake VERBATIM)
  endif()
endfunction()

hud_executable(claude-hud-c claude-hud-c ${CMAKE_CURRENT_BINARY_DIR}/release)
hud_executable(claude-hud-c-trace claude-hud-c ${CMAKE_CURRENT_BINARY_DIR}/trace HUD_C_TRACE)
set_target_properties(claude-hud-c-trace PROPERTIES EXCLUDE_FROM_ALL TRUE)
hud_executable(claude-hud-c-test claude-hud-c-test ${CMAKE_CURRENT_BINARY_DIR} HUD_C_TEST)
hud_executable(claude-hud-c-test-nostate claude-hud-c-test-nostate ${CMAKE_CURRENT_BINARY_DIR} HUD_C_TEST HUD_C_NO_TRANSCRIPT_STATE)

# The jj stand-in for the vcs cases and test/compare/fuzz.mjs. A test double, so it links the C runtime like any
# program.
add_executable(fake-jj test/fake-jj.c)
target_compile_options(fake-jj PRIVATE -std=c23 -O2 -Wall -Wextra -Werror)
set_target_properties(fake-jj PROPERTIES OUTPUT_NAME jj RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/fake-jj MSVC_RUNTIME_LIBRARY MultiThreaded)

# The byte-exact file work of the test drivers, which CMake cannot do itself (see test/bytes.c). A test helper
# like fake-jj.
add_executable(hud-bytes test/bytes.c)
target_compile_options(hud-bytes PRIVATE -std=c23 -O2 -Wall -Wextra -Werror)
# The Microsoft C library marks fopen deprecated in favour of its own fopen_s, which other platforms lack.
target_compile_definitions(hud-bytes PRIVATE $<$<PLATFORM_ID:Windows>:_CRT_SECURE_NO_WARNINGS>)
set_target_properties(hud-bytes PROPERTIES OUTPUT_NAME bytes RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/test-tools MSVC_RUNTIME_LIBRARY MultiThreaded)

# The tests, each group a ctest test of its own, all run with CMake alone (no node, no upstream source), so a
# build anywhere and CI run the same ones:
#   <Group>  each directory under test/cases: the test build against upstream's recorded output (test/run.cmake)
#   cache    resuming from the saved parse state against a full parse (test/cache.cmake)
#   smoke    one refresh with a fixed config: the release build, the test build and the test build without parse
#            state must agree. cmake/Smoke.cmake also takes CONFIG_DIR, to run the same against another config
#            directory
# They print nothing but failures, warnings and a summary; HUD_TEST_VERBOSE=1 and HUD_TEST_ONLY=<Group>[/<Case>]
# in the environment change that, see test/common.cmake.
enable_testing()
file(GLOB HUD_TEST_GROUPS LIST_DIRECTORIES true RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}/test/cases CONFIGURE_DEPENDS
     ${CMAKE_CURRENT_SOURCE_DIR}/test/cases/*)
list(SORT HUD_TEST_GROUPS)
# Files right in test/cases are the defaults every group's cases fall back on.
file(GLOB HUD_TEST_SHARED LIST_DIRECTORIES false CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/test/cases/*)
set(HUD_TEST_OUT ${CMAKE_CURRENT_BINARY_DIR}/test-output)
set(HUD_TEST_RUNS "")
foreach(group IN LISTS HUD_TEST_GROUPS)
  if(IS_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/test/cases/${group})
    file(GLOB_RECURSE inputs CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/test/cases/${group}/*)
    list(APPEND HUD_TEST_RUNS ${group})
    set(HUD_TEST_SCRIPT_${group} test/run.cmake)
    set(HUD_TEST_DEPS_${group} claude-hud-c-test fake-jj hud-bytes ${HUD_TEST_SHARED} ${inputs})
  endif()
endforeach()
list(APPEND HUD_TEST_RUNS cache smoke)
set(HUD_TEST_SCRIPT_cache test/cache.cmake)
set(HUD_TEST_DEPS_cache claude-hud-c-test claude-hud-c-test-nostate hud-bytes test/cache/lines.jsonl test/cache/config.json)
set(HUD_TEST_SCRIPT_smoke cmake/Smoke.cmake)
set(HUD_TEST_DEPS_smoke claude-hud-c claude-hud-c-test claude-hud-c-test-nostate)

# HUD_TEST_AFTER_BUILD: every build also runs the tests whose inputs changed, and fails when one fails. Each
# group leaves a stamp once all its checks passed; a group that failed, or ran filtered by HUD_TEST_ONLY,
# leaves none and runs again on the next build. Off, the tests run only through ctest.
option(HUD_TEST_AFTER_BUILD "Run the tests as part of every build" ON)
set(HUD_TEST_STAMPS "")
set(HUD_TEST_NAMES "")
foreach(name IN LISTS HUD_TEST_RUNS)
  set(script ${HUD_TEST_SCRIPT_${name}})
  set(command ${CMAKE_COMMAND} -DGROUP=${name} -DBIN=${CMAKE_CURRENT_BINARY_DIR} -DRESULT=${HUD_TEST_OUT}/${name}.result)
  if(name STREQUAL "smoke")
    list(APPEND command -DWORK=${CMAKE_CURRENT_BINARY_DIR}/smoke)
  endif()
  add_test(NAME ${name} COMMAND ${command} -P ${CMAKE_CURRENT_SOURCE_DIR}/${script})
  if(HUD_TEST_AFTER_BUILD)
    set(stamp ${HUD_TEST_OUT}/${name}.stamp)
    add_custom_command(OUTPUT ${stamp}
      COMMAND ${command} -DSTAMP=${stamp} -P ${CMAKE_CURRENT_SOURCE_DIR}/${script}
      DEPENDS ${HUD_TEST_DEPS_${name}} ${script} test/common.cmake
      COMMENT "Testing ${name}" VERBATIM)
    list(APPEND HUD_TEST_STAMPS ${stamp})
    list(APPEND HUD_TEST_NAMES ${name})
  endif()
endforeach()
if(HUD_TEST_AFTER_BUILD)
  list(JOIN HUD_TEST_NAMES "," names)
  add_custom_command(OUTPUT ${HUD_TEST_OUT}/summary.stamp
    COMMAND ${CMAKE_COMMAND} -DOUT=${HUD_TEST_OUT} -DGROUPS=${names} -DSTAMP=${HUD_TEST_OUT}/summary.stamp
            -P ${CMAKE_CURRENT_SOURCE_DIR}/test/summary.cmake
    DEPENDS ${HUD_TEST_STAMPS} test/summary.cmake COMMENT "Test summary" VERBATIM)
  add_custom_target(hud-tests ALL DEPENDS ${HUD_TEST_OUT}/summary.stamp)
endif()
