# Grabber
# © Benoit Saint-Moulin - 2026
# Version du fichier : 1.2.0
# Fonction du fichier : End to end test script that runs Grabber.exe against test windows and checks pixels and recordings.

# End to end checks of Grabber.exe, with test windows of our own (nothing of the real desktop is kept: only pixels of the test windows are measured).
#   powershell -File tests\e2e.ps1 [-Exe path\to\Grabber.exe]
# Needs Python with PySide6 (GRABBER_TEST_PYTHON, or python on the PATH) for the test windows, and ffmpeg/ffprobe on the PATH.
param([string]$Exe = (Join-Path (Split-Path -Parent $PSScriptRoot) 'Grabber.exe'))
$ErrorActionPreference = 'Stop'
$py = if ($env:GRABBER_TEST_PYTHON) { $env:GRABBER_TEST_PYTHON } else { 'python' }
$work = Join-Path $env:TEMP 'grabber_e2e'
if (Test-Path $work) { Remove-Item $work -Recurse -Force }
New-Item -ItemType Directory -Force $work | Out-Null
$env:GRABBER_HOME = Join-Path $work 'home'
$fail = 0
function Check($name, $ok, $info = '') { if ($ok) { Write-Host "ok    $name $info" } else { Write-Host "FAIL  $name $info"; $script:fail++ } }
function Pixel($file, $ss, $crop) {
    $tmp = Join-Path $work 'px.bin'
    & cmd /c "ffmpeg -v error -y -ss $ss -i `"$file`" -frames:v 1 -vf $crop -f rawvideo -pix_fmt rgb24 `"$tmp`""
    $b = [IO.File]::ReadAllBytes($tmp)
    return @([int]$b[0], [int]$b[1], [int]$b[2])
}
function Near($rgb, $r, $g, $b) { return ([Math]::Abs($rgb[0] - $r) -lt 40 -and [Math]::Abs($rgb[1] - $g) -lt 40 -and [Math]::Abs($rgb[2] - $b) -lt 40) }
function Run-Exe($args_, $seconds) {
    $p = Start-Process $Exe -ArgumentList $args_ -PassThru
    if (-not $p.WaitForExit($seconds * 1000)) { Stop-Process $p -Force; return -1 }
    return $p.ExitCode
}

# 1. the window draws (a picture of it, then it quits)
$shot = Join-Path $work 'window.png'
$rc = Run-Exe @('--shot', $shot) 30
Check 'window picture' ($rc -eq 0 -and (Test-Path $shot)) "exit $rc"

# 1b. real mouse messages on the window: a radio button and two boxes change state
$res = Join-Path $work 'click.txt'
$rc = Run-Exe @('--shot', "click:$res") 30
$txt = if (Test-Path $res) { (Get-Content $res -Raw).Trim() } else { '' }
Check 'mouse clicks' ($txt -eq 'ok') $txt

# 2. a zone over a red test window
$red = Start-Process $py -ArgumentList (Join-Path $PSScriptRoot 'topwin.py'), 'GT_RED', '#d01010', '300', '200', '640', '360' -PassThru
Start-Sleep 4
$out = Join-Path $work 'zone'
$rc = Run-Exe @('--rec-test', $out, '3', '300', '200', '640', '360') 90
Stop-Process $red -Force
$mp4 = Get-ChildItem $out -Filter *.mp4 -ErrorAction SilentlyContinue | Select-Object -First 1
Check 'zone recording' ($rc -eq 0 -and $mp4) "exit $rc"
if ($mp4) {
    $wh = & ffprobe -v error -select_streams v:0 -show_entries stream=width,height -of csv=p=0 $mp4.FullName
    Check 'zone size 640x360' ($wh -eq '640,360') $wh
    Check 'zone colour red' (Near (Pixel $mp4.FullName 1.5 'crop=2:2:iw/2:ih/2') 208 16 16)
}

# 3. several applications: red, then black (an application that is not ticked in front), blue, red; the camera in the corner; an intro first
$procs = @()
foreach ($t in @(@('GT_A', '#d01010', 100, 100, 600, 400), @('GT_B', '#1030d0', 300, 200, 640, 420), @('GT_C', '#10a010', 1000, 300, 500, 300))) {
    $procs += Start-Process $py -ArgumentList (Join-Path $PSScriptRoot 'apps_win.py'), $t[0], $t[1], $t[2], $t[3], $t[4], $t[5] -PassThru
}
Start-Sleep 6
$out = Join-Path $work 'apps'
$rc = Run-Exe @('--apps-test', $out, 'GT_A', 'GT_B', 'GT_C') 120
$procs | ForEach-Object { Stop-Process $_ -Force }
$mp4 = Get-ChildItem $out -Filter *.mp4 -ErrorAction SilentlyContinue | Select-Object -First 1
Check 'applications recording' ($rc -eq 0 -and $mp4) "exit $rc"
if ($mp4) {
    $f = $mp4.FullName
    $c = 'crop=2:2:iw/2:ih/2'
    Check 'intro is black' (Near (Pixel $f 0.5 $c) 0 0 0)
    Check 'A in front is red' (Near (Pixel $f 4.0 $c) 208 16 16)
    Check 'C in front is black' (Near (Pixel $f 6.0 $c) 0 0 0)
    Check 'B in front is blue' (Near (Pixel $f 7.8 $c) 16 48 208)
    Check 'webcam in the corner' ((Pixel $f 6.0 'crop=2:2:iw-60:30')[1] -gt 150)
}


# 3b. the sound and the picture are together: a window turns white and a tone starts at the same moment
$tone = Join-Path $work 'tone.wav'
& ffmpeg -v error -y -f lavfi -i 'sine=frequency=880:duration=0.6' -af volume=0.35 $tone
$out = Join-Path $work 'sync'
$rc = Run-Exe @('--sync-test', $out, $tone) 90
$mp4 = Get-ChildItem $out -Filter *.mp4 -ErrorAction SilentlyContinue | Select-Object -First 1
Check 'sync recording' ($rc -eq 0 -and $mp4) "exit $rc"
if ($mp4) {
    $f = $mp4.FullName
    $tw = $null
    for ($t = 1.0; $t -lt 5.0 -and -not $tw; $t += 0.1) { if ((Pixel $f $t 'crop=2:2:iw/2:ih/2')[0] -gt 200) { $tw = $t } }
    $log = (& cmd /c "ffmpeg -hide_banner -i ""$f"" -af silencedetect=noise=-45dB:d=0.15 -f null NUL 2>&1") | Out-String
    $m = [regex]::Match($log, 'silence_end: ([0-9.]+)')
    if ($tw -and $m.Success) {
        $ta = [double]::Parse($m.Groups[1].Value, [Globalization.CultureInfo]::InvariantCulture)
        Check 'sound and picture within 0.3 s' ([Math]::Abs($ta - $tw) -lt 0.3) ("picture {0:N2} s, sound {1:N2} s" -f $tw, $ta)
    } else {
        Check 'sound and picture within 0.3 s' $false "picture $tw, sound $($m.Success) (is something muting the PC?)"
    }
}
# 4. the zone picker answers a drag of the real mouse (it moves the pointer for a moment)
$res = Join-Path $work 'picker.txt'
$rc = Run-Exe @('--picker-test', $res) 30
$txt = if (Test-Path $res) { (Get-Content $res -Raw).Trim() } else { '' }
Check 'zone picker' ($txt -eq 'zone 500 400 300 250') $txt

Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
if ($fail) { Write-Host "$fail check(s) failed"; exit 1 }
Write-Host 'all checks passed'





