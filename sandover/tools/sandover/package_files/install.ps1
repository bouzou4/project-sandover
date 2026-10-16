<#
.SYNOPSIS
Installs Sandover from this folder: extracts the user's own Jak and Daxter disc image with the
OpenGOAL extractor, checks the remaster's prebuilt lighting sidecars and writes a shortcut.

.PARAMETER IsoPath
The disc image (.iso). Asked for with a file dialog when omitted.

.PARAMETER ShortcutDir
Where the shortcut goes; the desktop by default.

.PARAMETER ConfigDir
Settings and saves folder; %APPDATA%\Sandover by default (kept apart from a stock OpenGOAL).
#>
param(
    [string]$IsoPath,
    [string]$ShortcutDir = [Environment]::GetFolderPath("Desktop"),
    [string]$ConfigDir = (Join-Path $env:APPDATA "Sandover")
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$data = Join-Path $root "data"
$log = Join-Path $root "install.log"

function Say($msg) {
    Write-Host $msg
    Add-Content -LiteralPath $log -Value $msg
}

function Fail($msg) {
    Say "ERROR: $msg"
    Say "Nothing was deleted. Fix the problem and run Install.cmd again. Log: $log"
    exit 1
}

Set-Content -LiteralPath $log -Value "Sandover install $(Get-Date -Format s)"
if (-not (Test-Path -LiteralPath (Join-Path $data "out\jak1\iso\GAME.CGO"))) {
    Get-ChildItem -LiteralPath $root -Recurse -File | Unblock-File
}

foreach ($f in @("gk.exe", "goalc.exe", "extractor.exe")) {
    if (-not (Test-Path -LiteralPath (Join-Path $root $f))) { Fail "$f is missing; extract the whole zip first." }
}
if (-not (Test-Path -LiteralPath (Join-Path $data "goal_src\jak1\game.gp"))) { Fail "data\goal_src is missing; extract the whole zip first." }

if (-not $IsoPath) {
    Add-Type -AssemblyName System.Windows.Forms
    $dlg = New-Object System.Windows.Forms.OpenFileDialog
    $dlg.Title = "Select your Jak and Daxter: The Precursor Legacy disc image"
    $dlg.Filter = "Disc image (*.iso)|*.iso|All files (*.*)|*.*"
    if ($dlg.ShowDialog() -ne [System.Windows.Forms.DialogResult]::OK) { Fail "no disc image selected." }
    $IsoPath = $dlg.FileName
}
if (-not (Test-Path -LiteralPath $IsoPath -PathType Leaf)) { Fail "disc image not found: $IsoPath" }

$drive = (Get-Item -LiteralPath $root).PSDrive
if ($drive.Free -and $drive.Free -lt 5GB) { Fail "the install needs about 5 GB free on drive $($drive.Name):." }

function Extractor-Message($code) {
    $meta = Join-Path $data "launcher\error-code-metadata.json"
    if (Test-Path -LiteralPath $meta) {
        $m = (Get-Content -LiteralPath $meta -Raw | ConvertFrom-Json)."$code"
        if ($m) { return $m.msg }
    }
    return "unknown error"
}

Say "Extracting, checking, decompiling and compiling the game from your disc (a few minutes)..."
$ext = Join-Path $root "extractor.exe"
$extLog = Join-Path $root "extractor.log"
try {
    $p = Start-Process -FilePath $ext -WorkingDirectory $root -NoNewWindow -PassThru `
        -ArgumentList @("`"$IsoPath`"", "--proj-path", "`"$data`"", "--validate", "--extract", "--decompile", "--compile") `
        -RedirectStandardOutput $extLog -RedirectStandardError "$extLog.err"
} catch {
    Fail "the extractor could not be started ($($_.Exception.Message)); see $extLog."
}
try { $null = $p.Handle } catch { }  # without it ExitCode reads empty after a polled wait
while (-not $p.WaitForExit(15000)) {
    $last = Get-Content -LiteralPath $extLog -Tail 1 -ErrorAction SilentlyContinue
    if ($last) {
        $t = ($last -replace '\x1b\[[0-9;]*m', '' -replace '[^\x20-\x7e]', '').Trim()
        if ($t) { Write-Host ("  " + $t.Substring(0, [Math]::Min(100, $t.Length))) }
    }
}
$p.WaitForExit()
Get-Content -LiteralPath $extLog -Tail 5 | ForEach-Object { Add-Content -LiteralPath $log -Value $_ }
$code = $p.ExitCode
if ($null -eq $code) { Fail "the extractor ended without an exit code; see $extLog." }
if ($code -ge 4000 -and $code -lt 4030) {
    Fail "this disc image was not recognised as Jak and Daxter: The Precursor Legacy ($code, $(Extractor-Message $code)). Use a full image of your own disc."
}
if ($code -ge 4040 -and $code -lt 4050) {
    Fail "the disc image could not be read ($code, $(Extractor-Message $code)). Make a fresh image of the disc."
}
if ($code -ne 0) { Fail "the extractor stopped with code $code ($(Extractor-Message $code)); see $extLog." }

$fr3 = Join-Path $data "out\jak1\fr3"
$iso = Join-Path $data "out\jak1\iso"
if (-not (Test-Path -LiteralPath (Join-Path $iso "GAME.CGO"))) { Fail "the extractor finished but out\jak1\iso\GAME.CGO is missing; see $extLog." }
$levels = @(Get-ChildItem -LiteralPath $fr3 -Filter *.fr3).Count
Say "Extracted: $levels level files."
if (-not (Select-String -LiteralPath $extLog -Pattern "Config Version - ntsc_v1" -Quiet)) {
    Say "WARNING: not the NTSC-U 1.0 disc the lighting was baked from; levels whose geometry differs keep the stock lighting."
}

# gk reads the sidecars from the package folder; old copies next to the fr3 would win over them
Get-ChildItem -LiteralPath $fr3 -Filter *.unbake | Remove-Item -Force
$side = Join-Path $data "sandover\data\lighting"
$n = 0
if (Test-Path -LiteralPath $side) { $n = @(Get-ChildItem -LiteralPath $side -Filter *.unbake).Count }
Say "Lighting sidecars: $n."

New-Item -ItemType Directory -Force -Path $ConfigDir | Out-Null
$gk = Join-Path $root "gk.exe"
$gkArgs = "-v --game jak1 --proj-path `"$data`" --config-path `"$ConfigDir`" -- -boot -fakeiso"
$defaultConfig = Join-Path $env:APPDATA "Sandover"
$cmdConfig = if ($ConfigDir -eq $defaultConfig) { "%APPDATA%\Sandover" } else { $ConfigDir }
$cmd = @(
    "@echo off",
    "chcp 65001 >nul",
    "set SANDOVER_DATA=",
    "set OPENGOAL_PROJ_PATH=",
    "start `"`" /D `"%~dp0.`" `"%~dp0gk.exe`" -v --game jak1 --proj-path `"%~dp0data`" --config-path `"$cmdConfig`" -- -boot -fakeiso"
)
[IO.File]::WriteAllLines((Join-Path $root "Play Sandover.cmd"), $cmd, (New-Object Text.UTF8Encoding($false)))
$sh = New-Object -ComObject WScript.Shell
$lnk = $sh.CreateShortcut((Join-Path $ShortcutDir "Sandover.lnk"))
$lnk.TargetPath = $gk
$lnk.Arguments = $gkArgs
$lnk.WorkingDirectory = $root
$lnk.IconLocation = "$gk,0"
$lnk.Save()
Say "Shortcut written: $(Join-Path $ShortcutDir 'Sandover.lnk')"
Say "Settings and saves: $ConfigDir"
Say "Done. Start the game from the shortcut or 'Play Sandover.cmd'."
