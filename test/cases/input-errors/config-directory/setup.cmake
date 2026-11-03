# config.json is a directory: upstream opens it on every platform and finds no regular file.
file(REMOVE ${HUD_DIR}/claude/plugins/claude-hud/config.json)
file(MAKE_DIRECTORY ${HUD_DIR}/claude/plugins/claude-hud/config.json)
