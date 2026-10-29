<#
.SYNOPSIS
    Karp map regressions: runs the bot on several maps at once and reports how each run ended.

.DESCRIPTION
    Each map is a visible game (-autoplay -karpconsole -nomonsters -quitonexit -nosound), all
    started together. crispy keeps karp.log, karp.cfg and crispy-doom.cfg in the exe's folder
    (its config directory on Windows), so instances sharing one folder would overwrite each
    other's files: each map runs from its own folder holding a copy of the exe, its DLLs and
    the cfgs. A map still running after its time limit is killed.

    Afterwards each run's karp.log is in <Out>\logs\MAPnn.log, and one line per map is printed:
    the time it finished in, the keys picked up and when, and the first failed goal of a map that
    did not finish.

    The default maps are the Doom II regression set, all of which finish: MAP01, MAP02, MAP03,
    MAP05, MAP06, MAP08, MAP09, MAP10, MAP11, MAP12, MAP13, MAP14, MAP15, MAP16, MAP17, MAP18,
    MAP19, MAP20, MAP21, MAP22, MAP24, MAP25, MAP26, MAP27, MAP28, MAP29 and MAP31.

.PARAMETER Iwad
    The Doom II IWAD. Defaults to the DOOM2_IWAD environment variable.

.PARAMETER Maps
    The maps to run, as -warp takes them ('01', '17', ...).

.PARAMETER Seconds
    Time limit per map, unless Limits names it.

.PARAMETER Limits
    Per-map time limits in seconds, e.g. @{ '19' = 150 }. MAP13 takes about 93 s, MAP09 96 s,
    MAP19 101 s, MAP17 153 s and MAP29 163 s, so they get 150, 150, 150, 200 and 200 by default;
    the rest take under 81 s.

.PARAMETER Build
    The folder with crispy-doom.exe and its DLLs. Defaults to build\src\Release in this repo.

.PARAMETER Out
    Where the run folders and logs go. Defaults to build\karp-regress in this repo.

.EXAMPLE
    .\karp\tools\regress.ps1 -Iwad path\to\DOOM2.WAD

.EXAMPLE
    .\karp\tools\regress.ps1 -Maps '24' -Seconds 150
#>
param(
    [string]$Iwad = $env:DOOM2_IWAD,
    [string[]]$Maps = @('01', '02', '03', '05', '06', '08', '09', '10', '11', '12', '13', '14', '15',
                        '16', '17', '18', '19', '20', '21', '22', '24', '25', '26', '27', '28', '29',
                        '31'),
    [int]$Seconds = 120,
    [hashtable]$Limits = @{ '09' = 150; '13' = 150; '17' = 200; '19' = 150; '29' = 200 },
    [string]$Build = (Join-Path $PSScriptRoot '..\..\build\src\Release'),
    [string]$Out = (Join-Path $PSScriptRoot '..\..\build\karp-regress')
)

$ErrorActionPreference = 'Stop'

if (-not $Iwad -or -not (Test-Path $Iwad)) {
    throw 'No IWAD: pass -Iwad, or set DOOM2_IWAD.'
}

if (-not (Test-Path (Join-Path $Build 'crispy-doom.exe'))) {
    throw "No crispy-doom.exe in $Build - build it first, or pass -Build."
}

$Iwad = (Resolve-Path $Iwad).Path
$Build = (Resolve-Path $Build).Path
$runs = Join-Path $Out 'runs'
$logs = Join-Path $Out 'logs'
New-Item -ItemType Directory -Force -Path $runs, $logs | Out-Null

$started = @{}

foreach ($map in $Maps) {
    $dir = Join-Path $runs "MAP$map"

    if (Test-Path $dir) {
        Remove-Item $dir -Recurse -Force
    }

    New-Item -ItemType Directory -Path $dir | Out-Null
    Copy-Item (Join-Path $Build 'crispy-doom.exe'), (Join-Path $Build '*.dll') $dir

    foreach ($cfg in 'crispy-doom.cfg', 'default.cfg', 'karp.cfg') {
        if (Test-Path (Join-Path $Build $cfg)) {
            Copy-Item (Join-Path $Build $cfg) $dir
        }
    }

    $started[$map] = Start-Process -FilePath (Join-Path $dir 'crispy-doom.exe') -WorkingDirectory $dir `
        -PassThru -ArgumentList '-iwad', "`"$Iwad`"", '-window', '-nomouse', '-nosound', '-nomonsters',
                                '-autoplay', '-karpconsole', '-quitonexit', '-warp', $map
}

$clock = [Diagnostics.Stopwatch]::StartNew()

foreach ($map in $Maps) {
    $limit = if ($Limits.ContainsKey($map)) { $Limits[$map] } else { $Seconds }
    $left = [Math]::Max(0, $limit * 1000 - $clock.ElapsedMilliseconds)
    $process = $started[$map]

    if ($process.WaitForExit([int]$left)) {
        $end = 'exited'
    }
    else {
        Stop-Process -Id $process.Id -Force
        $end = 'killed'
    }

    $process.WaitForExit(5000) | Out-Null

    $log = Join-Path $logs "MAP$map.log"
    $source = Join-Path $runs "MAP$map\karp.log"

    if (-not (Test-Path $source)) {
        "MAP$map ($end): no karp.log"
        continue
    }

    Copy-Item $source $log
    $finish = Select-String -Path $log -Pattern 'level finished in ([0-9.]+) s' | Select-Object -First 1
    $keys = Select-String -Path $log -Pattern '^(\S+) BRAIN\s+GET (\w+) KEY done' | ForEach-Object {
        '{0} at {1}' -f $_.Matches[0].Groups[2].Value.ToLower(), $_.Matches[0].Groups[1].Value
    }
    $failure = Select-String -Path $log -Pattern 'failed - ' | Select-Object -First 1

    $text = if ($finish) { 'finished in {0} s' -f $finish.Matches[0].Groups[1].Value } else { 'not finished' }

    if ($keys) {
        $text += '; keys: ' + ($keys -join ', ')
    }

    if (-not $finish -and $failure) {
        $text += '; first failure: ' + $failure.Line
    }

    "MAP$map ($end): $text"
}

'all done in {0:N0} s - logs in {1}' -f $clock.Elapsed.TotalSeconds, (Resolve-Path $logs).Path
