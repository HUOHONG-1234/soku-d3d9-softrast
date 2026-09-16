# -*- coding: utf-8 -*-
"""严格版 vtable 对齐检查。
上一版 bug: STDMETHOD_(Ret, Name) 里的 Ret 被当成了方法名 -> 结论不可信。
这次同时支持两种写法，并把 IUnknown 三条也算进去逐位对比。"""
import io, sys, re
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace')

hdr = r'D:\Windows Kits\10\Include\10.0.26100.0\shared\d3d9.h'
lines = open(hdr, 'r', encoding='utf-8', errors='replace').read().splitlines()

def iface_methods(name):
    start = None
    for i, l in enumerate(lines):
        if re.match(r'#define INTERFACE ' + name + r'\s*$', l):
            start = i
            break
    if start is None:
        return None, 0, 0
    end = None
    for i in range(start, len(lines)):
        if lines[i].strip().startswith('};'):
            end = i
            break
    out = []
    for l in lines[start:end]:
        m = re.match(r'\s*STDMETHOD_\(\s*[^,]+,\s*([A-Za-z_]\w*)\s*\)', l)
        if m:
            out.append(m.group(1)); continue
        m = re.match(r'\s*STDMETHOD\(\s*([A-Za-z_]\w*)\s*\)', l)
        if m:
            out.append(m.group(1))
    return out, start + 1, end + 1

spec, s0, e0 = iface_methods('IDirect3DDevice9')
print('d3d9.h IDirect3DDevice9: %d 方法 (L%d..L%d)' % (len(spec), s0, e0))
print('  前 6: ' + ' '.join(spec[:6]))

stub = open(r'$RepoRoot\src\stubs_device9.h',
            'r', encoding='utf-8', errors='replace').read()
stubm = re.findall(r'STDMETHODCALLTYPE\s+([A-Za-z_]\w*)\s*\(', stub)
print('stubs_device9.h: %d 方法' % len(stubm))
print('  前 6: ' + ' '.join(stubm[:6]))

print('')
print('=== 逐位对比 (stubs 应等于 spec 去掉 IUnknown 3 条) ===')
spec_rest = spec[3:]          # 去掉 QueryInterface/AddRef/Release
print('  spec 去掉 IUnknown: %d   stubs: %d' % (len(spec_rest), len(stubm)))

ok = True
for i in range(max(len(spec_rest), len(stubm))):
    a = spec_rest[i] if i < len(spec_rest) else '---'
    b = stubm[i] if i < len(stubm) else '---'
    if a != b:
        print('  [%3d] d3d9.h=%-32s stubs=%s' % (i, a, b))
        ok = False
if ok:
    print('  *** 完全一致 -> vtable 对齐正确 ***')

print('')
print('=== 关键方法槽位 (原始 vtable index, 含 IUnknown) ===')
for key in ['CreateTexture', 'CreateVertexBuffer', 'CreateIndexBuffer', 'SetTexture',
            'DrawPrimitiveUP', 'DrawIndexedPrimitiveUP', 'SetFVF', 'SetStreamSource',
            'SetRenderState', 'Present', 'Clear', 'BeginScene', 'EndScene',
            'CreatePixelShader', 'CreateVertexShader', 'SetPixelShader']:
    a = spec.index(key) if key in spec else -1
    b = (stubm.index(key) + 3) if key in stubm else -1
    mark = 'OK ' if (a == b and a >= 0) else '!! '
    print('  %s%-24s d3d9.h=%3d  stubs(+3)=%3d' % (mark, key, a, b))
