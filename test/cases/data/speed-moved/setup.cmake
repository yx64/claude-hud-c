# The speed cache an older release left beside data/, named by the transcript's hash: moved into data/
# and taken up, so the speed comes from the 51 s of API time since then.
string(SHA256 sha "${HUD_NATIVE_TRANSCRIPT}")
file(WRITE ${HUD_DIR}/claude/plugins/claude-hud/speed-cache/${sha}.json "{\"apiDurationMs\":40000,\"usageKey\":\"1:1:1:1\",\"speed\":null}")
