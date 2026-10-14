# One synthetic refresh through the built executables. Usage:
#   cmake -DBIN=<BuildDir> [-DWORK=<WorkDir>] [-DCONFIG_DIR=<ConfigDir>] [-DTRACE=<TraceExe>] -P Smoke.cmake
# Checks, all at the real clock (the release build cannot freeze it), reported as the drivers under test/
# report (test/common.cmake), group smoke:
#   test-build           the test build exits 0; exit 3 (input it does not reproduce exactly) skips the
#                        comparison with a warning instead;
#   release              the release build exits 0, shows the model and ends its last line with the run time;
#   release-equals-test  with the run time taken off, the release build equals the test build at the same moment;
#   trace                a trace build, when given, writes its trace line and its capture file beside itself.
# Two runs can straddle a tick of a displayed duration, so the comparison is retried twice before it fails.
# Each run works in a fresh directory: under WORK when given, else under the build directory's test-work.
# CONFIG_DIR is used as CLAUDE_CONFIG_DIR and must be a copy: the refreshes write their ledger into it. Without
# it a fixed config is written into the work directory. TMP, TEMP and TMPDIR point under the work directory,
# so saved parse states stay there. The rest of the caller's environment is kept: both builds see the same, and
# run against a copy of a real config, that environment is part of what is checked.
if(NOT DEFINED GROUP)
  set(GROUP smoke)
endif()
include(${CMAKE_CURRENT_LIST_DIR}/../test/common.cmake)

hud_selected(refresh sel)
if(NOT sel)
  hud_finish()
  return()
endif()

set(release ${BIN}/release/claude-hud-c${HUD_EXE_SUFFIX})
set(test ${BIN}/claude-hud-c-test${HUD_EXE_SUFFIX})
foreach(exe ${release} ${test})
  if(NOT EXISTS ${exe})
    message(FATAL_ERROR "Smoke.cmake: missing ${exe}; build first")
  endif()
endforeach()
if(DEFINED WORK)
  set(work ${WORK}/${HUD_RUN_ID})
else()
  set(work ${HUD_RUN}/smoke)
endif()
file(MAKE_DIRECTORY ${work}/tmp)

if(NOT DEFINED CONFIG_DIR)
  set(CONFIG_DIR ${work}/claude)
  file(WRITE ${CONFIG_DIR}/plugins/claude-hud/config.json [=[
{
  "gitStatus": { "enabled": false },
  "display": {
    "showConfigCounts": true, "showCost": true, "showDailyCost": true, "showDuration": true,
    "showTools": true, "showTodos": true, "showSessionName": true, "showClaudeCodeVersion": true,
    "showEffortLevel": true, "showPromptCache": true, "showCacheHitRate": true, "showSessionTokens": true,
    "contextValue": "both", "timeFormat": "both", "hourCycle": "h23"
  }
}
]=])
endif()

# Non-ASCII on purpose: wide characters, an emoji, a path with non-ASCII directory names.
string(TIMESTAMP now_s "%s" UTC)
string(TIMESTAMP now_iso "%Y-%m-%dT%H:%M:%S.000Z" UTC)
math(EXPR resets "${now_s} + 5000")
math(EXPR expires "${now_s} + 200")
set(transcript ${work}/transcript.jsonl)
file(WRITE ${transcript}
"{\"type\":\"user\",\"timestamp\":\"${now_iso}\",\"message\":{\"content\":\"冒烟 smoke 😀\"}}
{\"type\":\"assistant\",\"timestamp\":\"${now_iso}\",\"requestId\":\"req_1\",\"message\":{\"id\":\"msg_1\",\"model\":\"claude-opus-5-5\",\"usage\":{\"input_tokens\":12,\"output_tokens\":345,\"cache_creation_input_tokens\":6789,\"cache_read_input_tokens\":98765,\"cache_creation\":{\"ephemeral_5m_input_tokens\":6789}},\"content\":[{\"type\":\"tool_use\",\"id\":\"toolu_1\",\"name\":\"Read\",\"input\":{\"file_path\":\"C:\\\\项目\\\\很长的目录名\\\\file.txt\"}},{\"type\":\"tool_use\",\"id\":\"toolu_2\",\"name\":\"TodoWrite\",\"input\":{\"todos\":[{\"content\":\"第一步\",\"status\":\"completed\"},{\"content\":\"second step\",\"status\":\"in_progress\"}]}}]}}
{\"type\":\"user\",\"timestamp\":\"${now_iso}\",\"message\":{\"content\":[{\"type\":\"tool_result\",\"tool_use_id\":\"toolu_1\",\"content\":\"ok\"}]}}
{\"type\":\"assistant\",\"timestamp\":\"${now_iso}\",\"requestId\":\"req_2\",\"message\":{\"id\":\"msg_2\",\"model\":\"claude-opus-5-5\",\"usage\":{\"input_tokens\":3,\"output_tokens\":40,\"cache_read_input_tokens\":105000},\"content\":[{\"type\":\"tool_use\",\"id\":\"toolu_3\",\"name\":\"PowerShell\",\"input\":{\"command\":\"Get-ChildItem\"}}]}}
")
file(TO_NATIVE_PATH ${transcript} transcript_native)
file(TO_NATIVE_PATH ${work} work_native)
string(REPLACE "\\" "\\\\" transcript_json "${transcript_native}")
string(REPLACE "\\" "\\\\" work_json "${work_native}")
set(stdin ${work}/stdin.json)
file(WRITE ${stdin} "{\"session_id\":\"smoke\",\"transcript_path\":\"${transcript_json}\",\"cwd\":\"${work_json}\",\"version\":\"2.1.300\",\"session_name\":\"smoke\",\"model\":{\"id\":\"claude-opus-5-5[1m]\",\"display_name\":\"Opus 5.5\"},\"workspace\":{\"current_dir\":\"${work_json}\",\"project_dir\":\"${work_json}\"},\"cost\":{\"total_cost_usd\":1.2345,\"total_duration_ms\":600000},\"context_window\":{\"context_window_size\":1000000,\"used_percentage\":11,\"current_usage\":{\"input_tokens\":3,\"cache_read_input_tokens\":105000}},\"rate_limits\":{\"five_hour\":{\"used_percentage\":42,\"resets_at\":${resets}}},\"prompt_cache\":{\"caching_observed\":true,\"warm\":true,\"expires_at\":${expires},\"ttl\":\"5m\",\"hit_ratio\":0.9},\"effort\":{\"level\":\"high\"}}")

# The ledger is put back after every run, so each run starts from the same state. A refresh still running after
# HUD_RUN_TIMEOUT is hung; execute_process ends it and reports a timeout as its result, which fails the check
# that reads it.
set(ledger ${CONFIG_DIR}/plugins/claude-hud/daily-cost.json)
function(refresh exe out_var code_var)
  set(had_ledger FALSE)
  if(EXISTS ${ledger})
    set(had_ledger TRUE)
    file(COPY_FILE ${ledger} ${work}/ledger.saved)
  endif()
  execute_process(
    COMMAND ${CMAKE_COMMAND} -E env --unset=HUD_FAKE_NOW_MS --unset=HUD_FAKE_MEM --unset=HUD_CACHE_DIR --unset=COLUMNS
            CLAUDE_CONFIG_DIR=${CONFIG_DIR} TMP=${work}/tmp TEMP=${work}/tmp TMPDIR=${work}/tmp ${exe}
    INPUT_FILE ${stdin} OUTPUT_VARIABLE out ERROR_VARIABLE err RESULT_VARIABLE code TIMEOUT ${HUD_RUN_TIMEOUT})
  if(had_ledger)
    file(COPY_FILE ${work}/ledger.saved ${ledger})
  endif()
  set(${out_var} "${out}" PARENT_SCOPE)
  set(${code_var} "${code}" PARENT_SCOPE)
  string(STRIP "${err}" err)
  set(refresh_err "${err}" PARENT_SCOPE)
endfunction()

string(ASCII 27 esc)
set(run_time " ${esc}\\[32m[0-9]+ms${esc}\\[0m\n$")

refresh(${test} t0 t0_code)
set(compare FALSE)
if(t0_code EQUAL 3)
  hud_skip(release-equals-test "the config has parts the test build does not reproduce exactly (${refresh_err}); the release build renders them anyway, possibly unlike upstream")
elseif(NOT t0_code EQUAL 0)
  hud_fail(test-build "exited ${t0_code}" "\n     stderr ${refresh_err}")
else()
  hud_pass(test-build)
  set(compare TRUE)
endif()

refresh(${release} r r_code)
hud_visible("${r}" r_shown)
if(NOT r_code EQUAL 0)
  hud_fail(release "exited ${r_code}" "\n     stderr ${refresh_err}")
elseif(NOT r MATCHES "Opus 5\\.5")
  hud_fail(release "the output does not show the model" "\n     output ${r_shown}")
elseif(NOT r MATCHES "${run_time}")
  hud_fail(release "the last line does not end with the run time" "\n     output ${r_shown}")
else()
  hud_pass(release)
endif()

if(compare)
  set(same FALSE)
  foreach(try 1 2 3)
    refresh(${release} a a_code)
    refresh(${test} b b_code)
    string(REGEX REPLACE "${run_time}" "\n" a "${a}")
    if(a_code EQUAL 0 AND b_code EQUAL 0 AND a STREQUAL b)
      set(same TRUE)
      break()
    endif()
  endforeach()
  if(same)
    hud_pass(release-equals-test)
  else()
    hud_first_diff("${b}" "${a}" d)
    hud_fail(release-equals-test "the release build (run time taken off) differs from the test build, exits ${a_code} and ${b_code}"
             "${d}\n     (expected: test build, actual: release build)")
  endif()
endif()

if(DEFINED TRACE)
  get_filename_component(trace_dir ${TRACE} DIRECTORY)
  refresh(${TRACE} tr tr_code)
  file(GLOB captured ${trace_dir}/capture/*.json)
  if(NOT tr_code EQUAL 0 OR NOT EXISTS ${trace_dir}/claude-hud-c-trace.log OR NOT captured)
    hud_fail(trace "exited ${tr_code}, or wrote no claude-hud-c-trace.log or capture file in ${trace_dir}" "\n     stderr ${refresh_err}")
  else()
    # They are only the smoke test's; moved aside rather than deleted, so they do not mix with the records a live
    # session writes once installed.
    file(MAKE_DIRECTORY ${work}/moved)
    file(RENAME ${trace_dir}/claude-hud-c-trace.log ${work}/moved/claude-hud-c-trace.log)
    file(RENAME ${trace_dir}/capture ${work}/moved/capture)
    hud_pass(trace)
  endif()
endif()

hud_finish()
