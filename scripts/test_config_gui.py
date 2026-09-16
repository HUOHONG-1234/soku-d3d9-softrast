# -*- coding: utf-8 -*-
"""测管理器: 逐个点后端单选 + 部署按钮, 看谁会崩。
PowerShell 5.1 按 GBK 读 .ps1, 中文脚本必被啃 —— 改用 Python。"""
import io, sys, os, time, ctypes
from ctypes import wintypes
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace')

u32 = ctypes.WinDLL('user32', use_last_error=True)

BM_CLICK = 0x00F5
WM_COMMAND = 0x0111

EnumProc = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)

u32.EnumWindows.argtypes = [EnumProc, wintypes.LPARAM]
u32.EnumChildWindows.argtypes = [wintypes.HWND, EnumProc, wintypes.LPARAM]
u32.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
u32.GetClassNameW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
u32.GetWindowTextW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
u32.SendMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
u32.IsWindowEnabled.argtypes = [wintypes.HWND]

EXE = r'E:\OpenClaw\.openclaw\workspace\soku_loader\tools\soku_config.exe'
GAME = r'D:\th123'

import subprocess
subprocess.run(['taskkill', '/F', '/IM', 'soku_config.exe'],
               capture_output=True)
time.sleep(1)

print('=== 启动管理器 ===')
p = subprocess.Popen([EXE, GAME])
time.sleep(3)
if p.poll() is not None:
    print('!!! 启动即崩 code=%s' % p.returncode)
    sys.exit(1)
print('  pid=%d' % p.pid)

main = []
def find_main(h, l):
    pid = wintypes.DWORD()
    u32.GetWindowThreadProcessId(h, ctypes.byref(pid))
    if pid.value == p.pid:
        buf = ctypes.create_unicode_buffer(256)
        u32.GetClassNameW(h, buf, 256)
        if buf.value == 'SokuConfigWnd':
            main.append(h)
    return True
u32.EnumWindows(EnumProc(find_main), 0)
if not main:
    print('[FAIL] 找不到主窗口')
    p.kill(); sys.exit(1)
hwnd_main = main[0]
print('  主窗口 %s' % hwnd_main)

def children(hwnd):
    out = []
    def cb(h, l):
        c = ctypes.create_unicode_buffer(256); u32.GetClassNameW(h, c, 256)
        t = ctypes.create_unicode_buffer(256); u32.GetWindowTextW(h, t, 256)
        out.append((h, c.value, t.value))
        return True
    u32.EnumChildWindows(hwnd, EnumProc(cb), 0)
    return out

def dump(tag):
    ks = children(hwnd_main)
    btns = [k for k in ks if k[1] in ('Button', 'Static')]
    print('--- %s : %d 个子控件 (%d 按钮/静态) ---' % (tag, len(ks), len(btns)))
    for h, c, t in btns:
        if t:
            print('    [%-6s] %-30s enabled=%s' % (c, t, bool(u32.IsWindowEnabled(h))))
    return ks

ks = dump('初始')

def click(name):
    hit = None
    for h, c, t in ks:
        if name in t:
            hit = h; break
    if hit is None:
        print('  [MISS] 找不到: %s' % name)
        return False
    u32.SendMessageW(hit, BM_CLICK, 0, 0)
    time.sleep(0.6)
    return True

print('')
print('=== 逐个点后端单选 ===')
for n in ['原生 D3D9', 'DXVK (Vulkan)', 'D3D9On12 (D3D12)',
          'dgVoodoo2 (D3D11)', 'Glide (D3D9->Glide)', 'CPU 渲染']:
    if p.poll() is not None:
        print('  !!! 已崩 (点 "%s" 时) code=%s' % (n, p.returncode))
        break
    click(n)
    print('  点 %-22s -> 存活=%s' % (n, p.poll() is None))

if p.poll() is None:
    print('')
    print('=== 点「部署后端」 ===')
    click('部署后端')
    print('  存活=%s' % (p.poll() is None))
    time.sleep(1)

    ks2 = children(hwnd_main)
    status = [t for h, c, t in ks2 if c == 'Static' and t and ('部署' in t or '后端' in t or '文件' in t)]
    print('  状态栏: %s' % (status or '(无)'))

    print('')
    print('=== 根目录（部署后）===')
    for f in sorted(os.listdir(GAME)):
        fp = os.path.join(GAME, f)
        if os.path.isfile(fp) and f.lower().endswith(('.dll', '.ini')):
            print('    %-28s %10d B' % (f, os.path.getsize(fp)))

print('')
print('=== 点「全部回收」 ===')
if p.poll() is None:
    ks3 = children(hwnd_main)
    for h, c, t in ks3:
        if '全部回收' in t:
            u32.SendMessageW(h, BM_CLICK, 0, 0)
            break
    time.sleep(1)
    print('  存活=%s' % (p.poll() is None))
    print('=== 根目录（回收后）===')
    for f in sorted(os.listdir(GAME)):
        fp = os.path.join(GAME, f)
        if os.path.isfile(fp) and f.lower().endswith(('.dll', '.ini')):
            print('    %-28s %10d B' % (f, os.path.getsize(fp)))

if p.poll() is None:
    p.kill()
print('DONE')
