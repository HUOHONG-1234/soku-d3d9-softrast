$ErrorActionPreference = "Continue"
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public class MK {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr SetActiveWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte sc, uint f, UIntPtr e);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, int dx, int dy, uint d, UIntPtr e);
  public delegate bool EnumProc(IntPtr h, IntPtr l);
}
"@
$KEYUP = 0x0002
function Press($vk) {
  [MK]::keybd_event($vk,0,0,[UIntPtr]::Zero); Start-Sleep -Milliseconds 70
  [MK]::keybd_event($vk,0,$KEYUP,[UIntPtr]::Zero); Start-Sleep -Milliseconds 300
}

$d = "D:\th123"
$WS = "E:\OpenClaw\.openclaw\workspace"

Get-Process -Name "th123*" -ErrorAction SilentlyContinue | ForEach-Object { Stop-Process -Id $_.Id -Force -ErrorAction SilentlyContinue }
Start-Sleep -Seconds 2

$p = Start-Process -FilePath "$d\th123.exe" -WorkingDirectory $d -PassThru
Start-Sleep -Seconds 20
if ($p.HasExited) { Write-Host ("crashed at menu, code=" + $p.ExitCode); exit 1 }

$tp = $p.Id
$script:win = [IntPtr]::Zero
$cb = [MK+EnumProc]{
  param($h,$l)
  $wp = 0
  [MK]::GetWindowThreadProcessId($h, [ref]$wp) | Out-Null
  if ($wp -eq $tp -and [MK]::IsWindowVisible($h)) {
    $sb = New-Object System.Text.StringBuilder 256
    [MK]::GetClassName($h, $sb, 256) | Out-Null
    if ($sb.ToString() -eq "th123_110a") { $script:win = $h }
  }
  return $true
}
[MK]::EnumWindows($cb, [IntPtr]::Zero) | Out-Null
if ($script:win -eq [IntPtr]::Zero) { Write-Host "[FAIL] no window"; Stop-Process -Id $p.Id -Force; exit 1 }

[MK]::ShowWindow($script:win, 5) | Out-Null
[MK]::SetForegroundWindow($script:win) | Out-Null
[MK]::SetActiveWindow($script:win) | Out-Null
Start-Sleep -Milliseconds 1500

# click into the window so DXVK knows it has focus
$r0 = New-Object MK+RECT
[MK]::GetWindowRect($script:win, [ref]$r0) | Out-Null
$cx = [int](($r0.Left + $r0.Right)/2); $cy = [int](($r0.Top + $r0.Bottom)/2)
[MK]::SetCursorPos($cx, $cy) | Out-Null
Start-Sleep -Milliseconds 400
[MK]::mouse_event(0x0002, 0, 0, 0, [UIntPtr]::Zero)   # left down
[MK]::mouse_event(0x0004, 0, 0, 0, [UIntPtr]::Zero)   # left up
Start-Sleep -Milliseconds 900

function Shot($name) {
  $r = New-Object MK+RECT
  [MK]::GetWindowRect($script:win, [ref]$r) | Out-Null
  $w = $r.Right-$r.Left; $h2 = $r.Bottom-$r.Top
  $f = "$WS\soku_d3d9glide\$name.png"
  $bmp = New-Object System.Drawing.Bitmap $w, $h2
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.CopyFromScreen($r.Left, $r.Top, 0, 0, (New-Object System.Drawing.Size $w, $h2))
  $bmp.Save($f, [System.Drawing.Imaging.ImageFormat]::Png)
  $g.Dispose(); $bmp.Dispose()
  # quick stats
  $b2 = New-Object System.Drawing.Bitmap $f
  $s=0;$nb=0
  for ($y=3;$y -lt $b2.Height;$y+=9) { for ($x=3;$x -lt $b2.Width;$x+=9) {
    $c=$b2.GetPixel($x,$y); $s++; if (($c.R+$c.G+$c.B) -gt 36) { $nb++ } } }
  $b2.Dispose()
  Write-Host ("  {0}: nonBlack={1}%" -f $name, [math]::Round(100.0*$nb/$s,1))
}

Write-Host "=== shot 1: menu ==="
Shot "mk_menu"

Write-Host "=== navigating to Vs COM ==="
Press 0x28; Press 0x28           # Down Down -> Vs COM
Press 0x0D                       # Enter
Start-Sleep -Seconds 3
Shot "mk_charselect"
Press 0x0D                       # confirm character
Start-Sleep -Seconds 4
Shot "mk_stage"
Press 0x0D                       # confirm stage
Start-Sleep -Seconds 8
Write-Host "=== shot 2: in match ==="
Shot "mk_match"
Start-Sleep -Seconds 4
Shot "mk_match2"

if ($p.HasExited) { Write-Host ("!!! crashed, code=" + $p.ExitCode) } else { Write-Host "still alive"; Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
