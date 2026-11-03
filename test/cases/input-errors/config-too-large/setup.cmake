# A config.json past 64 KB: a valid object padded with spaces, so only its size is wrong.
string(REPEAT " " 66000 pad)
file(WRITE ${HUD_DIR}/claude/plugins/claude-hud/config.json "{${pad}}")
