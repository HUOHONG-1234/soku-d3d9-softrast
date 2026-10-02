# package.ps1 -- build a release archive containing BOTH cpu backends.
#
# Only this project's own binaries go into the archive.  Microsoft's
# DirectX runtime, DXVK and dgVoodoo2 are loaded at run time from the
# system or must be fetched by the user; redistributing them is not
# permitted.
#
# ASCII-only: PowerShell 5.1 reads .ps1 as GBK.

param(
    [string]$Version       = "0.8.3hotfix", # manager (soku_config.exe)
    [string]$LocVersion    = "0.8.1",     # localization (backends\lang\*.ini)
    [string]$LoaderVersion = "0.1.2hotfix",# loader (d3d9.dll)
    [string]$OutDir  = ".\dist",
    [int]$DefaultSSAA = 2,      # SSAA level written into the packaged ini
    [string]$DefaultAA = "off"  # off | ssaa | fxaa  (default = safest)
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path

# The About page shows the loader version from soku_loader.ini, but the loader
# itself logs its own copy.  They ship together, so refuse to package a
# mismatch rather than publish a build that lies about itself.
$ldrSrc = Get-Content "$Root\loader\src\d3d9_proxy.cpp" -Raw
if ($ldrSrc -match '#define\s+SOKU_LOADER_VERSION\s+"([^"]+)"') {
    $inSrc = $Matches[1]
    if ($inSrc -ne $LoaderVersion) {
        Write-Host ("[FAIL] LoaderVersion mismatch: package.ps1 says '" + $LoaderVersion + "', d3d9_proxy.cpp says '" + $inSrc + "'") -ForegroundColor Red
        exit 1
    }
    Write-Host ("loader ver  = " + $LoaderVersion + "  (matches source)") -ForegroundColor Cyan
} else {
    Write-Host "[FAIL] SOKU_LOADER_VERSION not found in d3d9_proxy.cpp" -ForegroundColor Red
    exit 1
}

Write-Host "=== 1) build AVX2 (default) ===" -ForegroundColor Cyan
& "$Root\build.ps1" -OutDir ".\out_avx2" -Arch AVX2
if (-not (Test-Path "$Root\out_avx2\cpu_d3d9.dll")) { Write-Host "[FAIL] AVX2 build missing" -ForegroundColor Red; exit 1 }

Write-Host ""
Write-Host "=== 2) build SSE2 (fallback for pre-Haswell) ===" -ForegroundColor Cyan
& "$Root\build.ps1" -OutDir ".\out_sse2" -Arch SSE2
if (-not (Test-Path "$Root\out_sse2\cpu_d3d9.dll")) { Write-Host "[FAIL] SSE2 build missing" -ForegroundColor Red; exit 1 }

Write-Host ""
Write-Host "=== 3) regression test (both) ===" -ForegroundColor Cyan
& "$Root\out_avx2\test_rast.exe" | Write-Host
if ($LASTEXITCODE -ne 0) { Write-Host "[FAIL] AVX2 regression failed" -ForegroundColor Red; exit 1 }
& "$Root\out_sse2\test_rast.exe" | Write-Host
if ($LASTEXITCODE -ne 0) { Write-Host "[FAIL] SSE2 regression failed" -ForegroundColor Red; exit 1 }

Write-Host ""
Write-Host "=== 4) stage package ===" -ForegroundColor Cyan
$Pkg = Join-Path $OutDir "th123_softpipe-$Version"
if (Test-Path $Pkg) { Remove-Item $Pkg -Recurse -Force }
New-Item -ItemType Directory -Force -Path "$Pkg\backends\cpu"       | Out-Null
New-Item -ItemType Directory -Force -Path "$Pkg\backends\dxvk"      | Out-Null
New-Item -ItemType Directory -Force -Path "$Pkg\backends\dgvoodoo2" | Out-Null
New-Item -ItemType Directory -Force -Path "$Pkg\backends\wined3d"   | Out-Null
New-Item -ItemType Directory -Force -Path "$Pkg\backends\native"    | Out-Null
New-Item -ItemType Directory -Force -Path "$Pkg\backends\lang"      | Out-Null

# --- our own binaries ---
# loader + d3dx proxy live in the GAME ROOT (the game loads them by name).
Copy-Item "$Root\out_avx2\d3d9.dll"        "$Pkg\d3d9.dll"        -Force
Copy-Item "$Root\out_avx2\d3dx9_33.dll"    "$Pkg\d3dx9_33.dll"    -Force
Copy-Item "$Root\out_avx2\soku_config.exe" "$Pkg\soku_config.exe" -Force
# the two CPU rasterizers sit side by side; the loader resolves ChainDll
# against root, then backends\cpu\, then backends\.
Copy-Item "$Root\out_avx2\cpu_d3d9.dll"    "$Pkg\backends\cpu\cpu_avx2.dll" -Force
Copy-Item "$Root\out_sse2\cpu_d3d9.dll"    "$Pkg\backends\cpu\cpu_sse2.dll" -Force
Write-Host "  root    : d3d9.dll  d3dx9_33.dll  soku_config.exe"
Write-Host "  backends: cpu\cpu_avx2.dll  cpu\cpu_sse2.dll  lang\*.ini"

# --- language packs (soku_config reads backends\lang\<code>.ini) ---
if (Test-Path "$Root\lang") {
    Copy-Item "$Root\lang\*.ini" "$Pkg\backends\lang\" -Force
}

# --- config presets ---
$loaderIni = @"
[soku_loader]
Verbose=1
ForceLoad=0
DiscoverModules=1
LoadEngine=0
LoadModules=1
; Which CPU rasterizer to chain to.  Both live in backends\cpu\.
;   cpu_avx2.dll = fast, REQUIRES Haswell+ (2013+).  Default.
;   cpu_sse2.dll = any x64 CPU.  Use this on old machines (Nehalem/Core2 etc.)
; Switch with soku_config.exe, or edit this line by hand.
ChainDll=cpu_avx2.dll
UseOn12=0

[About]
; Shown on the About tab.  The three parts version independently.
AppName=soku_config
Version=$Version
LocVersion=$LocVersion
LoaderVersion=$LoaderVersion
Author=Aya
"@
[System.IO.File]::WriteAllText("$Pkg\soku_loader.ini", $loaderIni, [System.Text.Encoding]::ASCII)

$shimIni = @"
[soku_shim]
; Path to the real d3d9. Empty = auto-detect:
;   backends\dxvk -> backends\dgvoodoo2 -> backends\wined3d -> system
; The system (native) d3d9 works out of the box with this build.
RealD3D9=
; Advertised GPU: gf5200 | gma900 | weak
Gpu=gf5200

; ---------------- anti-aliasing (runtime selectable) ----------------
; AAMode: off | ssaa | fxaa
;   off  = no AA, fastest
;   ssaa = supersampling; real coverage AA, costs (SuperSample^2) raster work
;   fxaa = post-process edge blend; cheap, slightly softens 2D sprites
AAMode=$DefaultAA
; SuperSample: SSAA only. 1=off | 2=4x samples | 3=9x | 4=16x
SuperSample=$DefaultSSAA
; FXAAStrength: FXAA only, 0..100 (60 is a good default)
FXAAStrength=60
"@
[System.IO.File]::WriteAllText("$Pkg\soku_shim.ini", $shimIni, [System.Text.Encoding]::ASCII)
Write-Host "  config  : soku_loader.ini  soku_shim.ini"

# --- docs ---
Copy-Item "$Root\README.md" "$Pkg\README.md" -Force
Copy-Item "$Root\LICENSE"   "$Pkg\LICENSE"   -Force

# --- backends README ---
$bk = @"
backends\cpu\          SHIPPED.  cpu_avx2.dll (Haswell+) and cpu_sse2.dll (any x64).
                       Pick one via soku_loader.ini ChainDll= or soku_config.exe.

Place third-party backends here.  They are NOT bundled (licence reasons).

backends\dxvk\        dxvk_d3d9.dll, d3d11.dll, dxgi.dll, dxvk.conf
backends\dgvoodoo2\   dgvoodoo_d3d9.dll, dgVoodoo.conf
backends\wined3d\     wined3d_d3d9.dll
backends\native\      (empty; uses the system d3d9)

Deploy a backend with soku_config.exe ("Deploy backend"), which copies the
selected set next to th123.exe.  Leaving RealD3D9= empty falls back to the
system d3d9 when no backend is deployed.
"@
[System.IO.File]::WriteAllText("$Pkg\backends\README.txt", $bk, [System.Text.Encoding]::ASCII)

Write-Host ""
Write-Host "=== 5) archive ===" -ForegroundColor Cyan
$zip = Join-Path $OutDir "th123_softpipe-$Version.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path "$Pkg\*" -DestinationPath $zip -CompressionLevel Optimal
$kb = [math]::Round((Get-Item $zip).Length / 1KB, 1)
Write-Host ("  " + $zip + "  " + $kb + " KB") -ForegroundColor Green

Write-Host ""
Write-Host "=== package contents ===" -ForegroundColor Cyan
Get-ChildItem $Pkg -Recurse -File | ForEach-Object {
    Write-Host ("  {0,-42} {1,8} B" -f $_.FullName.Replace($Pkg + "\", ""), $_.Length)
}

Write-Host ""
Write-Host "NOTE: no Microsoft DirectX components, no DXVK, no dgVoodoo2 included." -ForegroundColor Yellow
Write-Host "      Users need the DirectX 9.0c runtime for d3dx9_33.dll." -ForegroundColor Yellow
