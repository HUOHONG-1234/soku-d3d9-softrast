# -*- coding: utf-8 -*-
"""gen_stubs.py —— 从 d3d9.h 抽取 COM 方法骨架 + Core 函数原型

输出:
  src/stubs_d3d9.h      IDirect3D9 的方法体（含进 class 里）
  src/stubs_device9.h   IDirect3DDevice9 的方法体
  src/core_proto.h      Core_* 的精确原型（供 shim.cpp 定义时对齐）

设计: 核心方法映射到 Core_*，其余全部打桩并记录调用 ——
      第一次跑游戏就能看到它到底用了哪些方法，相当于一份需求清单。
"""
import re, os

HDR = r"D:\Windows Kits\10\Include\10.0.26100.0\shared\d3d9.h"
OUT = r"E:\OpenClaw\.openclaw\workspace\soku_d3d9glide\src"
os.makedirs(OUT, exist_ok=True)

src = open(HDR, encoding="utf-8", errors="ignore").read()

# 手写的方法，不生成
SKIP = {"QueryInterface", "AddRef", "Release", "CreateDevice"}

# 需特殊处理的方法 —— 不排除, 而是改调用 H_* 实现。
# 【关键】vtable 槽位 = 声明顺序, 所以绝对不能把这些挪到别处!
SPECIAL = {
    "IDirect3D9":      {"GetDeviceCaps", "GetAdapterDisplayMode", "GetAdapterIdentifier"},
    "IDirect3DDevice9": {"GetDeviceCaps", "GetDisplayMode", "GetSwapChain", "GetBackBuffer",
                          "GetRenderTarget", "GetDepthStencilSurface", "GetFrontBufferData"},
    "IDirect3DSurface9": {"GetDevice", "GetDesc", "LockRect", "UnlockRect", "GetDC", "ReleaseDC",
                           "SetPrivateData", "GetPrivateData", "FreePrivateData"},
    "IDirect3DSwapChain9": {"Present", "GetFrontBufferData", "GetBackBuffer", "GetRasterStatus",
                            "GetDisplayMode", "GetDevice", "GetPresentParameters"},
}

# 注意: IDirect3D9 和 IDirect3DDevice9 都有 GetDeviceCaps, 签名不同。
# 所以 H_* 实现要带接口前缀, 否则重定义。
PREFIX = {"IDirect3D9": "H9_", "IDirect3DDevice9": "HD_",
          "IDirect3DSwapChain9": "HS_", "IDirect3DSurface9": "HF_"}

CORE = {
    "Clear", "Present", "BeginScene", "EndScene",
    "DrawPrimitiveUP", "DrawPrimitive", "DrawIndexedPrimitive",
    "SetRenderState", "GetRenderState",
    "SetTexture", "CreateTexture", "SetTextureStageState", "SetSamplerState",
    "SetTransform", "SetViewport", "SetStreamSource", "SetIndices", "SetFVF",
    "CreateVertexBuffer", "CreateIndexBuffer",
}


def extract(iface):
    m = re.search(r"DECLARE_INTERFACE_\(" + iface + r",\s*\w+\)\s*\{(.*?)\n\};", src, re.S)
    if not m:
        return []
    res = []
    for mm in re.finditer(r"STDMETHOD(_)?\(([^)]*)\)\s*\((.*?)\)\s*PURE;", m.group(1), re.S):
        explicit = (mm.group(1) == "_")
        head = mm.group(2).strip()
        args = mm.group(3).strip()
        if explicit:
            i = head.find(",")
            rtype = head[:i].strip()
            fname = head[i + 1:].strip()
        else:
            rtype = "HRESULT"
            fname = head
        res.append((rtype, fname, args))
    return res


def norm(a):
    a = a.replace("THIS_", " ").replace("THIS", " ")
    return " ".join(a.split())


def arg_names(a):
    a = norm(a)
    if not a or a.strip() == "void":
        return []
    names = []
    for p in a.split(","):
        p = p.strip()
        if not p:
            continue
        toks = p.split()
        if len(toks) == 1:
            # 只有类型、没有参数名（如 DrawIndexedPrimitive 的第一个参数）
            names.append("a%d" % len(names))
        else:
            m = re.search(r"(\w+)\s*(\[[^\]]*\])?\s*$", p)
            names.append(m.group(1) if m else ("a%d" % len(names)))
    return names


def fix_proto(a):
    """给无名参数补上占位名, 供 Core_* 原型使用"""
    if not a or a.strip() == "void":
        return a
    out = []
    for i, p in enumerate(a.split(",")):
        p = p.strip()
        if not p:
            continue
        toks = p.split()
        out.append(p if len(toks) > 1 else (p + " a%d" % i))
    return ",".join(out)


def gen(iface, special=None):
    if special is None:
        special = SPECIAL.get(iface, set())
    methods, protos = [], []
    for rtype, fname, args in extract(iface):
        if fname in SKIP:
            continue
        a = norm(args)
        an = arg_names(args)
        if fname in special:
            # 特殊方法: 保留在 vtable 正确位置, 实现转发到接口前缀的 H*_ 函数
            pre = PREFIX.get(iface, "H_")
            if rtype == "void":
                body = "%s%s(%s);" % (pre, fname, ", ".join(an))
            else:
                body = "return %s%s(%s);" % (pre, fname, ", ".join(an))
        elif fname in CORE:
            body = "return Core_%s(%s);" % (fname, ", ".join(an))
            # 原型里也要把无名参数补上名字
            protos.append("%s Core_%s(%s);" % (rtype, fname, fix_proto(a)))
        else:
            # 打桩: 记录调用。
            # 关键: 如果出参是指针, 就不能返回 S_OK —— 游戏拿到空指针会直接崩。
            # 返回失败码更安全: 游戏会自己走异常分支, 我们也能从日志看出它依赖什么。
            stmts = ['LogStub("%s");' % fname]
            has_ptr_out = False
            for p in a.split(","):
                p = p.strip()
                m = re.match(r"^(\w+)\s*(\*+)\s*(\w+)$", p)
                if m and m.group(3).startswith("pp"):
                    stmts.append("if (%s) *%s = nullptr;" % (m.group(3), m.group(3)))
                    has_ptr_out = True
            if rtype == "HRESULT":
                stmts.append("return D3DERR_INVALIDCALL;" if has_ptr_out else "return S_OK;")
            elif rtype == "void":
                pass
            else:
                stmts.append("return {};")   # 枚举/DWORD 等通用零值
            body = " ".join(stmts)
        methods.append("    %s STDMETHODCALLTYPE %s(%s) override { %s }" % (rtype, fname, fix_proto(a), body))
    return methods, protos


d9m, _ = gen("IDirect3D9")
dvm, dprotos = gen("IDirect3DDevice9")

# 额外两个接口: 交换链与表面 (游戏不检查 GetSwapChain 失败, 必须给真对象)
HW_SWAP = {"Present", "GetBackBuffer", "GetFrontBufferData", "GetDisplayMode",
           "GetRasterStatus", "GetPresentParameters"}
HW_SURF = {"GetDesc", "LockRect", "UnlockRect", "GetDC", "ReleaseDC",
           "SetPrivateData", "GetPrivateData", "FreePrivateData"}
swm, _ = gen("IDirect3DSwapChain9", HW_SWAP)
sfm, _ = gen("IDirect3DSurface9",   HW_SURF)

open(os.path.join(OUT, "stubs_swapchain9.h"), "w", encoding="utf-8").write(
    "/* 自动生成, 勿手改: tools/gen_stubs.py */\n#pragma once\n" + "\n".join(swm) + "\n")
open(os.path.join(OUT, "stubs_surface9.h"), "w", encoding="utf-8").write(
    "/* 自动生成, 勿手改: tools/gen_stubs.py */\n#pragma once\n" + "\n".join(sfm) + "\n")
print("IDirect3DSwapChain9 生成方法:", len(swm))
print("IDirect3DSurface9   生成方法:", len(sfm))

open(os.path.join(OUT, "stubs_d3d9.h"), "w", encoding="utf-8").write(
    "/* 自动生成, 勿手改: tools/gen_stubs.py */\n#pragma once\n" + "\n".join(d9m) + "\n")
open(os.path.join(OUT, "stubs_device9.h"), "w", encoding="utf-8").write(
    "/* 自动生成, 勿手改: tools/gen_stubs.py */\n#pragma once\n" + "\n".join(dvm) + "\n")
open(os.path.join(OUT, "core_proto.h"), "w", encoding="utf-8").write(
    "/* 自动生成, 勿手改: tools/gen_stubs.py  —— Core_* 精确原型 */\n#pragma once\n"
    + "\n".join(dprotos) + "\n")

print("IDirect3D9       生成方法:", len(d9m))
print("IDirect3DDevice9 生成方法:", len(dvm))
print("Core_* 原型:", len(dprotos))
for p in dprotos:
    print("   ", p)
