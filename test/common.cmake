# Shared by the test drivers (run.cmake, cache.cmake): the report, the environment a refresh runs in,
# the placeholders of the case files, and the canonical form outputs are compared in.
#
# The report is quiet: a check that passes prints nothing, a failing one prints one FAIL line naming
# <Group>/<Case> and where it differs, and the group prints its summary only when something failed
# or warned. Two switches, read from the environment so that ctest, a build that runs the tests and
# CI all take them alike; the program under test reads neither:
#   HUD_TEST_VERBOSE=1                 also print a PASS line per check and the summary
#   HUD_TEST_ONLY=<Group>[/<Case>],..  run only these; a filtered run never marks its group done
#
# Defined by the caller: GROUP, BIN (the build directory), optionally STAMP (written once every check
# of the group passed, so a build reruns only what changed) and RESULT (the counts, for the summary).
foreach(v GROUP BIN)
  if(NOT DEFINED ${v})
    message(FATAL_ERROR "${CMAKE_CURRENT_LIST_FILE}: pass -D${v}=...")
  endif()
endforeach()
if(CMAKE_HOST_WIN32)
  set(HUD_EXE_SUFFIX .exe)
  set(HUD_PATH_SEP ";")
else()
  set(HUD_EXE_SUFFIX "")
  set(HUD_PATH_SEP ":")
endif()
set(HUD_TEST_DIR ${CMAKE_CURRENT_LIST_DIR})
set(HUD_OUT ${BIN}/test-output)
# A fresh work directory per run: nothing a run leaves behind is deleted, and nothing left from an
# earlier run can leak into this one. Old runs stay in the build directory until it goes.
string(TIMESTAMP _stamp "%Y%m%d-%H%M%S")
string(RANDOM LENGTH 6 ALPHABET 0123456789abcdef _rnd)
set(HUD_RUN_ID run-${_stamp}-${_rnd})
set(HUD_RUN ${BIN}/test-work/${HUD_RUN_ID})
# The frozen clock and memory figures of the test build (HUD_C_TEST): noon UTC, so that every time
# zone from UTC-11 to UTC+11 sees the same date.
set(HUD_NOW_MS 1791633600000)
set(HUD_MEM 17179869184:5368709120)

# ---- report ----

set(HUD_VERBOSE FALSE)
if("$ENV{HUD_TEST_VERBOSE}" MATCHES "^(1|[Tt][Rr][Uu][Ee]|[Oo][Nn]|[Yy][Ee][Ss])$")
  set(HUD_VERBOSE TRUE)
endif()
set(HUD_FILTERED FALSE)
if(NOT "$ENV{HUD_TEST_ONLY}" STREQUAL "")
  set(HUD_FILTERED TRUE)
endif()
set_property(GLOBAL PROPERTY hud_passed 0)
set_property(GLOBAL PROPERTY hud_failed 0)
set_property(GLOBAL PROPERTY hud_skipped 0)
set_property(GLOBAL PROPERTY hud_warned 0)

# Whether <Case> of this group is to run under HUD_TEST_ONLY.
function(hud_selected case out)
  set(sel TRUE)
  if(HUD_FILTERED)
    set(sel FALSE)
    string(REPLACE "," ";" only "$ENV{HUD_TEST_ONLY}")
    foreach(o IN LISTS only)
      string(STRIP "${o}" o)
      if(o STREQUAL GROUP OR o STREQUAL "${GROUP}/${case}")
        set(sel TRUE)
      endif()
    endforeach()
  endif()
  set(${out} ${sel} PARENT_SCOPE)
endfunction()

function(hud_count what)
  get_property(n GLOBAL PROPERTY hud_${what})
  math(EXPR n "${n} + 1")
  set_property(GLOBAL PROPERTY hud_${what} ${n})
endfunction()

function(hud_pass case)
  hud_count(passed)
  if(HUD_VERBOSE)
    message("PASS ${GROUP}/${case}")
  endif()
endfunction()

# <What> names the failure; an optional third argument holds detail lines, already indented (output
# may hold semicolons, so the details are one string, not a list).
function(hud_fail case what)
  hud_count(failed)
  set(detail "")
  if(ARGC GREATER 2)
    set(detail "${ARGV2}")
  endif()
  message("FAIL ${GROUP}/${case}: ${what}${detail}")
endfunction()

function(hud_warn what)
  hud_count(warned)
  message("WARN ${GROUP}: ${what}")
endfunction()

# A check that cannot run here, for a reason outside the program (such as a system locale it does not
# reproduce). Counted apart and warned about once per reason.
function(hud_skip case why)
  hud_count(skipped)
  get_property(seen GLOBAL PROPERTY hud_skip_reasons)
  if(NOT why IN_LIST seen)
    set_property(GLOBAL APPEND PROPERTY hud_skip_reasons "${why}")
    hud_warn("${why}; such checks are skipped")
  endif()
  if(HUD_VERBOSE)
    message("SKIP ${GROUP}/${case}: ${why}")
  endif()
endfunction()

# Ends the group: the summary when it is of interest, the counts for the build's summary, the stamp
# when the whole group passed, and a failing exit code when anything failed.
function(hud_finish)
  foreach(k passed failed skipped warned)
    get_property(${k} GLOBAL PROPERTY hud_${k})
  endforeach()
  math(EXPR total "${passed} + ${failed}")
  set(line "${GROUP}: ${passed}/${total} passed")
  if(skipped)
    string(APPEND line ", ${skipped} skipped")
  endif()
  if(DEFINED RESULT AND NOT HUD_FILTERED)
    file(WRITE ${RESULT} "${GROUP} ${passed} ${total} ${skipped}\n")
  endif()
  if(failed)
    if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.29)
      message("${line}")
      cmake_language(EXIT 1)
    endif()
    message(FATAL_ERROR "${line}")
  endif()
  if(warned OR HUD_VERBOSE)
    message("${line}")
  endif()
  if(DEFINED STAMP AND NOT HUD_FILTERED)
    file(TOUCH ${STAMP})
  endif()
endfunction()

# ---- environment ----

# Only these survive from the caller's environment. The program and upstream read variables
# (ANTHROPIC_BASE_URL, CLAUDE_CONFIG_DIR, COLUMNS, LANG, TZ and more) whose stray values change the
# output, so everything not needed to start a process goes, and each refresh gets what it needs.
set(HUD_ENV_KEEP PATH SYSTEMROOT SYSTEMDRIVE WINDIR COMSPEC PATHEXT TEMP TMP TMPDIR)
set(HUD_PATH0 "$ENV{PATH}")

# How long any one process a test starts may run, in seconds. A refresh or a git call takes milliseconds; one
# still running after this is hung, and execute_process ends it and reports a timeout as its result.
set(HUD_RUN_TIMEOUT 60)

# Unsets every variable not kept, then sets the ones every refresh gets for the work directory <Dir>.
function(hud_env_reset dir)
  execute_process(COMMAND ${CMAKE_COMMAND} -E environment OUTPUT_VARIABLE all COMMAND_ERROR_IS_FATAL ANY)
  string(REGEX MATCHALL "(^|\n)[^=\n]+=" names "${all}")
  foreach(n IN LISTS names)
    string(REGEX REPLACE "^\n?(.*)=$" "\\1" n "${n}")
    string(TOUPPER "${n}" upper)
    if(NOT upper IN_LIST HUD_ENV_KEEP)
      unset(ENV{${n}})
    endif()
  endforeach()
  # GIT_CEILING_DIRECTORIES: a build directory inside a git checkout must not make the cwd a repository.
  foreach(pair "CLAUDE_CONFIG_DIR;${dir}/claude" "HOME;${dir}/home" "USERPROFILE;${dir}/home" "HUD_CACHE_DIR;${dir}/state"
               "GIT_CONFIG_GLOBAL;${dir}/home/.gitconfig" "GIT_CEILING_DIRECTORIES;${dir}")
    list(GET pair 0 k)
    list(GET pair 1 v)
    file(TO_NATIVE_PATH "${v}" v)
    set(ENV{${k}} "${v}")
  endforeach()
  set(ENV{HUD_FAKE_NOW_MS} ${HUD_NOW_MS})
  set(ENV{HUD_FAKE_MEM} ${HUD_MEM})
  set(ENV{GIT_CONFIG_NOSYSTEM} 1)
  # git fails instead of prompting for credentials, which would hang a test with no one to answer.
  set(ENV{GIT_TERMINAL_PROMPT} 0)
  file(TO_NATIVE_PATH "${BIN}/fake-jj" jj)
  set(ENV{PATH} "${jj}${HUD_PATH_SEP}${HUD_PATH0}")
  file(MAKE_DIRECTORY ${dir}/claude ${dir}/home ${dir}/proj)
  file(TOUCH ${dir}/home/.gitconfig)
endfunction()

# ---- placeholders ----

# The placeholders for the work directory <Dir>, set in the caller: HUD_NATIVE_<Name> the native path,
# HUD_JSON_<Name> the same inside a JSON string, for WORK (the directory), PROJ (the cwd the stdin
# names), CLAUDE (CLAUDE_CONFIG_DIR), HOME, and HUD_URL_WORK the file URL of WORK as the program
# writes it into hyperlinks.
macro(hud_placeholders dir transcript)
  foreach(_p "WORK;${dir}" "PROJ;${dir}/proj" "CLAUDE;${dir}/claude" "HOME;${dir}/home" "TRANSCRIPT;${transcript}")
    list(GET _p 0 _k)
    list(GET _p 1 _v)
    file(TO_NATIVE_PATH "${_v}" HUD_NATIVE_${_k})
    string(REPLACE "\\" "\\\\" HUD_JSON_${_k} "${HUD_NATIVE_${_k}}")
  endforeach()
  if(CMAKE_HOST_WIN32)
    set(HUD_JSON_SEP "\\\\")
  else()
    set(HUD_JSON_SEP "/")
  endif()
  hud_file_url("${dir}" HUD_URL_WORK)
endmacro()

# node's pathToFileURL: forward slashes, a leading / before a drive letter, and percent-encoding of the
# bytes a URL path may not hold as they are.
function(hud_file_url path out)
  string(LENGTH "${path}" n)
  set(url "")
  math(EXPR last "${n} - 1")
  foreach(i RANGE 0 ${last})
    string(SUBSTRING "${path}" ${i} 1 c)
    string(HEX "${c}" h)
    # A semicolon would be kept too, but cannot pass through a CMake list; it is encoded here, and the
    # leftover check in hud_canon then reports the mismatch instead of a wrong comparison.
    if(c MATCHES "^[A-Za-z0-9._~!$&'()*+,=:@/[|^-]$" OR c STREQUAL "]")
      string(APPEND url "${c}")
    else()
      string(TOUPPER "${h}" h)
      string(APPEND url "%${h}")
    endif()
  endforeach()
  if(CMAKE_HOST_WIN32)
    set(url "/${url}")
  endif()
  set(${out} "file://${url}" PARENT_SCOPE)
endfunction()

# Values set in the environment of a refresh (env.txt): @REP:<N>:<Text>@ and @x<hh>@ as in case files,
# see hud_copy_subst; the path placeholders do not apply. Needs hud_placeholders in the caller's scope.
function(hud_subst text out)
  while(text MATCHES "@REP:([0-9]+):([^@]*)@")
    string(REPEAT "${CMAKE_MATCH_2}" ${CMAKE_MATCH_1} r)
    string(REPLACE "${CMAKE_MATCH_0}" "${r}" text "${text}")
  endwhile()
  while(text MATCHES "@x([0-9a-f][0-9a-f])@")
    set(m "${CMAKE_MATCH_0}")
    math(EXPR b "0x${CMAKE_MATCH_1}")
    if(b EQUAL 0)
      message(FATAL_ERROR "@x00@: a NUL byte cannot be passed in the environment")
    endif()
    string(ASCII ${b} c)
    string(REPLACE "${m}" "${c}" text "${text}")
  endwhile()
  set(${out} "${text}" PARENT_SCOPE)
endfunction()

set(HUD_BYTES ${BIN}/test-tools/bytes${HUD_EXE_SUFFIX})
if(NOT EXISTS ${HUD_BYTES})
  message(FATAL_ERROR "${CMAKE_CURRENT_LIST_FILE}: missing ${HUD_BYTES}; build first")
endif()

# Writes a case file into the work directory, byte for byte (test/bytes.c: subst), <Count> times if
# given: a carriage return a checkout may have added is dropped (case files hold none of their own),
# then
#   @REP:<N>:<Text>@   <Text> (no @ in it) N times
#   @WORK@ @PROJ@ @CLAUDE@ @HOME@ @TRANSCRIPT@   the paths, escaped for a JSON string
#   @SEP@              the path separator, escaped likewise
#   @x<hh>@            the byte <hh> in lowercase hex, 01 to ff: a carriage return is @x0d@
# Needs hud_placeholders in the caller's scope.
function(hud_copy_subst src dst)
  set(count 1)
  if(ARGC GREATER 2)
    set(count ${ARGV2})
  endif()
  get_filename_component(d "${dst}" DIRECTORY)
  file(MAKE_DIRECTORY "${d}")
  set(values "")
  foreach(p WORK PROJ CLAUDE HOME TRANSCRIPT SEP)
    list(APPEND values "${p}=${HUD_JSON_${p}}")
  endforeach()
  execute_process(COMMAND ${HUD_BYTES} subst ${src} ${dst} ${count} ${values} COMMAND_ERROR_IS_FATAL ANY)
endfunction()

# ---- canonical form ----

# Writes the canonical form of an output file to <Dst>, the form expected files hold: every carriage
# return as @x0d@ (so a checkout that converts line ends cannot touch it) and the work directory as
# @WORKURL@ (its file URL) or @WORK@. Sets <Problem> to why the output has no canonical form, or empty:
# it holds a NUL, which the form cannot carry, or the work directory in a form not replaced. Needs
# hud_placeholders in the caller's scope.
function(hud_canon src dst problem)
  get_filename_component(d "${dst}" DIRECTORY)
  file(MAKE_DIRECTORY "${d}")
  execute_process(COMMAND ${HUD_BYTES} canon ${src} ${dst} "WORKURL=${HUD_URL_WORK}" "WORK=${HUD_JSON_WORK}" "WORK=${HUD_NATIVE_WORK}"
                  RESULT_VARIABLE code ERROR_VARIABLE err)
  set(p "")
  if(code EQUAL 3)
    set(p "holds a NUL byte")
  elseif(NOT code EQUAL 0)
    message(FATAL_ERROR "bytes canon ${src}: ${err}")
  else()
    file(READ "${dst}" t)
    string(FIND "${t}" "${HUD_RUN_ID}" at)
    if(at GREATER -1)
      set(p "holds the work directory in a form the driver does not replace")
    endif()
  endif()
  set(${problem} "${p}" PARENT_SCOPE)
endfunction()
# Shows a byte string on one line: escape as \e, tab as \t, newline as \n.
function(hud_visible s out)
  string(ASCII 27 esc)
  string(REPLACE "${esc}" "\\e" s "${s}")
  string(REPLACE "\t" "\\t" s "${s}")
  string(REPLACE "\n" "\\n" s "${s}")
  set(${out} "${s}" PARENT_SCOPE)
endfunction()

# A window of up to <Len> bytes of <S> from <From>, widened to whole UTF-8 characters.
function(hud_window s from len out)
  string(LENGTH "${s}" n)
  if(from GREATER n)
    set(from ${n})
  endif()
  math(EXPR to "${from} + ${len}")
  if(to GREATER n)
    set(to ${n})
  endif()
  foreach(edge from to)
    while(${edge} GREATER 0 AND ${edge} LESS n)
      string(SUBSTRING "${s}" ${${edge}} 1 c)
      string(HEX "${c}" h)
      if(NOT h MATCHES "^[89ab]")
        break()
      endif()
      math(EXPR ${edge} "${${edge}} - 1")
    endwhile()
  endforeach()
  math(EXPR k "${to} - ${from}")
  string(SUBSTRING "${s}" ${from} ${k} w)
  set(${out} "${w}" PARENT_SCOPE)
endfunction()

# The line of <S> from byte <From> on, at most a window, shown visibly.
function(hud_diff_line s from out)
  hud_window("${s}" ${from} 120 w)
  string(FIND "${w}" "\n" cut)
  if(cut GREATER -1)
    math(EXPR cut "${cut} + 1")
    string(SUBSTRING "${w}" 0 ${cut} w)
  endif()
  if(w STREQUAL "")
    set(w "(end)")
  endif()
  hud_visible("${w}" w)
  set(${out} "${w}" PARENT_SCOPE)
endfunction()

# Where <A> (expected) and <B> (actual) first differ, as the detail string for hud_fail: the line and
# byte, and both sides from shortly before that point to the end of the line.
function(hud_first_diff a b out)
  string(LENGTH "${a}" la)
  string(LENGTH "${b}" lb)
  set(lo 0)
  set(hi ${la})
  if(lb LESS la)
    set(hi ${lb})
  endif()
  while(lo LESS hi)
    math(EXPR mid "(${lo} + ${hi} + 1) / 2")
    string(SUBSTRING "${a}" 0 ${mid} pa)
    string(SUBSTRING "${b}" 0 ${mid} pb)
    if(pa STREQUAL pb)
      set(lo ${mid})
    else()
      math(EXPR hi "${mid} - 1")
    endif()
  endwhile()
  string(SUBSTRING "${a}" 0 ${lo} pre)
  string(REGEX MATCHALL "\n" nl "${pre}")
  list(LENGTH nl line)
  math(EXPR line "${line} + 1")
  string(FIND "${pre}" "\n" start REVERSE)
  math(EXPR start "${start} + 1")
  math(EXPR col "${lo} - ${start} + 1")
  set(from ${start})
  if(col GREATER 50)
    math(EXPR from "${lo} - 40")
  endif()
  hud_diff_line("${a}" ${from} wa)
  hud_diff_line("${b}" ${from} wb)
  set(${out} " at line ${line}, byte ${col}\n     expected ${wa}\n     actual   ${wb}" PARENT_SCOPE)
endfunction()
