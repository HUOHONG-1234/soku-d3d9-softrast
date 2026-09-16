# -*- coding: utf-8 -*-
"""生成 d3dx9_33 代理的 .def：
   除少数要记账的函数外，其余全部【转发】给 d3dx9_real.dll。
   转发不需要写代码，零成本 —— 334 个导出才可维护。"""
import io, sys, os, re, subprocess

sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace')

VC = r'D:\微软大战代码\VC\Tools\MSVC\14.50.35717\bin\Hostx86\x86\dumpbin.exe'
REAL = r'C:\Windows\SysWOW64\d3dx9_33.dll'
OUTDEF = r'$RepoRoot\src\d3dx9.def'

# 要自己实现（记账）的导出 —— 其余全部转发
# 只收 D3DXCreateTexture: 签名已从 mingw-w64 + wine 两份头文件交叉验证。
# D3DXCreateEffect 参数复杂，先纯转发，需要时再收。
OVERRIDE = ['D3DXCreateTexture']

out = subprocess.run([VC, '/exports', REAL], capture_output=True, text=True,
                     encoding='gbk', errors='replace').stdout
names = []
for line in out.splitlines():
    m = re.match(r'\s+(\d+)\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]{8}\s+(\S+)\s*$', line)
    if m:
        names.append(m.group(2))

print('真实导出: %d 个' % len(names))
missing = [o for o in OVERRIDE if o not in names]
if missing:
    print('[FAIL] 要覆盖的名字不在导出表里: %s' % missing)
    sys.exit(1)

fwd = [n for n in names if n not in OVERRIDE]
print('转发: %d 个   自实现: %d 个 (%s)' % (len(fwd), len(OVERRIDE), ', '.join(OVERRIDE)))

lines = ['LIBRARY d3dx9_33', 'EXPORTS']
for n in fwd:
    lines.append('    %s = d3dx9_real.%s' % (n, n))
for n in OVERRIDE:
    lines.append('    %s' % n)
open(OUTDEF, 'w', encoding='ascii', newline='\r\n').write('\n'.join(lines) + '\n')
print('已写 %s  (%d 行)' % (OUTDEF, len(lines)))
