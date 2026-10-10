# claude-hud-c

A C port of [claude-hud](https://github.com/jarrodwatts/claude-hud), the Claude Code status line by Jarrod Watts. Same
output, byte for byte, without node, in a few milliseconds per refresh.

Everything you see is claude-hud's: its display items, its two layouts, its options and its config file. This project
only reimplements it as one small native executable. For what the status line shows and what each option does, read
[claude-hud's README](https://github.com/jarrodwatts/claude-hud#readme).

With the default config, inside a git repository with uncommitted changes:

```
[Opus] │ my-project git:(main*)
Context █████░░░░░ 45% │ Usage ███░░░░░░░ 25% (resets in 1h 31m)
```

The same as claude-hud prints, except that the last line also ends with the refresh's run time in green, such as
` 6ms`.

claude-hud-c is an independent project, not affiliated with Anthropic or with claude-hud's author.

## Why

- **No node.** claude-hud runs on node; on a machine that has no other use for node, installing it just for the status
  line is a heavy dependency. claude-hud-c is one executable with no runtime to install.
- **Faster refreshes.** Claude Code reruns the status line command whenever the conversation updates. claude-hud starts
  node and loads its modules every time, and reads the whole session transcript again; claude-hud-c starts in a few
  milliseconds and only reads what was appended since the previous refresh.

## Performance

One refresh, median (p50) and 90th percentile in ms:

| Transcript | claude-hud-c, started directly | claude-hud-c, through sh | claude-hud, through sh |
| --- | --- | --- | --- |
| 0.3 MB | 7.0 / 13.6 | 34.1 / 48.8 | 133.7 / 139.5 |
| 10 MB | 9.5 / 21.7 | 33.8 / 50.3 | 198.0 / 208.9 |

"Through sh" is how Claude Code runs the command on Windows when Git Bash is set up; about 25 ms of it is sh itself.
claude-hud's time grows with the transcript, because it parses the whole file on every refresh; claude-hud-c's barely
moves.

Measured on Windows, claude-hud 0.10.0 on node 24.21.0, Git Bash's sh, git status display off;
40 rounds with the three forms alternated, CPU load 8% before and after; the two sh outputs were identical apart from
the run time claude-hud-c appends. To reproduce: `test/Measure-HudLatency.ps1` (Windows).

With git status display on, each refresh also waits for one git process, on both sides alike.

## Same output as claude-hud

Byte-identical output is the rule every change is held to, not an aim. The test scripts under `test/` run claude-hud
and claude-hud-c on the same input at a frozen clock and compare the status line and every file both write: all the
session transcripts on the machine crossed with sets of stdin, config, width and time; generated hostile transcripts
and configs; transcripts growing step by step to check the incremental parsing. Running them needs node 24 and a
claude-hud source tree of the version named in `test/common.mjs`, with its `dist/` built; each script's header says
how to call it.

Known gaps, where claude-hud-c renders English or an approximation instead:

- claude-hud's `language` option: only English is implemented, whatever it is set to. This is deliberate: the status
  line carries only a handful of words, too few to justify the extra machinery. Anyone who wants another language is
  welcome to add it; the strings sit in one table in `src/render.c`, and the output has to match claude-hud's own
  translation;
- `--extra-cmd` (Claude Code's `statusLine` cannot pass arguments anyway);
- a few inputs where claude-hud depends on node's locale data or throws, such as a system locale other than en-US or
  zh-CN, or a Windows path that is not drive-absolute. The source marks each one `UNSUPPORTED("<Reason>")`.

Two additions: the run time of each refresh at the end of the last line, and a parse-state file per transcript (next
section).

## Parse-state files

To read only what was appended since the previous refresh, claude-hud-c saves its parse state per transcript, as
`claude-hud-c/<TranscriptName>.bin` under the temporary directory: `%TEMP%` on Windows; on Linux and macOS the first
of `TMPDIR`, `TMP`, `TEMP` that is set, else `/tmp`. A long session's file is a few hundred KB.

This is claude-hud-c's own addition, not taken from claude-hud, and the reason its time in the table above stays flat
as a session grows. claude-hud is welcome to the idea: parse only what was appended, keep the parser's state between
refreshes, and trust a saved state only for the same file (by its identity) whose first block and the block before
the saved offset still hash the same.

The files are a cache: deleting them is always safe, and a missing or outdated one only costs a full parse of that
transcript on its next refresh, after which the state is saved again. A new build of claude-hud-c ignores the files an
earlier build wrote and replaces them as it goes.

- **Windows**: the temporary directory survives reboots, and claude-hud-c never deletes its files, so the directory
  grows by one file per transcript. Delete `%TEMP%\claude-hud-c` whenever you like.
- **Linux and macOS**: the temporary directory is usually emptied at boot (`/tmp` on tmpfs, `systemd-tmpfiles`) or
  periodically (macOS), so the first refresh of each session after that parses its transcript in full. Nothing else
  changes.

## Platforms

- **Windows**: the primary platform, Windows 10 or later. No C runtime is linked; the executable imports from
  `KERNEL32.dll` only.
- **Linux**: builds on the C library; tested under WSL2. Correct output, not tuned for start-up time.
- **macOS**: the code is written but has not been run on a Mac.

## Install

There are no prebuilt binaries; build from source. You need CMake 3.28 or later, Ninja, and clang with C23
support and lld (clang 18 is too old). Neither node nor claude-hud is needed to build or run it.

```
git clone https://github.com/yx64/claude-hud-c
cmake -S claude-hud-c -B <BuildDir> -G Ninja -DCMAKE_C_COMPILER=<Clang>
cmake --build <BuildDir>
ctest --test-dir <BuildDir> --output-on-failure
```

Copy `<BuildDir>/release/claude-hud-c.exe` (`claude-hud-c` off Windows) to a directory that is not on `PATH`, such as
`~/.local/tools/`, and point `statusLine` in the user-level `~/.claude/settings.json` at it:

```json
"statusLine": { "type": "command", "command": "~/.local/tools/claude-hud-c.exe" }
```

Use forward slashes: Claude Code runs the command through a shell, and Git Bash drops unquoted backslashes. That shell
also expands the `~`. To update, rename the old executable aside before copying the new one in, since a refresh may be
running it.

The config is claude-hud's, read from the same place, `~/.claude/plugins/claude-hud/config.json` (under
`CLAUDE_CONFIG_DIR` when that is set). An existing claude-hud config works as it is. To go back to claude-hud,
install the plugin and run `/claude-hud:setup`, which points `statusLine` at it again.

## Scope and direction

claude-hud-c follows claude-hud; it is not a fork with a direction of its own, and it is not meant to replace it.

- **No new features.** Feature requests belong in [claude-hud](https://github.com/jarrodwatts/claude-hud/issues). When
  claude-hud adds something, claude-hud-c may follow that release.
- **Performance work is not a feature.** It changes how fast the output comes, never what it is. The parse-state
  cache (see "Parse-state files") is one such change, designed here rather than taken from claude-hud.
- **Bug reports are welcome:** output that differs from claude-hud for the same input, crashes, wrong files written.
- **Performance ideas are welcome too**, from anyone who enjoys shaving off milliseconds, on one condition: no
  functionality is lost. A change has to keep the output byte-identical to claude-hud, which the comparison tests under
  `test/` check.
- **If claude-hud stops adding features, claude-hud-c will not add any on its own.** Should Claude Code ever ship a
  HUD of its own, that is outside this project.

## Reporting a bug

A bug is output that differs from what claude-hud prints for the same input, so the input is what makes a report
useful:

- the JSON Claude Code passed on stdin, and your `config.json`;
- what claude-hud-c printed, and what claude-hud prints for the same stdin, if you can run it;
- your platform, and the claude-hud-c commit you built.

Check them before attaching: the stdin holds paths and session ids, and a session transcript holds your whole
conversation, so do not attach one unless asked, and then cut it down to the lines that matter.

## Acknowledgements

- [claude-hud](https://github.com/jarrodwatts/claude-hud) by Jarrod Watts, MIT licensed: the display, the behaviour
  and the config format all come from it. The incremental transcript parsing is claude-hud-c's own.
- [yyjson](https://github.com/ibireme/yyjson) for JSON and [utf8proc](https://github.com/JuliaStrings/utf8proc) for
  Unicode, both included unmodified under `vendor/` with their licenses.

## License

MIT, see [LICENSE](LICENSE). Being a port of claude-hud, it keeps claude-hud's copyright notice next to its own. The
vendored libraries carry their own licenses next to them in `vendor/`.
