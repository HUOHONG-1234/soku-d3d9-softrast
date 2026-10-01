$ErrorActionPreference = "Stop"

$clExe = Get-ChildItem "C:\*\VC\Tools\MSVC\*\bin\Hostx86\x86\cl.exe","D:\*\VC\Tools\MSVC\*\bin\Hostx86\x86\cl.exe","E:\*\VC\Tools\MSVC\*\bin\Hostx86\x86\cl.exe" -ErrorAction SilentlyContinue |
         Sort-Object FullName | Select-Object -Last 1
if (-not $clExe) { Write-Host "[FAIL] cl.exe not found"; exit 1 }
$VC = Split-Path (Split-Path (Split-Path (Split-Path $clExe.FullName)))
Write-Host "MSVC: $VC"

# SDK: test well-known roots directly (same approach as build.ps1)
$SDK = $null
$cands = @("${env:ProgramFiles(x86)}\Windows Kits\10")
foreach ($root in @("D:\", "C:\", "E:\")) {
    $kit = Get-ChildItem $root -Directory -ErrorAction SilentlyContinue | Where-Object { $_.Name -eq "Windows Kits" }
    foreach ($k in $kit) { $cands += "$($k.FullName)\10" }
}
foreach ($c in $cands) { if (Test-Path "$c\Include") { $SDK = $c; break } }
if (-not $SDK) { Write-Host "[FAIL] SDK not found"; exit 1 }
$SV = (Get-ChildItem "$SDK\Include" -Directory | Where-Object { $_.Name -match "^10\." } | Sort-Object Name | Select-Object -Last 1).Name
Write-Host "SDK : $SDK  ver $SV"

$env:INCLUDE = "$VC\include;$SDK\Include\$SV\um;$SDK\Include\$SV\shared;$SDK\Include\$SV\ucrt"
$env:LIB     = "$VC\lib\x86;$SDK\Lib\$SV\ucrt\x86;$SDK\Lib\$SV\um\x86"
$env:PATH    = "$VC\bin\Hostx86\x86;$env:PATH"

$R = $PSScriptRoot
& $clExe.FullName /nologo /O2 /MT /arch:AVX2 /fp:fast /D_CRT_SECURE_NO_WARNINGS /source-charset:utf-8 /execution-charset:gbk "$R\bench\bench_blend.c" /link /OUT:"$R\out\bench_blend.exe" 2>&1 | Select-Object -Last 10
if (Test-Path "$R\out\bench_blend.exe") { Write-Host "[OK] bench_blend.exe" } else { Write-Host "[FAIL]" }
