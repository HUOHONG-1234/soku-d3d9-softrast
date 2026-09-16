# -*- coding: utf-8 -*-
"""Controlled A/B test: does vsync gate the frame rate?

Run A: baseline (no dxvk.conf)
Run B: vsync forced off (d3d9.presentInterval=0, via DXVK_CONFIG_FILE)

Same scene (title screen, no input), same duration, same startup wait.
NOTE: the previous vsync test was probably invalid because DXVK's real dll
lives in backends\\dxvk\\ and may not read a conf placed in the game root.
"""
import io, sys, os, re, time, subprocess

sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace')

GAME = r'D:\th123'
EXE = os.path.join(GAME, 'th123.exe')
STATS = os.path.join(GAME, 'shim_stats.log')
CONF_R = os.path.join(GAME, 'dxvk.conf')
CONF_B = os.path.join(GAME, 'backends', 'dxvk', 'dxvk.conf')

CONF_TEXT = (
    "d3d9.presentInterval = 0\n"
    "dxgi.syncInterval = 0\n"
    "d3d9.tearFree = False\n"
    "d3d9.numBackBuffers = 3\n"
    "dxgi.numBackBuffers = 3\n"
)


def kill_game():
    subprocess.run(['taskkill', '/F', '/IM', 'th123.exe'],
                   capture_output=True)
    time.sleep(3)


def read_stats():
    try:
        return open(STATS, 'r', encoding='gbk', errors='replace').read()
    except Exception:
        return ''


def last_fps(txt):
    m = re.findall(r'([\d.]+)\s*fps', txt)
    return float(m[-1]) if m else None


def present_count(txt):
    m = re.search(r'Core_Present\s+(\d+)', txt)
    return int(m.group(1)) if m else None


def run_once(tag, use_conf, wait=22, window=16):
    kill_game()
    for p in (CONF_R, CONF_B):
        try:
            os.remove(p)
        except Exception:
            pass
    try:
        os.remove(STATS)
    except Exception:
        pass

    env = dict(os.environ)
    if use_conf:
        with open(CONF_B, 'w') as f:
            f.write(CONF_TEXT)
        env['DXVK_CONFIG_FILE'] = CONF_B

    p = subprocess.Popen([EXE], cwd=GAME, env=env)
    time.sleep(wait)
    if p.poll() is not None:
        print('  [%s] crashed early code=%s' % (tag, p.returncode))
        return None

    t0 = time.time()
    c1 = present_count(read_stats())
    time.sleep(window)
    c2 = present_count(read_stats())
    el = time.time() - t0
    txt = read_stats()

    delta_fps = None
    if c1 is not None and c2 is not None and el > 0:
        delta_fps = (c2 - c1) / el

    r = {
        'tag': tag,
        'conf': use_conf,
        'delta_fps': delta_fps,
        'log_fps': last_fps(txt),
        'c1': c1, 'c2': c2, 'el': el,
    }
    try:
        p.kill()
    except Exception:
        pass
    time.sleep(2)
    return r


print('=== A/B: vsync on vs off ===')
a = run_once('A_baseline', False)
print('  A baseline : delta_fps=%s  log_fps=%s  (Present %s -> %s / %.1fs)'
      % (a['delta_fps'], a['log_fps'], a['c1'], a['c2'], a['el']) if a else '  A FAILED')

b = run_once('B_novsync', True)
print('  B novsync  : delta_fps=%s  log_fps=%s  (Present %s -> %s / %.1fs)'
      % (b['delta_fps'], b['log_fps'], b['c1'], b['c2'], b['el']) if b else '  B FAILED')

if a and b and a['delta_fps'] and b['delta_fps']:
    print()
    print('  ===> A=%.2f fps   B=%.2f fps   比值 %.2fx'
          % (a['delta_fps'], b['delta_fps'], b['delta_fps'] / a['delta_fps']))
    if b['delta_fps'] > a['delta_fps'] * 1.4:
        print('  ===> 结论: vsync 确实是闸门')
    else:
        print('  ===> 结论: vsync 不是闸门')

# restore: leave no conf behind
for p in (CONF_R, CONF_B):
    try:
        os.remove(p)
    except Exception:
        pass
print()
print('已清理测试用 dxvk.conf')
