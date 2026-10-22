# The subagents' own transcripts are read 64 KiB at a time: a line end split across two reads (U+2028,
# U+2029), a \r as the last byte of a read, and lines longer than a read before the first reply.
foreach(sub a1 a2 a3)
  hud_copy_subst(${CMAKE_CURRENT_LIST_DIR}/agent-${sub}.jsonl ${HUD_DIR}/transcript/subagents/agent-${sub}.jsonl)
endforeach()
