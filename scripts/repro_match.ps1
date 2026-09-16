$ErrorActionPreference = "Continue"
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public class RZ {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr SetActiveWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr a, int x, int y, int cx, int cy, uint f);
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
  public delegate bool EnumProc(IntPtr h, IntPtr l);
}
"@

$KEYUP = 0x0002
function Press($vk) {
  [RZ]::keybd_event($vk, 0, 0, [UIntPtr]::Zero)
  Start-Sleep -Milliseconds 60
  [RZ]::keybd_event($vk, 0, $KEYUP, [UIntPtr]::Zero)
  Start-Sleep -Milliseconds 220
}

$d = "D:\th123"
$WS = "E:\OpenClaw\.openclaw\workspace"

Write-Host "=== A) kill any running game ==="
Get-Process -Name "th123*" -ErrorAction SilentlyContinue | ForEach-Object { Stop-Process -Id $_.Id -Force -ErrorAction SilentlyContinue }
Start-Sleep -Seconds 2

Write-Host "=== B) simulate manager: deploy CPU backend ==="
foreach ($f in @("cpu_d3d9.dll","d3dx9_33.dll","d3dx9_real.dll","glide3x.dll")) {
  $src = "$d\backends\cpu\$f"
  if (Test-Path $src) { Copy-Item $src "$d\$f" -Force; Write-Host ("   " + $f + " -> root") }
  else { Write-Host ("   [MISS] backends\cpu\$f") }
}

Write-Host "=== C) write ini (ChainDll=cpu_d3d9.dll) ==="
$ini = "[soku_loader]`r`nVerbose=1`r`nForceLoad=0`r`nDiscoverModules=1`r`nLoadEngine=0`r`nLoadModules=0`r`nChainDll=cpu_d3d9.dll`r`nUseOn12=0`r`n"
[System.IO.File]::WriteAllText("$d\soku_loader.ini", $ini, [System.Text.Encoding]::ASCII)

Remove-Item "$d\shim_glide.log","$d\shim_calls.log","$d\shim_stats.log","$d\d3dx_log.txt","$d\crash_modules.txt","$d\soku_loader.log" -Force -ErrorAction SilentlyContinue

Write-Host "=== D) launch ==="
$p = Start-Process -FilePath "$d\th123.exe" -WorkingDirectory $d -PassThru
Start-Sleep -Seconds 22
if ($p.HasExited) { Write-Host ("!!! CRASH at menu stage, code=" + $p.ExitCode); }
else {
  Write-Host "menu reached, entering Vs COM match..."

  $tp = $p.Id
  $script:win = [IntPtr]::Zero
  $cb = [RZ+EnumProc]{
    param($h,$l)
    $wp = 0
    [RZ]::GetWindowThreadProcessId($h, [ref]$wp) | Out-Null
    if ($wp -eq $tp -and [RZ]::IsWindowVisible($h)) {
      $sb = New-Object System.Text.StringBuilder 256
      [RZ]::GetClassName($h, $sb, 256) | Out-Null
      if ($sb.ToString() -eq "th123_110a") { $script:win = $h }
    }
    return $true
  }
  [RZ]::EnumWindows($cb, [IntPtr]::Zero) | Out-Null

  if ($script:win -ne [IntPtr]::Zero) {
    [RZ]::ShowWindow($script:win, 5) | Out-Null
    [RZ]::SetForegroundWindow($script:win) | Out-Null
    [RZ]::SetActiveWindow($script:win) | Out-Null
    Start-Sleep -Milliseconds 1200

    # menu: Down x2 -> Vs COM, Enter
    Press 0x28; Press 0x28      # Down Down
    Press 0x0D                  # Enter
    Start-Sleep -Seconds 3
    Press 0x0D                  # character select confirm
    Start-Sleep -Seconds 3
    Press 0x0D                  # stage / next
    Start-Sleep -Seconds 4
    Press 0x0D
    Start-Sleep -Seconds 6      # let the match render
  } else { Write-Host "[FAIL] game window not found" }

  if ($p.HasExited) { Write-Host ("!!! CRASH after entering match, code=" + $p.ExitCode) }
  else { Write-Host "still alive after match entry" }
}

Write-Host "=== E) collect ==="
function RS($f) { try { $fs=New-Object System.IO.FileStream($f,[System.IO.FileMode]::Open,[System.IO.FileAccess]::Read,[System.IO.FileShare]::ReadWrite); $sr=New-Object System.IO.StreamReader($fs,[System.Text.Encoding]::GetEncoding(936)); $t=$sr.ReadToEnd(); $sr.Close(); $fs.Close(); return $t } catch { return "[none]" } }

$c = RS "$d\crash_modules.txt"
if ($c -ne "[none]") {
  Write-Host "--- crash_modules.txt ---"
  ($c -split "`n") | Where-Object { $_.Trim() -ne "" } | Select-Object -First 12 | ForEach-Object { Write-Host ("  " + $_.Trim()) }
} else { Write-Host "--- no crash file ---" }

$s = RS "$d\shim_stats.log"
if ($s -ne "[none]") {
  Write-Host "--- shim_stats.log ---"
  ($s -split "`n") | Where-Object { $_.Trim() -ne "" } | ForEach-Object { Write-Host ("  " + $_.Trim()) }
} else { Write-Host "--- no stats (crashed before 120 frames?) ---" }

# screenshot if alive
if (-not $p.HasExited) {
  [RZ]::SetForegroundWindow($script:win) | Out-Null
  Start-Sleep -Milliseconds 1500
  $r = New-Object RZ+RECT
  [RZ]::GetWindowRect($script:win, [ref]$r) | Out-Null
  $w = $r.Right-$r.Left; $h2 = $r.Bottom-$r.Top
  $out = "$WS\soku_d3d9glide\match_cpu.png"
  $bmp = New-Object System.Drawing.Bitmap $w, $h2
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.CopyFromScreen($r.Left, $r.Top, 0, 0, (New-Object System.Drawing.Size $w, $h2))
  $bmp.Save($out, [System.Drawing.Imaging.ImageFormat]::Png)
  $g.Dispose(); $bmp.Dispose()
  Write-Host ("screenshot -> match_cpu.png  (" + $w + "x" + $h2 + ")")
  Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
}
