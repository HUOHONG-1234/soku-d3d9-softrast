$ErrorActionPreference = "Continue"
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public class FX {
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
  public delegate bool EnumProc(IntPtr h, IntPtr l);
}
"@
$TOP = [IntPtr](-1)

$d = "D:\th123"
$WS = "E:\OpenClaw\.openclaw\workspace"
$out = "$WS\soku_d3d9glide\fps_check.png"
Remove-Item $out -Force -ErrorAction SilentlyContinue

Get-Process -Name "th123*" -ErrorAction SilentlyContinue | ForEach-Object { Stop-Process -Id $_.Id -Force -ErrorAction SilentlyContinue }
Start-Sleep -Seconds 2
Remove-Item "$d\shim_stats.log","$d\shim_glide.log" -Force -ErrorAction SilentlyContinue

$t0 = Get-Date
$p = Start-Process -FilePath "$d\th123.exe" -WorkingDirectory $d -PassThru
Start-Sleep -Seconds 18     # let it settle into the menu
if ($p.HasExited) { Write-Host ("crashed, code=" + $p.ExitCode); exit 1 }

$tp = $p.Id
$script:win = [IntPtr]::Zero
$cb = [FX+EnumProc]{
  param($h,$l)
  $wp = 0
  [FX]::GetWindowThreadProcessId($h, [ref]$wp) | Out-Null
  if ($wp -eq $tp -and [FX]::IsWindowVisible($h)) {
    $sb = New-Object System.Text.StringBuilder 256
    [FX]::GetClassName($h, $sb, 256) | Out-Null
    if ($sb.ToString() -eq "th123_110a") { $script:win = $h }
  }
  return $true
}
[FX]::EnumWindows($cb, [IntPtr]::Zero) | Out-Null
if ($script:win -eq [IntPtr]::Zero) { Write-Host "[FAIL] no window"; Stop-Process -Id $p.Id -Force; exit 1 }

[FX]::SetWindowPos($script:win, $TOP, 40, 40, 646, 509, 0x0040) | Out-Null
[FX]::SetForegroundWindow($script:win) | Out-Null
[FX]::SetActiveWindow($script:win) | Out-Null
Start-Sleep -Milliseconds 1500

# --- FPS: read Present count from stats before/after a fixed window ---
function PresentCount {
  try {
    $fs = New-Object System.IO.FileStream("$d\shim_stats.log",[System.IO.FileMode]::Open,[System.IO.FileAccess]::Read,[System.IO.FileShare]::ReadWrite)
    $sr = New-Object System.IO.StreamReader($fs,[System.Text.Encoding]::GetEncoding(936))
    $t = $sr.ReadToEnd(); $sr.Close(); $fs.Close()
    $m = [regex]::Match($t, "Core_Present\s+(\d+)")
    if ($m.Success) { return [int]$m.Groups[1].Value } else { return -1 }
  } catch { return -1 }
}
# force a stats write by waiting for the 120-frame dump
Start-Sleep -Seconds 3
$c1 = PresentCount
$w0 = Get-Date
Start-Sleep -Seconds 10
$w1 = Get-Date
$c2 = PresentCount
$secs = ($w1 - $w0).TotalSeconds
if ($c1 -ge 0 -and $c2 -ge 0 -and $c2 -gt $c1) {
  Write-Host ("FPS ~ {0:N1}   (Present {1} -> {2} in {3:N1}s)" -f (($c2-$c1)/$secs), $c1, $c2, $secs)
} else {
  Write-Host ("FPS 数据不足: c1=$c1 c2=$c2  (stats 每 120 帧才刷新)")
}

# --- screenshot ---
$r = New-Object FX+RECT
[FX]::GetWindowRect($script:win, [ref]$r) | Out-Null
$w = $r.Right-$r.Left; $h2 = $r.Bottom-$r.Top
$bmp = New-Object System.Drawing.Bitmap $w, $h2
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($r.Left, $r.Top, 0, 0, (New-Object System.Drawing.Size $w, $h2))
$bmp.Save($out, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $bmp.Dispose()
Write-Host ("screenshot -> fps_check.png ({0}x{1})" -f $w,$h2)

Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
