#!/usr/bin/env python3
# Drives the PRIVATE headless mutter only (its own session bus): monitor scale
# via DisplayConfig, the app's window geometry via the extension, and real
# title-bar move grabs via org.gnome.Mutter.RemoteDesktop pointer injection.
import json, sys, time
from gi.repository import Gio, GLib

bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)


def call(dest, path, iface, method, args=None, rtype=None):
    return bus.call_sync(dest, path, iface, method, args, GLib.VariantType(rtype) if rtype else None,
                         Gio.DBusCallFlags.NONE, 10000, None)


def now_us():
    return int(time.monotonic() * 1e6)


def set_scale(scale):
    DC = ('org.gnome.Mutter.DisplayConfig', '/org/gnome/Mutter/DisplayConfig', 'org.gnome.Mutter.DisplayConfig')
    st = call(*DC, 'GetCurrentState').unpack()
    serial, monitors = st[0], st[1]
    mon = monitors[0]
    conn = mon[0][0]
    modes = mon[1]
    cur = [m for m in modes if m[6].get('is-current')] or modes
    mode = cur[0]
    scales = list(mode[5])
    best = min(scales, key=lambda s: abs(s - scale))
    print(f'driver: monitor {conn} mode {mode[0]} scales {[round(s, 4) for s in scales]} -> {best}')
    lm = [(0, 0, best, 0, True, [(conn, mode[0], {})])]
    call(*DC, 'ApplyMonitorsConfig', GLib.Variant('(uua(iiduba(ssa{sv}))a{sv})', (serial, 1, lm, {})))


def ready(pid, out):
    r = call('org.displayxr.WindowGeometry', '/org/displayxr/WindowGeometry', 'org.displayxr.WindowGeometry1',
             'GetWindows', None, '(s)').unpack()[0]
    for w in json.loads(r)['windows']:
        if w['pid'] == pid:
            json.dump(w, open(out, 'w'))
            print('driver: window', w['frame'], w['buffer'], w['monitor'])
            return True
    return False


# Each profile: (pre-motion pause s, [(dx, dy)...] per event, event interval s, release pause s)
# A kind prefixed `rmb_` runs the same motion as a RIGHT-button content drag
# (the extension's pointer drag, runtime#1754) instead of a left-button
# title-bar drag (mutter's move grab).
def profile(kind):
    if kind.startswith('rmb_'):
        kind = kind[4:]
    if kind == 'slow_h':
        return 0.3, [(1.0, 0.0)] * 300, 0.008, 0.3
    if kind == 'fast_h':
        return 0.3, [(10.0, 0.0)] * 60, 0.008, 0.3
    if kind == 'slow_d':
        return 0.3, [(0.94, 0.34)] * 300, 0.008, 0.3
    if kind == 'fast_d':
        return 0.3, [(9.4, 3.4)] * 60, 0.008, 0.3
    if kind == 'reverse':
        return 0.3, [(4.0, 0.0)] * 60 + [(-4.0, 0.0)] * 60, 0.008, 0.3
    if kind == 'rest_start':
        return 1.0, [(3.0, 0.0)] * 100, 0.008, 0.3
    if kind == 'release_mid':
        return 0.3, [(6.0, 0.0)] * 80, 0.008, 0.0
    if kind == 'back_h':  # return leftwards, to keep the window on screen
        return 0.3, [(-6.0, 0.0)] * 100, 0.008, 0.3
    if kind == 'tile_top':  # to the top edge: edge tiling maximises on release
        return 0.3, [(0.0, -12.0)] * 80, 0.008, 0.6
    if kind == 'freeze_app':  # SIGSTOP the app (our own child) 500 ms mid-drag
        return 0.3, [(3.0, 0.0)] * 150, 0.008, 0.3
    if kind == 'tile_left':
        return 0.3, [(-15.0, 0.0)] * 120, 0.008, 0.6
    raise SystemExit(f'unknown drag {kind}')


def drag(ready_path, kind, marks):
    w = json.load(open(ready_path))
    f, b = w['frame'], w['buffer']
    W, Hh = w['monitor']['w'], w['monitor']['h']
    RD = ('org.gnome.Mutter.RemoteDesktop', '/org/gnome/Mutter/RemoteDesktop', 'org.gnome.Mutter.RemoteDesktop')
    path = call(*RD, 'CreateSession', None, '(o)').unpack()[0]
    S = ('org.gnome.Mutter.RemoteDesktop', path, 'org.gnome.Mutter.RemoteDesktop.Session')
    call(*S, 'Start')
    time.sleep(0.3)
    # Park at the bottom-right (the top-left hot corner opens the overview),
    # then walk to the middle of the title bar (frame top .. buffer top).
    for _ in range(60):
        call(*S, 'NotifyPointerMotionRelative', GLib.Variant('(dd)', (100.0, 100.0)))
    time.sleep(0.1)
    bar = b[1] - f[1]
    rmb = kind.startswith('rmb_')
    tx = f[0] + f[2] / 2
    ty = f[1] + (bar / 2 if bar >= 8 else 6)
    if rmb:  # the middle of the content
        tx, ty = b[0] + b[2] / 2, b[1] + b[3] / 2
    button = 273 if rmb else 272  # BTN_RIGHT / BTN_LEFT
    rx, ry = float(tx - (W - 1)), float(ty - (Hh - 1))
    for _ in range(10):
        call(*S, 'NotifyPointerMotionRelative', GLib.Variant('(dd)', (rx / 10, ry / 10)))
    time.sleep(0.4)
    pre, steps, dt, rel = profile(kind)
    t_press = now_us()
    call(*S, 'NotifyPointerButton', GLib.Variant('(ib)', (button, True)))
    time.sleep(pre)
    t_move = now_us()
    import os, signal
    app_pid = int(os.environ.get('APP_PID', '0'))
    for i, (dx, dy) in enumerate(steps):
        if kind.endswith('freeze_app') and app_pid > 0 and i == 40:
            os.kill(app_pid, signal.SIGSTOP)
            t_freeze = now_us()
        if kind.endswith('freeze_app') and app_pid > 0 and i == 100:
            os.kill(app_pid, signal.SIGCONT)
            with open(marks + '.freeze', 'a') as fz:
                fz.write(f'freeze={t_freeze} thaw={now_us()}\n')
        call(*S, 'NotifyPointerMotionRelative', GLib.Variant('(dd)', (float(dx), float(dy))))
        time.sleep(dt)
    t_stop = now_us()
    time.sleep(rel)
    t_release = now_us()
    call(*S, 'NotifyPointerButton', GLib.Variant('(ib)', (button, False)))
    time.sleep(0.8)
    call(*S, 'Stop')
    with open(marks, 'a') as m:
        m.write(f'{kind} press={t_press} move={t_move} stop={t_stop} release={t_release} bar={bar}\n')
    print(f'driver: {kind} press at {tx:.0f},{ty:.0f} (bar {bar})', flush=True)


if __name__ == '__main__':
    if sys.argv[1] == 'scale':
        set_scale(float(sys.argv[2]))
    elif sys.argv[1] == 'ready':
        sys.exit(0 if ready(int(sys.argv[2]), sys.argv[3]) else 1)
    elif sys.argv[1] == 'drag':
        drag(sys.argv[2], sys.argv[3], sys.argv[4])
