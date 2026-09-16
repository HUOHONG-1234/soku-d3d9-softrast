$RepoRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$ErrorActionPreference = "Continue"
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public class TP {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint f);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr SetActiveWindow(IntPtr h);
  public delegate bool EnumProc(IntPtr h, IntPtr l);
}
"@
$TOPMOST   = [IntPtr](-1)
$NOTOPMOST = [IntPtr](-2)
$SWP_SHOWWIN = 0x0040
$SWP_NOMOVE  = 0x0002
$SWP_NOSIZE  = 0x0001

$d = "D:\th123"
$out = "$RepoRoot\cpu_run.png"
Remove-Item $out -Force -ErrorAction SilentlyContinue

Get-Process -Name "th123*" -ErrorAction SilentlyContinue | ForEach-Object { Stop-Process -Id $_.Id -Force -ErrorAction SilentlyContinue }
Start-Sleep -Seconds 2

$p = Start-Process -FilePath "$d\th123.exe" -WorkingDirectory $d -PassThru
Start-Sleep -Seconds 30
if ($p.HasExited) { Write-Host ("exited code=" + $p.ExitCode); exit 1 }
$tp = $p.Id

$script:list = @()
$cb = [TP+EnumProc]{
  param($h,$l)
  $wp = 0
  [TP]::GetWindowThreadProcessId($h, [ref]$wp) | Out-Null
  if ($wp -eq $tp) {
    $sb = New-Object System.Text.StringBuilder 256
    [TP]::GetClassName($h, $sb, 256) | Out-Null
    $tt = New-Object System.Text.StringBuilder 256
    [TP]::GetWindowText($h, $tt, 256) | Out-Null
    $r = New-Object TP+RECT
    [TP]::GetWindowRect($h, [ref]$r) | Out-Null
    $script:list += [pscustomobject]@{
      hwnd=$h; cls=$sb.ToString(); title=$tt.ToString(); vis=[TP]::IsWindowVisible($h)
      x=$r.Left; y=$r.Top; w=($r.Right-$r.Left); hh=($r.Bottom-$r.Top)
    }
  }
  return $true
}
[TP]::EnumWindows($cb, [IntPtr]::Zero) | Out-Null

Write-Host "=== th123.exe 的所有窗口 ==="
$script:list | ForEach-Object { Write-Host ("  cls='{0}' vis={1} {2}x{3} @({4},{5}) title='{6}'" -f $_.cls,$_.vis,$_.w,$_.hh,$_.x,$_.y,$_.title) }

# 选游戏主窗口：可见 + 够大 + 不是代理/看门狗类
$game = $script:list | Where-Object {
  $_.vis -and $_.w -gt 400 -and $_.hh -gt 300 -and
  $_.cls -notmatch "Proxy|Watchdog|IME|MSCTF|tooltips"
} | Sort-Object { $_.w * $_.hh } -Descending | Select-Object -First 1

if (-not $game) { Write-Host "[FAIL] 找不到游戏主窗口"; Stop-Process -Id $p.Id -Force; exit 1 }
Write-Host ("[选中] cls='{0}' {1}x{2} @({3},{4})" -f $game.cls,$game.w,$game.hh,$game.x,$game.y)

# 强制置顶 + 激活，确保屏幕坐标处就是它
[TP]::ShowWindow($game.hwnd, 9) | Out-Null
[TP]::SetWindowPos($game.hwnd, $TOPMOST, $game.x, $game.y, $game.w, $game.hh, $SWP_SHOWWIN) | Out-Null
[TP]::SetForegroundWindow($game.hwnd) | Out-Null
[TP]::SetActiveWindow($game.hwnd) | Out-Null
Start-Sleep -Milliseconds 2500

$r = New-Object TP+RECT
[TP]::GetWindowRect($game.hwnd, [ref]$r) | Out-Null
$w = $r.Right - $r.Left; $h2 = $r.Bottom - $r.Top
Write-Host ("实际 {0}x{1} @({2},{3})" -f $w,$h2,$r.Left,$r.Top)

$bmp = New-Object System.Drawing.Bitmap $w, $h2
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($r.Left, $r.Top, 0, 0, (New-Object System.Drawing.Size $w, $h2))
$bmp.Save($out, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $bmp.Dispose()

# 恢复非置顶
[TP]::SetWindowPos($game.hwnd, $NOTOPMOST, 0, 0, 0, 0, $SWP_NOMOVE -bor $SWP_NOSIZE) | Out-Null

$b2 = New-Object System.Drawing.Bitmap $out
$s=0;$nb=0;$buckets=@{}
for ($y=3; $y -lt $b2.Height; $y+=7) {
  for ($x=3; $x -lt $b2.Width; $x+=7) {
    $c=$b2.GetPixel($x,$y); $s++
    if (($c.R+$c.G+$c.B) -gt 36) { $nb++ }
    $k = "{0},{1},{2}" -f [int]($c.R/32),[int]($c.G/32),[int]($c.B/32)
    if ($buckets.ContainsKey($k)) { $buckets[$k]++ } else { $buckets[$k]=1 }
  }
}
$b2.Dispose()
Write-Host ("非黑 {0}%  色彩桶 {1}" -f [math]::Round(100.0*$nb/$s,1), $buckets.Count)
$buckets.GetEnumerator() | Sort-Object Value -Descending | Select-Object -First 5 |
  ForEach-Object { Write-Host ("   " + $_.Key + " x" + $_.Value) }

Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
