<#
.SYNOPSIS
    Times one status line refresh: claude-hud-c directly and through sh, alternated; with
    -UpstreamDir also the upstream HUD through sh, checking that the two sh outputs are identical
    apart from the run time claude-hud-c appends to its last line.

.DESCRIPTION
    Each round runs the forms in alternating order with the same stdin, built from the given
    transcript. Reports p50 and p90 per form, and whole-machine CPU load before and after:
    figures taken while another session builds or tests are not comparable.

    sh is the one Claude Code uses: CLAUDE_CODE_GIT_BASH_PATH from the environment or from
    env in the user-level settings.json. The upstream HUD runs the way its own launcher runs it
    (run-upstream.mjs beside this script).

    The refreshes write the HUD's runtime files (daily-cost ledger) as live refreshes do; the
    stdin uses its own session id, so the live sessions' ledger entries are not changed.

.PARAMETER Transcript
    Session transcript to refresh against, such as
    <UserHome>\.claude\projects\<ProjectDirName>\<SessionId>.jsonl.

.PARAMETER HudExe
    The release build to time, in the form <Drive>:\<Dir>\<ProjectName>.exe.

.PARAMETER Rounds
    Measured rounds; one warm-up round is added and discarded.

.PARAMETER UpstreamDir
    Upstream claude-hud source tree with its built dist\, in the form <Dir>. Optional; nothing is
    looked up when it is left out, and the upstream form is not timed.

.EXAMPLE
    .\Measure-HudLatency.ps1 -Transcript $TranscriptPath -HudExe $HudExe

.EXAMPLE
    .\Measure-HudLatency.ps1 -Transcript $TranscriptPath -HudExe $HudExe -Rounds 50 -UpstreamDir $UpstreamDir
#>

#requires -Version 7.1

[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidateNotNullOrEmpty()][string]$Transcript,
    [Parameter(Mandatory)][ValidateNotNullOrEmpty()][string]$HudExe,
    [ValidateRange(1, 1000)][int]$Rounds = 30,
    [string]$UpstreamDir
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $true

$Transcript = (Resolve-Path -LiteralPath $Transcript).ProviderPath
$HudExe = (Resolve-Path -LiteralPath $HudExe).ProviderPath
$sh = $env:CLAUDE_CODE_GIT_BASH_PATH
if (-not $sh) {
    $settings = Join-Path $HOME '.claude' 'settings.json'
    $j = Get-Content -LiteralPath $settings -Raw | ConvertFrom-Json -AsHashtable
    $sh = $j['env']?['CLAUDE_CODE_GIT_BASH_PATH']
}
if (-not $sh -or -not (Test-Path -LiteralPath $sh -PathType Leaf)) { throw 'CLAUDE_CODE_GIT_BASH_PATH is not set or the file is missing; statusLine then runs through PowerShell, which this script does not time' }
# sh takes backslashes as escapes, so paths are written with forward slashes.
$hudForSh = $HudExe -replace '\\', '/'

$stdin = [ordered]@{
    session_id = 'measure-hud-latency'; transcript_path = $Transcript; cwd = (Get-Location).Path
    model = @{ id = 'claude-opus-5-5[1m]'; display_name = 'Opus 5.5' }
    context_window = @{ context_window_size = 1000000; used_percentage = 5; current_usage = @{ input_tokens = 10; cache_read_input_tokens = 49000 } }
} | ConvertTo-Json -Depth 5 -Compress

$forms = [ordered]@{
    'claude-hud-c'       = @($HudExe, '')
    'sh -> claude-hud-c' = @($sh, "-c `"$hudForSh`"")
}
if ($UpstreamDir) {
    $UpstreamDir = (Resolve-Path -LiteralPath $UpstreamDir).ProviderPath
    if (-not (Test-Path -LiteralPath (Join-Path $UpstreamDir 'dist\index.js') -PathType Leaf)) { throw "No dist\index.js in the upstream directory; build it first: $UpstreamDir" }
    $nodeLine = "node '$((Join-Path $PSScriptRoot 'run-upstream.mjs') -replace '\\', '/')' '$($UpstreamDir -replace '\\', '/')'"
    $forms['sh -> node'] = @($sh, "-c `"$nodeLine`"")
}

function Invoke-Timed([string]$Exe, [string]$Arguments) {
    $psi = [Diagnostics.ProcessStartInfo]::new($Exe, $Arguments)
    $psi.UseShellExecute = $false
    $psi.RedirectStandardInput = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $p = [Diagnostics.Process]::Start($psi)
    $err = $p.StandardError.ReadToEndAsync()
    $p.StandardInput.Write($stdin)
    $p.StandardInput.Close()
    $out = $p.StandardOutput.ReadToEnd()
    $p.WaitForExit()
    $sw.Stop()
    if ($p.ExitCode) { throw "$Exe $Arguments exited with $($p.ExitCode): $($err.Result)" }
    [pscustomobject]@{ Ms = $sw.Elapsed.TotalMilliseconds; Out = $out }
}

function Get-Load { [math]::Round((Get-CimInstance Win32_Processor | Measure-Object LoadPercentage -Average).Average) }

$loadBefore = Get-Load
$keys = @($forms.Keys)
$samples = @{}
foreach ($k in $keys) { $samples[$k] = [Collections.Generic.List[double]]::new() }
$last = @{}
for ($r = 0; $r -le $Rounds; $r++) {
    $order = ($r % 2) ? $keys[($keys.Count - 1)..0] : $keys
    foreach ($k in $order) {
        # A short gap between refreshes, so one process's exit does not overlap the next timing.
        Start-Sleep -Milliseconds 50
        $res = Invoke-Timed $forms[$k][0] $forms[$k][1]
        if ($r -gt 0) { $samples[$k].Add($res.Ms) }
        $last[$k] = $res.Out
    }
}
$loadAfter = Get-Load

foreach ($k in $keys) {
    $s = @($samples[$k] | Sort-Object)
    [pscustomobject]@{
        Form  = $k
        P50Ms = [math]::Round($s[[int][math]::Floor(($s.Count - 1) / 2)], 1)
        P90Ms = [math]::Round($s[[int][math]::Floor(($s.Count - 1) * 0.9)], 1)
    }
}
$same = 'not compared'
if ($UpstreamDir) {
    # What is timed is the build to be installed, which appends its run time to the last line; that is not
    # upstream's output, so it is taken off before comparing with the node side character by character. The two
    # do not run at the same moment, so a clock tick across a displayed unit can make them differ.
    $same = ($last['sh -> claude-hud-c'] -replace ' \x1b\[32m\d+ms\x1b\[0m\n$', "`n") -ceq $last['sh -> node']
}
Write-Host ('transcript {0:N1} MB; CPU load {1}% before, {2}% after; sh outputs identical: {3}' -f
    ((Get-Item -LiteralPath $Transcript).Length / 1MB), $loadBefore, $loadAfter, $same)
