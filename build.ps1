# build.ps1 -- build soku-d3d9-softrast from source.
#
# ASCII-only on purpose: PowerShell 5.1 reads .ps1 as GBK, so non-ASCII
# would be mangled.  No hardcoded install paths either -- both MSVC and the
# Windows SDK are auto-detected.

param(
    [string]$OutDir = ".\out"
)

$ErrorActionPreference = "Stop"

function Find-Msvc {
    foreach ($root in @("C:\", "D:\", "E:\")) {
        $hits = Get-ChildItem $root -Directory -ErrorAction SilentlyContinue |
                Where-Object { $_.Name -match "VC|Visual|vs|code" }
        foreach ($h in $hits) {
            $cand = Get-ChildItem "$($h.FullName)\VC\Tools\MSVC" -Directory -ErrorAction SilentlyContinue
            foreach ($c in $cand) {
                if (Test-Path "$($c.FullName)\bin\Hostx86\x86\cl.exe") { return $c.FullName }
            }
        }
    }
    $vs = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vs) {
        $p = & $vs -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null
        if ($p) {
            $c = Get-ChildItem "$p\VC\Tools\MSVC" -Directory -ErrorAction SilentlyContinue | Select-Object -Last 1
            if ($c -and (Test-Path "$($c.FullName)\bin\Hostx86\x86\cl.exe")) { return $c.FullName }
        }
    }
    return $null
}

$VC = Find-Msvc
if (-not $VC) { Write-Host "[FAIL] MSVC cl.exe not found" -ForegroundColor Red; exit 1 }
Write-Host ("MSVC: " + $VC) -ForegroundColor Cyan

$SDK = "$env:ProgramFiles(x86)\Windows Kits\10"
if (-not (Test-Path $SDK)) {
    foreach ($root in @("D:\", "C:\", "E:\")) {
        $kit = Get-ChildItem $root -Directory -ErrorAction SilentlyContinue |
               Where-Object { $_.Name -eq "Windows Kits" }
        if ($kit -and (Test-Path "$($kit.FullName)\10\Include")) { $SDK = "$($kit.FullName)\10"; break }
    }
}
if (-not (Test-Path $SDK)) { Write-Host "[FAIL] Windows SDK not found" -ForegroundColor Red; exit 1 }

$SV = (Get-ChildItem "$SDK\Include" -Directory -ErrorAction SilentlyContinue |
       Where-Object { $_.Name -match "^10\." } | Sort-Object Name | Select-Object -Last 1).Name
if (-not $SV) { Write-Host "[FAIL] no SDK version found" -ForegroundColor Red; exit 1 }
Write-Host ("SDK : " + $SDK + "  ver " + $SV) -ForegroundColor Cyan

$env:INCLUDE = "$VC\include;$SDK\Include\$SV\um;$SDK\Include\$SV\shared;$SDK\Include\$SV\ucrt"
$env:LIB     = "$VC\lib\x86;$SDK\Lib\$SV\ucrt\x86;$SDK\Lib\$SV\um\x86"
$env:PATH    = "$VC\bin\Hostx86\x86;$env:PATH"
$CL          = "$VC\bin\Hostx86\x86\cl.exe"

# UTF-8 source, GBK narrow strings.  Required: the comments and log strings
# are Chinese; without this they are stored as UTF-8 but read back as GBK.
$COMMON = @("/nologo", "/source-charset:utf-8", "/execution-charset:gbk",
            "/O2", "/MT", "/DWIN32", "/D_WINDOWS", "/D_CRT_SECURE_NO_WARNINGS")

$ROOT = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory -Force -Path $OutDir | Out-Null }

function Build-One($name, $src, $extra, $link, $out) {
    Write-Host ""
    Write-Host ("=== " + $name + " ===") -ForegroundColor Cyan
    $outPath = Join-Path $OutDir $out
    if (Test-Path $outPath) { Remove-Item $outPath -Force }
    $a = $COMMON + $extra + @($src) + @("/link") + $link + @("/OUT:$outPath", "/MACHINE:X86")
    $log = & $CL @a 2>&1
    if (Test-Path $outPath) {
        Write-Host ("  [OK] " + $out + "  " + [math]::Round((Get-Item $outPath).Length/1KB,1) + " KB") -ForegroundColor Green
    } else {
        Write-Host ("  [FAIL] " + $name) -ForegroundColor Red
        $log | Select-String -Pattern "error" | Select-Object -First 10 | ForEach-Object { Write-Host ("    " + $_.Line) }
    }
}

# 1) the shim itself
Build-One "cpu_d3d9.dll (shim)" "$ROOT\src\shim.cpp" @("/LD", "/EHa", "/arch:SSE2") `
          @("/DEF:$ROOT\src\d3d9.def", "user32.lib") "cpu_d3d9.dll"

# 2) the d3dx9 proxy (bypasses D3DX internal validation)
Build-One "d3dx9_33.dll (proxy)" "$ROOT\src\d3dx9_proxy.cpp" @("/LD", "/EHa") `
          @("/DEF:$ROOT\src\d3dx9_small.def", "user32.lib") "d3dx9_33.dll"

# 3) loader proxy
Build-One "d3d9.dll (loader)" "$ROOT\loader\src\d3d9_proxy.cpp" @("/LD", "/EHa") `
          @("/DEF:$ROOT\loader\src\d3d9.def", "user32.lib") "d3d9.dll"

# 4) config GUI
Build-One "soku_config.exe" "$ROOT\loader\tools\soku_config.cpp" @() `
          @("/SUBSYSTEM:WINDOWS", "comctl32.lib", "user32.lib", "gdi32.lib") "soku_config.exe"

# 5) tests / benchmarks
Build-One "test_rast.exe" "$ROOT\test\test_rast.cpp" @("/I", "$ROOT\src") @("user32.lib") "test_rast.exe"
Build-One "bench_rast.exe" "$ROOT\bench\bench_rast.c" @("/I", "$ROOT\src") @("user32.lib") "bench_rast.exe"

Write-Host ""
Write-Host "=== outputs in $OutDir ===" -ForegroundColor Cyan
Get-ChildItem $OutDir -File | ForEach-Object { Write-Host ("  " + $_.Name) }

Write-Host ""
Write-Host "Run the regression test with:  .\out\test_rast.exe" -ForegroundColor Yellow
Write-Host "Deploy: copy out\cpu_d3d9.dll + out\d3dx9_33.dll next to th123.exe,"
Write-Host "        set ChainDll=cpu_d3d9.dll in soku_loader.ini"
