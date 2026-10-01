# build.ps1 -- build soku-d3d9-softrast from source.
#
# ASCII-ONLY ON PURPOSE.  PowerShell 5.1 reads .ps1 as GBK, so a UTF-8
# Chinese comment can decode into a stray GBK lead byte and EAT the
# following newline -- silently turning the next line into part of the
# comment.  That is exactly how the GUI build step once vanished.
# No hardcoded install paths either -- MSVC and the Windows SDK are probed.

param(
    [string]$OutDir = ".\out",
    [string]$Arch = "AVX2"      # AVX2 (fast, needs Haswell+) | SSE2 (any x64)
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
# SDK\bin\<ver>\x86 holds mt.exe, needed to embed the GUI manifest (else LNK1158).
$env:PATH    = "$VC\bin\Hostx86\x86;$SDK\bin\$SV\x86;$env:PATH"
$CL          = "$VC\bin\Hostx86\x86\cl.exe"
if (-not (Test-Path "$SDK\bin\$SV\x86\mt.exe")) {
    Write-Host "[WARN] mt.exe missing -- GUI manifest will not embed" -ForegroundColor Yellow
}

# UTF-8 source, GBK narrow strings.  Required: the comments and log strings
# are Chinese; without this they are stored as UTF-8 but read back as GBK.
$COMMON = @("/nologo", "/source-charset:utf-8", "/execution-charset:gbk",
            "/O2", "/MT", "/DWIN32", "/D_WINDOWS", "/D_CRT_SECURE_NO_WARNINGS")

$ROOT = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory -Force -Path $OutDir | Out-Null }

# ---- core selection ----
# Two independent core headers (each with its own include guard).  Copy the
# chosen one over src\softrast.h so shim/test/bench keep their #include.
$coreMap = @{ "AVX2" = "softrast_avx2.h"; "SSE2" = "softrast_sse2.h" }
if (-not $coreMap.ContainsKey($Arch)) {
    Write-Host ("[FAIL] unknown Arch '" + $Arch + "' (use AVX2 or SSE2)") -ForegroundColor Red
    exit 1
}
$coreSrc = Join-Path $ROOT ("src\" + $coreMap[$Arch])
if (-not (Test-Path $coreSrc)) { Write-Host ("[FAIL] core missing: " + $coreSrc) -ForegroundColor Red; exit 1 }
Copy-Item $coreSrc (Join-Path $ROOT "src\softrast.h") -Force
Write-Host ("core        = " + $coreMap[$Arch]) -ForegroundColor Cyan

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
# AVX2 core needs /arch:AVX2 (also enables FMA on MSVC); SSE2 core uses /arch:SSE2.
$ShimArch = @("/arch:$Arch", "/fp:fast")
Write-Host ("shim /arch  = " + $Arch) -ForegroundColor Cyan
Build-One "cpu_d3d9.dll (shim)" "$ROOT\src\shim.cpp" (@("/LD", "/EHa") + $ShimArch) @("/DEF:$ROOT\src\d3d9.def", "user32.lib") "cpu_d3d9.dll"

# 2) the d3dx9 proxy (bypasses D3DX internal validation)
Build-One "d3dx9_33.dll (proxy)" "$ROOT\src\d3dx9_proxy.cpp" @("/LD", "/EHa") @("/DEF:$ROOT\src\d3dx9_small.def", "user32.lib") "d3dx9_33.dll"

# 3) loader proxy
Build-One "d3d9.dll (loader)" "$ROOT\loader\src\d3d9_proxy.cpp" @("/LD", "/EHa") @("/DEF:$ROOT\loader\src\d3d9.def", "user32.lib") "d3d9.dll"

# 4) config GUI
# /MANIFEST:EMBED + /MANIFESTINPUT writes the comctl32 v6 dependency into the
# exe.  Without it the whole UI is Win95 grey bricks.
Build-One "soku_config.exe" "$ROOT\loader\tools\soku_config.cpp" @() @("/SUBSYSTEM:WINDOWS", "comctl32.lib", "user32.lib", "gdi32.lib", "/MANIFEST:EMBED", "/MANIFESTINPUT:$ROOT\loader\tools\soku_config.manifest") "soku_config.exe"

# 5) tests / benchmarks
Build-One "test_rast.exe"  "$ROOT\test\test_rast.cpp"   @("/I", "$ROOT\src") @("user32.lib") "test_rast.exe"
Build-One "test_blend.exe" "$ROOT\test\test_blend.cpp"  @("/I", "$ROOT\src") @("user32.lib") "test_blend.exe"
Build-One "bench_rast.exe" "$ROOT\bench\bench_rast.c"   @("/I", "$ROOT\src") @("user32.lib") "bench_rast.exe"

Write-Host ""
Write-Host "=== outputs in $OutDir ===" -ForegroundColor Cyan
Get-ChildItem $OutDir -File | ForEach-Object { Write-Host ("  " + $_.Name) }

Write-Host ""
Write-Host "Run the regression tests with:  .\out\test_rast.exe  .\out\test_blend.exe" -ForegroundColor Yellow
Write-Host "Deploy: copy out\cpu_d3d9.dll + out\d3dx9_33.dll next to th123.exe,"
Write-Host "        set ChainDll=cpu_d3d9.dll in soku_loader.ini"
