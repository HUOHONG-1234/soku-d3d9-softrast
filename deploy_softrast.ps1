# deploy_softrast.ps1 -- ASCII-only.  Back up then deploy rebuilt artifacts.
# PowerShell 5.1 reads .ps1 as GBK, so keep every literal ASCII.
$ErrorActionPreference = "Stop"

$SRC  = "E:\OpenClaw\.openclaw\workspace\soku-d3d9-softrast\out"
$GAME = "D:\th123"
$STAMP = "20260929"

# target = destination path; src = file in $SRC
$jobs = @(
    @{ name = "cpu_d3d9.dll";  src = "cpu_d3d9.dll";  dst = "$GAME\backends\cpu\cpu_d3d9.dll" },
    @{ name = "d3dx9_33.dll";  src = "d3dx9_33.dll";  dst = "$GAME\backends\cpu\d3dx9_33.dll" },
    @{ name = "d3d9.dll(loader)"; src = "d3d9.dll";   dst = "$GAME\backends\loader\d3d9.dll" },
    @{ name = "d3d9.dll(root)";   src = "d3d9.dll";   dst = "$GAME\d3d9.dll" },
    @{ name = "soku_config.exe";  src = "soku_config.exe"; dst = "$GAME\soku_config.exe" }
)

if (Get-Process th123 -ErrorAction SilentlyContinue) {
    Write-Host "[ABORT] th123.exe is running. Close the game first." -ForegroundColor Red
    exit 1
}

Write-Host "=== BACKUP ===" -ForegroundColor Cyan
foreach ($j in $jobs) {
    $d = $j.dst
    if (Test-Path $d) {
        $bak = "$d.bak_$STAMP"
        Copy-Item $d $bak -Force
        Write-Host ("  bak  " + $bak)
    } else {
        Write-Host ("  (new) " + $d)
    }
}

Write-Host ""
Write-Host "=== DEPLOY ===" -ForegroundColor Cyan
foreach ($j in $jobs) {
    $s = Join-Path $SRC $j.src
    if (-not (Test-Path $s)) { Write-Host ("  [MISS] " + $s) -ForegroundColor Red; continue }
    Copy-Item $s $j.dst -Force
    $len = (Get-Item $j.dst).Length
    $h = (Get-FileHash $j.dst -Algorithm SHA256).Hash.Substring(0,16)
    Write-Host ("  OK   " + $j.dst + "  " + $len + " B  " + $h) -ForegroundColor Green
}

Write-Host ""
Write-Host "=== VERIFY (hash src vs dst) ===" -ForegroundColor Cyan
foreach ($j in $jobs) {
    $s = Join-Path $SRC $j.src
    if ((Test-Path $s) -and (Test-Path $j.dst)) {
        $hs = (Get-FileHash $s -Algorithm SHA256).Hash
        $hd = (Get-FileHash $j.dst -Algorithm SHA256).Hash
        $ok = if ($hs -eq $hd) { "MATCH" } else { "MISMATCH" }
        Write-Host ("  {0,-18} {1}" -f $j.name, $ok)
    }
}
