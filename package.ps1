# package.ps1 -- build a release archive.
#
# Only this project's own binaries go into the archive.  Microsoft's
# DirectX runtime, DXVK and dgVoodoo2 are loaded at run time from the
# system or must be fetched by the user; redistributing them is not
# permitted.
#
# ASCII-only: PowerShell 5.1 reads .ps1 as GBK.

param(
    [string]$Version = "0.1.0",
    [string]$OutDir  = ".\dist"
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path

Write-Host "=== 1) build ===" -ForegroundColor Cyan
& "$Root\build.ps1"
if (-not (Test-Path "$Root\out\cpu_d3d9.dll")) { Write-Host "[FAIL] build did not produce output" -ForegroundColor Red; exit 1 }

Write-Host ""
Write-Host "=== 2) regression test ===" -ForegroundColor Cyan
& "$Root\out\test_rast.exe" | Write-Host
if ($LASTEXITCODE -ne 0) { Write-Host "[FAIL] regression test failed" -ForegroundColor Red; exit 1 }

Write-Host ""
Write-Host "=== 3) stage package ===" -ForegroundColor Cyan
$Pkg = Join-Path $OutDir "th123_softpipe-$Version"
if (Test-Path $Pkg) { Remove-Item $Pkg -Recurse -Force }
New-Item -ItemType Directory -Force -Path "$Pkg\backends\dxvk" | Out-Null
New-Item -ItemType Directory -Force -Path "$Pkg\backends\dgvoodoo2" | Out-Null
New-Item -ItemType Directory -Force -Path "$Pkg\backends\wined3d" | Out-Null
New-Item -ItemType Directory -Force -Path "$Pkg\backends\native" | Out-Null

# --- our own binaries only ---
Copy-Item "$Root\out\d3d9.dll"        "$Pkg\d3d9.dll"        -Force
Copy-Item "$Root\out\cpu_d3d9.dll"    "$Pkg\cpu_d3d9.dll"    -Force
Copy-Item "$Root\out\d3dx9_33.dll"    "$Pkg\d3dx9_33.dll"    -Force
Copy-Item "$Root\out\soku_config.exe" "$Pkg\soku_config.exe" -Force
Write-Host "  binaries: d3d9.dll cpu_d3d9.dll d3dx9_33.dll soku_config.exe"

# --- config presets ---
$loaderIni = @"
[soku_loader]
Verbose=1
ForceLoad=0
DiscoverModules=1
LoadEngine=0
LoadModules=1
ChainDll=cpu_d3d9.dll
UseOn12=0
"@
[System.IO.File]::WriteAllText("$Pkg\soku_loader.ini", $loaderIni, [System.Text.Encoding]::ASCII)

$shimIni = @"
[soku_shim]
; Path to the real d3d9. Empty = auto-detect:
;   backends\dxvk -> backends\dgvoodoo2 -> backends\wined3d -> system
RealD3D9=
; Advertised GPU: gf5200 | gma900 | weak
Gpu=gf5200
"@
[System.IO.File]::WriteAllText("$Pkg\soku_shim.ini", $shimIni, [System.Text.Encoding]::ASCII)
Write-Host "  config: soku_loader.ini soku_shim.ini"

# --- docs ---
Copy-Item "$Root\README.md" "$Pkg\README.md" -Force
Copy-Item "$Root\LICENSE"   "$Pkg\LICENSE"   -Force

# --- backends README (so users know what to drop in) ---
$bk = @"
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
Write-Host "=== 4) archive ===" -ForegroundColor Cyan
$zip = Join-Path $OutDir "th123_softpipe-$Version.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path "$Pkg\*" -DestinationPath $zip -CompressionLevel Optimal
$kb = [math]::Round((Get-Item $zip).Length / 1KB, 1)
Write-Host ("  " + $zip + "  " + $kb + " KB") -ForegroundColor Green

Write-Host ""
Write-Host "=== package contents ===" -ForegroundColor Cyan
Get-ChildItem $Pkg -Recurse -File | ForEach-Object {
    Write-Host ("  {0,-40} {1,8} B" -f $_.FullName.Replace($Pkg + "\", ""), $_.Length)
}

Write-Host ""
Write-Host "NOTE: no Microsoft DirectX components, no DXVK, no dgVoodoo2 included." -ForegroundColor Yellow
Write-Host "      Users need the DirectX 9.0c runtime for d3dx9_33.dll." -ForegroundColor Yellow
