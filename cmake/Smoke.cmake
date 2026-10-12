# One synthetic refresh through the built executables. Usage:
#   cmake -DBIN=<BuildDir> -DWORK=<WorkDir> [-DCONFIG_DIR=<ConfigDir>] [-DTRACE=<TraceExe>] -P Smoke.cmake
# Checks, all at the real clock (the release build cannot freeze it):
#   - the test build exits 0; exit 3 (input it does not reproduce exactly) only warns and skips the comparisons;
#   - the release build exits 0, shows the model and ends its last line with the run time;
#   - with the run time taken off, the release build equals the test build at the same moment;
#   - the test build without parse state equals the test build, which by then resumes from its saved state;
#   - a trace build, when given, writes its trace line and its capture file beside itself.
# Two runs can straddle a tick of a displayed duration, so a comparison is retried twice before it fails.
# CONFIG_DIR is used as CLAUDE_CONFIG_DIR and must be a copy: the refreshes write their ledger into it. Without
# it a fixed config is written under WORK. TMP, TEMP and TMPDIR point under WORK, so saved parse states stay
# there. The executables carry .exe on a Windows host only.
# Quiet on success unless HUD_TEST_VERBOSE is set, like the drivers under test/; RESULT and STAMP, when
# given, are written as they write theirs (test/common.cmake).
foreach(v BIN WORK)
  if(NOT DEFINED ${v})
    message(FATAL_ERROR "Smoke.cmake: pass -D${v}=...")
  endif()
endforeach()
if(CMAKE_HOST_WIN32)
  set(exe_suffix .exe)
endif()
set(release ${BIN}/release/claude-hud-c${exe_suffix})
set(test ${BIN}/claude-hud-c-test${exe_suffix})
set(nostate ${BIN}/claude-hud-c-test-nostate${exe_suffix})
foreach(exe ${release} ${test} ${nostate})
  if(NOT EXISTS ${exe})
    message(FATAL_ERROR "smoke: missing ${exe}; build first")
  endif()
endforeach()
file(MAKE_DIRECTORY ${WORK}/tmp)
function(smoke_note text)
  if("$ENV{HUD_TEST_VERBOSE}" MATCHES "^(1|[Tt][Rr][Uu][Ee]|[Oo][Nn]|[Yy][Ee][Ss])$")
    message("${text}")
  endif()
endfunction()

if(NOT DEFINED CONFIG_DIR)
  set(CONFIG_DIR ${WORK}/claude)
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
set(transcript ${WORK}/transcript.jsonl)
file(WRITE ${transcript}
"{\"type\":\"user\",\"timestamp\":\"${now_iso}\",\"message\":{\"content\":\"冒烟 smoke 😀\"}}
{\"type\":\"assistant\",\"timestamp\":\"${now_iso}\",\"requestId\":\"req_1\",\"message\":{\"id\":\"msg_1\",\"model\":\"claude-opus-5-5\",\"usage\":{\"input_tokens\":12,\"output_tokens\":345,\"cache_creation_input_tokens\":6789,\"cache_read_input_tokens\":98765,\"cache_creation\":{\"ephemeral_5m_input_tokens\":6789}},\"content\":[{\"type\":\"tool_use\",\"id\":\"toolu_1\",\"name\":\"Read\",\"input\":{\"file_path\":\"C:\\\\项目\\\\很长的目录名\\\\file.txt\"}},{\"type\":\"tool_use\",\"id\":\"toolu_2\",\"name\":\"TodoWrite\",\"input\":{\"todos\":[{\"content\":\"第一步\",\"status\":\"completed\"},{\"content\":\"second step\",\"status\":\"in_progress\"}]}}]}}
{\"type\":\"user\",\"timestamp\":\"${now_iso}\",\"message\":{\"content\":[{\"type\":\"tool_result\",\"tool_use_id\":\"toolu_1\",\"content\":\"ok\"}]}}
{\"type\":\"assistant\",\"timestamp\":\"${now_iso}\",\"requestId\":\"req_2\",\"message\":{\"id\":\"msg_2\",\"model\":\"claude-opus-5-5\",\"usage\":{\"input_tokens\":3,\"output_tokens\":40,\"cache_read_input_tokens\":105000},\"content\":[{\"type\":\"tool_use\",\"id\":\"toolu_3\",\"name\":\"PowerShell\",\"input\":{\"command\":\"Get-ChildItem\"}}]}}
")
file(TO_NATIVE_PATH ${transcript} transcript_native)
file(TO_NATIVE_PATH ${WORK} work_native)
string(REPLACE "\\" "\\\\" transcript_json "${transcript_native}")
string(REPLACE "\\" "\\\\" work_json "${work_native}")
set(stdin ${WORK}/stdin.json)
file(WRITE ${stdin} "{\"session_id\":\"smoke\",\"transcript_path\":\"${transcript_json}\",\"cwd\":\"${work_json}\",\"version\":\"2.1.300\",\"session_name\":\"smoke\",\"model\":{\"id\":\"claude-opus-5-5[1m]\",\"display_name\":\"Opus 5.5\"},\"workspace\":{\"current_dir\":\"${work_json}\",\"project_dir\":\"${work_json}\"},\"cost\":{\"total_cost_usd\":1.2345,\"total_duration_ms\":600000},\"context_window\":{\"context_window_size\":1000000,\"used_percentage\":11,\"current_usage\":{\"input_tokens\":3,\"cache_read_input_tokens\":105000}},\"rate_limits\":{\"five_hour\":{\"used_percentage\":42,\"resets_at\":${resets}}},\"prompt_cache\":{\"caching_observed\":true,\"warm\":true,\"expires_at\":${expires},\"ttl\":\"5m\",\"hit_ratio\":0.9},\"effort\":{\"level\":\"high\"}}")

# The ledger is put back after every run, so each run starts from the same state.
set(ledger ${CONFIG_DIR}/plugins/claude-hud/daily-cost.json)
function(refresh exe out_var code_var)
  set(had_ledger FALSE)
  if(EXISTS ${ledger})
    set(had_ledger TRUE)
    file(COPY_FILE ${ledger} ${WORK}/ledger.saved)
  endif()
  execute_process(
    COMMAND ${CMAKE_COMMAND} -E env --unset=HUD_FAKE_NOW_MS --unset=HUD_FAKE_MEM --unset=HUD_CACHE_DIR --unset=COLUMNS
            CLAUDE_CONFIG_DIR=${CONFIG_DIR} TMP=${WORK}/tmp TEMP=${WORK}/tmp TMPDIR=${WORK}/tmp ${exe}
    INPUT_FILE ${stdin} OUTPUT_VARIABLE out ERROR_VARIABLE err RESULT_VARIABLE code)
  if(had_ledger)
    file(COPY_FILE ${WORK}/ledger.saved ${ledger})
  endif()
  set(${out_var} "${out}" PARENT_SCOPE)
  set(${code_var} "${code}" PARENT_SCOPE)
  set(refresh_err "${err}" PARENT_SCOPE)
endfunction()

string(ASCII 27 esc)
set(run_time " ${esc}\\[32m[0-9]+ms${esc}\\[0m\n$")

refresh(${test} t0 t0_code)
if(t0_code EQUAL 3)
  string(STRIP "${refresh_err}" why)
  message(WARNING "smoke: the config has parts this port does not reproduce exactly; the release build renders them anyway, possibly unlike upstream: ${why}")
elseif(NOT t0_code EQUAL 0)
  message(FATAL_ERROR "smoke: test build exited ${t0_code}: ${t0} ${refresh_err}")
endif()

refresh(${release} r r_code)
if(NOT r_code EQUAL 0 OR NOT r MATCHES "Opus 5\\.5")
  message(FATAL_ERROR "smoke: release build exited ${r_code}: ${r} ${refresh_err}")
endif()
if(NOT r MATCHES "${run_time}")
  message(FATAL_ERROR "smoke: release build output does not end with the run time: ${r}")
endif()

if(t0_code EQUAL 0)
  foreach(pair "release;${release};1" "test without parse state;${nostate};0")
    list(GET pair 0 name)
    list(GET pair 1 exe)
    list(GET pair 2 strip)
    set(same FALSE)
    foreach(try 1 2 3)
      refresh(${exe} a a_code)
      refresh(${test} b b_code)
      if(strip)
        string(REGEX REPLACE "${run_time}" "\n" a "${a}")
      endif()
      if(a_code EQUAL 0 AND b_code EQUAL 0 AND a STREQUAL b)
        set(same TRUE)
        break()
      endif()
    endforeach()
    if(NOT same)
      message(FATAL_ERROR "smoke: ${name} and test build differ at the same moment\n--- ${name}\n${a}--- test\n${b}")
    endif()
    smoke_note("smoke: ${name} equals the test build")
  endforeach()
endif()

if(DEFINED TRACE)
  get_filename_component(trace_dir ${TRACE} DIRECTORY)
  refresh(${TRACE} tr tr_code)
  file(GLOB captured ${trace_dir}/capture/*.json)
  if(NOT tr_code EQUAL 0 OR NOT EXISTS ${trace_dir}/claude-hud-c-trace.log OR NOT captured)
    message(FATAL_ERROR "smoke: trace build exited ${tr_code} or wrote no claude-hud-c-trace.log or capture file in ${trace_dir}")
  endif()
  # They are only the smoke test's; moved aside rather than deleted, so they do not mix with the records a live
  # session writes once installed.
  string(TIMESTAMP stamp "%Y%m%d%H%M%S")
  file(MAKE_DIRECTORY ${WORK}/moved/${stamp})
  file(RENAME ${trace_dir}/claude-hud-c-trace.log ${WORK}/moved/${stamp}/claude-hud-c-trace.log)
  file(RENAME ${trace_dir}/capture ${WORK}/moved/${stamp}/capture)
  smoke_note("smoke: trace build wrote its trace line and capture file")
endif()

if(DEFINED RESULT)
  file(WRITE ${RESULT} "smoke 1 1 0\n")
endif()
if(DEFINED STAMP)
  file(TOUCH ${STAMP})
endif()