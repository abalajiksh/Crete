<#
.SYNOPSIS
    Install what crête needs to build, on Windows.

.DESCRIPTION
    crête's build is GNU make + g++ + pkg-config, so on Windows the realistic
    toolchain is MSYS2's MinGW-w64. This script installs MSYS2 (via winget, or
    Chocolatey/Scoop if you already use one) and then pacman's the packages for
    the tiers you ask for.

    Tiers, and what each adds:

        cli      mingw-w64 g++/gcc, make                (the zero-dependency CLI)
        json     + curl        (the Makefile fetches nlohmann/json single-header)
        ffmpeg   + pkgconf, nasm, git, diffutils   (builds the compact FFmpeg)
        gui      + mingw-w64 SDL2, git             (SDL2 + Dear ImGui)

    THE ONE THING THAT MATTERS ON WINDOWS is the MinGW threading model. The
    win32 model ships a libstdc++ with no std::thread at all, and crête's
    parallel per-file decode is its DEFAULT path -- such a build links clean and
    then throws "Enable multithreading to use std::thread" on the first album.
    MSYS2's mingw-w64-x86_64-gcc is the posix model, which works with zero
    source changes. This script checks the model and says so.

    ALSO: build Windows binaries with STATIC=1. Without it the .exe needs
    libstdc++-6.dll, libgcc_s_seh-1.dll and libwinpthread-1.dll beside it,
    which breaks crête's single-self-contained-executable promise. With it the
    only imports are KERNEL32 plus the Universal CRT stubs that ship with
    Windows 10+.

.PARAMETER Tier
    Which tiers to set up. Repeatable. 'all' selects every tier.
    Default: cli, json, ffmpeg, gui.

.PARAMETER Check
    Report what is present and what is missing; install nothing.

.PARAMETER DryRun
    Print the exact commands that would run, execute none.

.PARAMETER Yes
    Do not prompt before installing.

.PARAMETER Fetch
    Also fetch Dear ImGui / nlohmann-json and build the compact FFmpeg.
    Slow (several minutes for FFmpeg).

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\setup-deps.ps1 -Check

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\setup-deps.ps1 -Tier cli -Tier json -Yes

.NOTES
    Installing MSYS2 needs Administrator; the pacman steps do not. If you
    already have MSYS2, run this from an ordinary prompt and it will skip
    straight to pacman.
#>

[CmdletBinding()]
param(
    # Validated here so an unknown tier cannot reach "Check-<Tier>" as a
    # nonexistent function and fail with a confusing CommandNotFound.
    # Note there is no 'cross' tier on Windows: MinGW-w64 IS the native
    # toolchain here, which is the 'cli' tier.
    [ValidateSet('cli','json','ffmpeg','gui','all')]
    [string[]]$Tier,
    [switch]$Check,
    [switch]$DryRun,
    [switch]$Yes,
    [switch]$Fetch
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)

if (-not $Tier -or $Tier.Count -eq 0) { $Tier = @('cli','json','ffmpeg','gui') }
if ($Tier -contains 'all')            { $Tier = @('cli','json','ffmpeg','gui') }

function Want([string]$t) { return $Tier -contains $t }

# ── Output ──────────────────────────────────────────────────────────────────
function Say   ([string]$m) { Write-Host $m }
function Head2 ([string]$m) { Write-Host ""; Write-Host "== $m" -ForegroundColor White }
function OK    ([string]$m) { Write-Host "  ok    $m" -ForegroundColor Green }
function Miss  ([string]$m) { Write-Host "  MISS  $m" -ForegroundColor Red }
function Note  ([string]$m) { Write-Host "  ..    $m" -ForegroundColor Yellow }

function Have([string]$cmd) { return [bool](Get-Command $cmd -ErrorAction SilentlyContinue) }

# ── Locate MSYS2 ────────────────────────────────────────────────────────────
# Checked by path rather than by asking a package manager, because MSYS2 is
# just as often installed by hand or carried over from another machine.
function Find-Msys2 {
    # @(...) around the WHOLE pipeline: a pipeline yielding one item unrolls to
    # a bare string, and indexing a string gives a CHARACTER -- 'C:\msys64'[0]
    # is 'C', which is exactly what the first real run reported.
    $candidates = @(@(
        $env:MSYS2_ROOT,
        'C:\msys64',
        'C:\tools\msys64',
        (Join-Path $env:LOCALAPPDATA 'msys64'),
        (Join-Path ${env:ProgramFiles} 'msys64')
    ) | Where-Object { $_ -and (Test-Path (Join-Path $_ 'usr\bin\pacman.exe')) })
    if ($candidates.Count) { return $candidates[0] }
    return $null
}

$Msys2 = Find-Msys2

# Run a command inside the MSYS2 MinGW64 login shell. -lc so the profile sets
# up PATH; MSYSTEM=MINGW64 so /mingw64 is the active prefix.
# Two Windows PowerShell 5.1 traps, both handled here:
#  - bash's stdout must go to the HOST, not the pipeline. Left in the pipeline
#    it becomes part of the function's return value, so `$rc = Invoke-Msys`
#    got every pacman line plus the exit code, and `$rc -ne 0` was always true.
#  - With $ErrorActionPreference = 'Stop', redirecting a native command's
#    stderr turns every stderr line into a TERMINATING NativeCommandError.
#    Scoped to 'Continue' here so a warning on stderr cannot abort the script.
function Invoke-Msys([string]$cmdline, [switch]$Quiet) {
    if (-not $Msys2) { throw "MSYS2 not found" }
    $ErrorActionPreference = 'Continue'
    $bash = Join-Path $Msys2 'usr\bin\bash.exe'
    $env:MSYSTEM = 'MINGW64'
    $env:CHERE_INVOKING = '1'
    if ($Quiet) {
        & $bash -lc $cmdline 2>$null | Out-Null
    } else {
        & $bash -lc $cmdline | Out-Host
    }
    return $LASTEXITCODE
}

function Msys-Query([string]$cmdline) {
    if (-not $Msys2) { return $null }
    $ErrorActionPreference = 'Continue'   # see Invoke-Msys
    $bash = Join-Path $Msys2 'usr\bin\bash.exe'
    $env:MSYSTEM = 'MINGW64'
    $out = & $bash -lc $cmdline 2>$null
    if ($LASTEXITCODE -ne 0) { return $null }
    return ($out | Out-String).Trim()
}

# ── Package sets ────────────────────────────────────────────────────────────
function Pkgs-For([string]$t) {
    switch ($t) {
        'cli'    { return @('mingw-w64-x86_64-gcc','make') }
        'json'   { return @('curl') }
        'ffmpeg' { return @('mingw-w64-x86_64-pkgconf','mingw-w64-x86_64-nasm','git','diffutils') }
        'gui'    { return @('mingw-w64-x86_64-SDL2','git') }
        default  { return @() }
    }
}

# ── Capability checks ───────────────────────────────────────────────────────
function Check-Cli {
    $bad = $false
    $gxx = Msys-Query 'command -v g++ && g++ --version | head -1'
    if ($gxx) {
        OK ("C++ compiler   " + ($gxx -split "`n")[-1])
        # The threading model is the whole Windows story -- see .DESCRIPTION.
        $tm = Msys-Query "g++ -v 2>&1 | sed -n 's/^Thread model: //p'"
        if ($tm -eq 'posix') {
            OK "thread model   posix (std::thread works)"
        } else {
            Miss "thread model is '$tm' -- need 'posix'. The win32 model has NO std::thread;"
            Say  "        crete links clean and then dies on its first parallel decode."
            Say  "        Install mingw-w64-x86_64-gcc from MSYS2 rather than a win32-model MinGW."
            $bad = $true
        }
    } else { Miss "g++ (MinGW-w64, C++17)"; $bad = $true }

    if (Msys-Query 'command -v gcc') { OK "C compiler     present (C99, for third_party/libdsddpcm)" }
    else { Miss "gcc"; $bad = $true }

    if (Msys-Query 'command -v make') { OK "make           present" }
    else { Miss "make"; $bad = $true }

    return -not $bad
}

function Check-Json {
    if (Msys-Query 'command -v curl') { OK "curl           present"; return $true }
    Miss "curl (Makefile fetches nlohmann/json with it)"; return $false
}

function Check-Ffmpeg {
    $bad = $false
    if (Msys-Query 'command -v pkg-config || command -v pkgconf') { OK "pkg-config     present" }
    else { Miss "pkg-config"; $bad = $true }
    # FFmpeg's configure ERRORS on x86_64 without an assembler, because
    # build_ffmpeg_compact.sh does not pass --disable-x86asm.
    if (Msys-Query 'command -v nasm || command -v yasm') { OK "nasm/yasm      present (x86 needs it for FFmpeg asm)" }
    else { Miss "nasm (FFmpeg configure FAILS on x86_64 without it)"; $bad = $true }
    if (Msys-Query 'command -v git') { OK "git            present" } else { Miss "git"; $bad = $true }
    Note "the compact FFmpeg has never been cross-built for mingw -- see README"
    return -not $bad
}

function Check-Gui {
    $bad = $false
    if (Msys-Query 'pkg-config --exists sdl2 && pkg-config --modversion sdl2') {
        OK ("SDL2           " + (Msys-Query 'pkg-config --modversion sdl2'))
    } else { Miss "SDL2 development files"; $bad = $true }
    if (Msys-Query 'command -v git') { OK "git            present (clones Dear ImGui)" }
    else { Miss "git"; $bad = $true }
    Note "'make gui CROSS=...' is untested on Windows; the CLI tiers are proven"
    return -not $bad
}

# ── Report ──────────────────────────────────────────────────────────────────
Say "crete build dependencies   (Windows, $env:PROCESSOR_ARCHITECTURE)"
Say "tiers: $($Tier -join ' ')"

if (-not $Msys2) {
    Head2 "MSYS2"
    Miss "not found (looked in C:\msys64, C:\tools\msys64, %LOCALAPPDATA%, %ProgramFiles%)"
    Say ""
    Say "crete's build is GNU make + g++ + pkg-config, so MSYS2 is the toolchain."
    $installer = $null
    if     (Have 'winget') { $installer = 'winget install --id MSYS2.MSYS2 -e --accept-package-agreements --accept-source-agreements' }
    elseif (Have 'choco')  { $installer = 'choco install msys2 -y' }
    elseif (Have 'scoop')  { $installer = 'scoop install msys2' }

    if ($installer) {
        Say "Install it with:"
        Say "    $installer"
        if ($Check -or $DryRun) {
            Say ""
            Say "$(if ($Check) {'-Check'} else {'-DryRun'}) given; nothing executed."
            exit 1
        }
        if (-not $Yes) {
            $r = Read-Host "`nRun it now? (needs Administrator) [y/N]"
            if ($r -notmatch '^(y|yes)$') { Say "Aborted. The command above is safe to run yourself."; exit 1 }
        }
        Invoke-Expression $installer | Out-Host
        $Msys2 = Find-Msys2
        if (-not $Msys2) {
            Say ""
            Say "MSYS2 still not found. If the installer succeeded, open a NEW shell"
            Say "(PATH is only picked up by new processes) and re-run this script."
            exit 1
        }
        OK "MSYS2 at $Msys2"
    } else {
        Say "No winget/choco/scoop found. Install MSYS2 by hand from https://www.msys2.org/"
        Say "then re-run this script."
        exit 1
    }
} else {
    Head2 "MSYS2"
    OK "found at $Msys2"
}

$need = @()
foreach ($t in $Tier) {
    Head2 "tier: $t"
    $fn = "Check-" + $t.Substring(0,1).ToUpper() + $t.Substring(1)
    if (-not (& $fn)) { $need += $t }
}

Head2 "result"
if ($need.Count -eq 0) {
    OK "every requested tier is already satisfied"
} else {
    Say "  unsatisfied tiers: $($need -join ' ')"
}

if ($Check) { Say ""; Say "-Check given; nothing installed."; if ($need.Count) { exit 1 } else { exit 0 } }

# ── Install ─────────────────────────────────────────────────────────────────
if ($need.Count -gt 0) {
    $pkgs = @()
    foreach ($t in $need) { $pkgs += Pkgs-For $t }
    $pkgs = @($pkgs | Select-Object -Unique)   # @(): one package must stay a list

    if ($pkgs.Count -eq 0) {
        Say ""
        Say "Nothing this script can install covers the gap above -- see the notes."
    } else {
        $cmd = "pacman -S --needed --noconfirm " + ($pkgs -join ' ')
        Say ""
        Say "Command to install the missing packages (inside MSYS2):"
        Say "    $cmd"

        if ($DryRun) { Say ""; Say "-DryRun given; nothing executed."; exit 0 }

        if (-not $Yes) {
            $r = Read-Host "`nRun it now? [y/N]"
            if ($r -notmatch '^(y|yes)$') { Say "Aborted. The command above is safe to run yourself."; exit 1 }
        }

        # -Syu first: pacman refuses to install into a partially-updated tree,
        # and a stale MSYS2 is the single most common cause of a confusing
        # "target not found" on a package name that plainly exists.
        Invoke-Msys 'pacman -Syu --noconfirm' | Out-Null
        $rc = Invoke-Msys $cmd
        if ($rc -ne 0) { Say "package install failed"; exit 1 }

        Head2 "re-checking"
        $still = @()
        foreach ($t in $Tier) {
            $fn = "Check-" + $t.Substring(0,1).ToUpper() + $t.Substring(1)
            if (-not (& $fn 6>$null)) { $still += $t }
        }
        if ($still.Count) { Say "  still unsatisfied: $($still -join ' ')" } else { OK "all requested tiers satisfied" }
    }
}

# ── Optional fetch ──────────────────────────────────────────────────────────
if ($Fetch) {
    $unix = (Msys-Query "cygpath -u '$Root'")
    if (Want 'gui') {
        Head2 "Dear ImGui"
        if (Test-Path (Join-Path $Root 'third_party\imgui')) { OK "already present" }
        elseif ($DryRun) { Say "    make -C $unix setup-imgui" }
        else { Invoke-Msys "make -C '$unix' setup-imgui" | Out-Null }
    }
    if (Want 'json') {
        Head2 "nlohmann/json"
        if (Test-Path (Join-Path $Root 'third_party\nlohmann\json.hpp')) { OK "already present" }
        elseif ($DryRun) { Say "    make -C $unix setup-nlohmann" }
        else { Invoke-Msys "make -C '$unix' setup-nlohmann" | Out-Null }
    }
    if (Want 'ffmpeg') {
        Head2 "compact FFmpeg"
        Note "NOT cross-built for mingw before -- build_ffmpeg_compact.sh is untested"
        Note "against this target. Skipping; build the cli / cli-json tiers instead."
    }
}

Head2 "next  (run these in an MSYS2 MINGW64 shell, from the repo root)"
Say "  make STATIC=1                 zero-dependency CLI -- STATIC=1 is not optional"
Say "                                on Windows, see the note at the top of this file"
if (Want 'json')   { Say "  make cli-json STATIC=1        + JSON output" }
if (Want 'gui')    { Say "  make gui                      GUI (untested on Windows)" }
Say "  ./scripts/check_fix12.sh      verify the DR self-check passes"
Say ""
Say "  Confirm the .exe is self-contained:"
Say "      objdump -p crete.exe | grep 'DLL Name'"
Say "  Expect only KERNEL32.dll and api-ms-win-crt-*.dll. If you see"
Say "  libwinpthread-1.dll / libstdc++-6.dll / libgcc_s_seh-1.dll, STATIC=1 did not apply."
exit 0
