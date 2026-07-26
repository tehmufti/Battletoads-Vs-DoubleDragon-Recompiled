# build.ps1 — compile the recompilation with MSVC (Visual Studio 2022+).
#   .\build.ps1            → interpreter-only headless (build\recomp-headless.exe)
#   .\build.ps1 -Gen       → headless with generated native blocks (run tools\gen-c.mjs first)
#   .\build.ps1 -Win       → Battletoads-Recomp.exe (playable; implies generated code if gen\ exists)
param([switch]$Gen, [switch]$Win)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$build = Join-Path $root "build"
New-Item -ItemType Directory -Force $build | Out-Null

# ── locate MSVC: vswhere first, then well-known paths ─────────────────────────
$vcvars = $null
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (Test-Path $vswhere) {
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null
    if ($vs) { $vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat" }
}
if (-not $vcvars -or -not (Test-Path $vcvars)) {
    $candidates = @(
        "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat",
        "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat",
        "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat",
        "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
    )
    $vcvars = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
}
if (-not $vcvars) { throw "MSVC not found - install Visual Studio 2022 with the C++ toolset" }

$runtime = @("bus.c", "cpu.c", "ppu.c", "apu.c", "state.c") | ForEach-Object { Join-Path $root "runtime\$_" }
$genDir = Join-Path $root "gen"
$genFiles = @()
if (($Gen -or $Win) -and (Test-Path $genDir)) {
    $genFiles = Get-ChildItem $genDir -Filter "*.c" | ForEach-Object { $_.FullName }
}
if ($genFiles.Count -eq 0) {
    $genFiles = @(Join-Path $root "runtime\recomp_stub.c")
    Write-Host "linking interpreter stub (no generated code - run: npm run gen)"
} else {
    Write-Host "linking $($genFiles.Count) generated file(s)"
}

$common = "/nologo /O2 /W3 /std:c11 /D_CRT_SECURE_NO_WARNINGS /I`"$root\runtime`""

if ($Win) {
    $main = Join-Path $root "runtime\win32_main.c"
    $out = Join-Path $root "Battletoads-Recomp.exe"
    $srcs = (@($main) + $runtime + $genFiles | ForEach-Object { "`"$_`"" }) -join " "
    $cmd = "`"$vcvars`" >nul && cl $common /Fe`"$out`" /Fo`"$build\\`" $srcs user32.lib gdi32.lib winmm.lib"
} else {
    $main = Join-Path $root "runtime\headless.c"
    $out = Join-Path $build "recomp-headless.exe"
    $srcs = (@($main) + $runtime + $genFiles | ForEach-Object { "`"$_`"" }) -join " "
    $cmd = "`"$vcvars`" >nul && cl $common /Fe`"$out`" /Fo`"$build\\`" $srcs"
}

Write-Host "building -> $out"
cmd /c $cmd
if ($LASTEXITCODE -ne 0) { throw "compile failed ($LASTEXITCODE)" }
Write-Host "OK: $out"
