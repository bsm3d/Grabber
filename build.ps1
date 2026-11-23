# Builds Grabber with MinGW-w64 (MSYS2 ucrt64), statically linked: no runtime to install next to it.
#   powershell -File build.ps1            Grabber.exe, then dist\Grabber (the program, ffmpeg.exe, the licences)
#   powershell -File build.ps1 -Package   the same, then the portable zip and the installer in D:\Documents\Coding\distributions\Grabber, or in $env:BSM_DISTRIBUTIONS\Grabber (Inno Setup 6)
#   powershell -File build.ps1 -Syntax    only checks that the sources compile
# ffmpeg.exe: see below (GRABBER_FFMPEG, C:\Tools\ffmpeg, the PATH). MinGW: C:\msys64\ucrt64\bin, or the folder named by GRABBER_MINGW.
param([switch]$Syntax, [switch]$Package)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $here
$mingw = if ($env:GRABBER_MINGW) { $env:GRABBER_MINGW } else { 'C:\msys64\ucrt64\bin' }
$env:PATH = "$mingw;$env:PATH"
$src = 'util.cpp', 'core.cpp', 'cards.cpp', 'toolkit.cpp', 'ui.cpp', 'main.cpp'
$flags = '-std=c++17', '-O2', '-Wall', '-Wno-unknown-pragmas', '-Wno-unused-function', '-DUNICODE', '-D_UNICODE', '-DWINVER=0x0A00', '-D_WIN32_WINNT=0x0A00', '-DNTDDI_VERSION=0x0A000007'
if ($Syntax) {
    & g++ @flags -fsyntax-only @src
    exit $LASTEXITCODE
}
$version = ([regex]::Match((Get-Content grabber.h -Raw), '#define APP_VERSION L"([^"]+)"')).Groups[1].Value
New-Item -ItemType Directory -Force obj | Out-Null
$headers = Get-ChildItem *.h
$objs = @()
foreach ($s in $src) {
    $o = "obj\$([IO.Path]::GetFileNameWithoutExtension($s)).o"
    $stale = -not (Test-Path $o)
    if (-not $stale) {
        $t = (Get-Item $o).LastWriteTime
        $stale = (Get-Item $s).LastWriteTime -gt $t -or ($headers | Where-Object { $_.LastWriteTime -gt $t })
    }
    if ($stale) {
        Write-Host "compiling $s"
        & g++ @flags -c $s -o $o
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    }
    $objs += $o
}
& windres -DUNICODE -O coff grabber.rc obj\grabber_res.o
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& g++ -municode -mwindows -static -static-libgcc -static-libstdc++ @objs obj\grabber_res.o -o Grabber.exe -lgdiplus -lgdi32 -luser32 -lole32 -loleaut32 -luuid -ldwmapi -lshell32 -lshlwapi -lcomdlg32 -lcomctl32 -ladvapi32 -lpropsys -lwinmm
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& strip Grabber.exe

# dist\Grabber: what the installer and the zip carry
$dst = Join-Path $here 'dist\Grabber'
if (Test-Path $dst) { Remove-Item $dst -Recurse -Force }
New-Item -ItemType Directory -Force $dst | Out-Null
# ffmpeg 8.0.1 (gyan.dev, full build): it opens NVENC with the NVIDIA drivers from the 570 series, where ffmpeg 9 needs 610 or newer.
# The one named by GRABBER_FFMPEG, else C:\Tools\ffmpeg (8.0.1, the one the PATH points to), else the one on the PATH.
$ff = if ($env:GRABBER_FFMPEG) { $env:GRABBER_FFMPEG } else { 'C:\Tools\ffmpeg\bin\ffmpeg.exe' }
if (-not (Test-Path $ff)) { $ff = (Get-Command ffmpeg -ErrorAction SilentlyContinue).Source }
if (-not (Test-Path $ff)) { throw 'ffmpeg.exe not found' }
Copy-Item Grabber.exe, $ff, LICENSE.txt, FFMPEG-LICENSE.txt, README.txt $dst
Write-Host "built dist\Grabber (Grabber.exe $((Get-Item Grabber.exe).Length) bytes, version $version)"

if ($Package) {
    $distRoot = if ($env:BSM_DISTRIBUTIONS) { $env:BSM_DISTRIBUTIONS } else { 'D:\Documents\Coding\distributions' }
    $outDir = [IO.Path]::GetFullPath((Join-Path $distRoot 'Grabber'))
    New-Item -ItemType Directory -Force $outDir | Out-Null
    $zip = Join-Path $outDir "Grabber-$version-portable.zip"
    if (Test-Path $zip) { Remove-Item $zip }
    $stage = Join-Path $env:TEMP 'grabber_zip'
    if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
    New-Item -ItemType Directory -Force "$stage" | Out-Null
    Copy-Item $dst "$stage\Grabber" -Recurse
    Compress-Archive -Path "$stage\Grabber" -DestinationPath $zip -CompressionLevel Optimal
    Remove-Item $stage -Recurse -Force
    $iscc = @("$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe", 'C:\Program Files (x86)\Inno Setup 6\ISCC.exe', 'C:\Program Files\Inno Setup 6\ISCC.exe') | Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $iscc) { throw 'Inno Setup 6 not found: the program is built (dist\Grabber) but there is no installer' }
    & $iscc /Qp "/DAppVersion=$version" installer\Grabber.iss
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    Write-Host "the portable zip and the installer are in $outDir"
}


