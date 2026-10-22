# The subagents' own transcripts beside the session's: the first assistant record's effort counts, after
# lines that are not one; the third agent has none.
set(sub ${HUD_DIR}/transcript/subagents)
file(MAKE_DIRECTORY ${sub})
file(WRITE ${sub}/agent-a1.jsonl [=[{"type":"user","effort":"low"}
not json
{"type":"assistant","effort":"high","message":{"content":[]}}
{"type":"assistant","effort":"low"}
]=])
file(WRITE ${sub}/agent-a2.jsonl [=[{"type":"assistant","effort":"MAX ","message":{"content":[]}}
]=])
