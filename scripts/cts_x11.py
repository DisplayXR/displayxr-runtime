#!/usr/bin/env python3
# Copyright 2026, The DisplayXR Project
# SPDX-License-Identifier: BSL-1.0
"""
X11 primitives for the Linux interactive CTS harness (#1727).

The Linux counterpart of the Win32 calls the Windows driver makes
(SetForegroundWindow, keybd_event, PrintWindow): focus a window, inject keys and
mouse buttons, and read a window's pixels. Implemented over ctypes against
libX11 + libXtst — the libraries every X client box already has — so the
harness needs no xdotool / wmctrl / ImageMagick install and no root.

Two injection routes, chosen with --route:

  send   (default) XSendEvent straight to the window (what `xdotool key
         --window` does). No focus needed, nothing else on the desktop is
         touched. The runtime's XCB pump masks the send_event bit, so the events
         decode exactly like real ones; FocusIn/Out are never generated.
  xtest  XTEST fake input, after activating the window through EWMH
         _NET_ACTIVE_WINDOW (what `xdotool windowactivate` sends). Real server
         input, so it also exercises focus and the modifier re-sync. Use it on
         a bare X server or a real Xorg session ONLY: a GNOME Wayland session
         starts Xwayland with -enable-ei-portal, which routes every XTEST
         request through the RemoteDesktop portal — a consent dialog pops up
         on the desktop, takes focus, and the input is held until someone
         answers it (measured on GNOME 50, #1727). xdotool has the same
         problem there; it is XTEST underneath.

The runtime's self-created window is XCB. Under a Wayland session it lives on
XWayland, where both routes reach it; native Wayland windows are out of reach
of either.

Subcommands (all print one line; exit 0 on success):

  find     --name DisplayXR [--newest]        -> window id (hex)
  geom     --window 0x...                     -> x y w h (root coords)
  activate --window 0x...
  key      --window 0x... KEYSYM [KEYSYM ...]  press in order, release in reverse
  keydown / keyup --window 0x... KEYSYM
  click    --window 0x... [--button 1] [--hold-ms 60]
  shot     --window 0x... --out file.png      window pixels (XGetImage)
"""

import argparse
import ctypes
import ctypes.util
import sys
import time

# ---------------------------------------------------------------------------
# libX11 / libXtst bindings
# ---------------------------------------------------------------------------

_x11 = ctypes.CDLL(ctypes.util.find_library("X11") or "libX11.so.6")
_xtst = ctypes.CDLL(ctypes.util.find_library("Xtst") or "libXtst.so.6")

Window = ctypes.c_ulong
Atom = ctypes.c_ulong
Time = ctypes.c_ulong
KeySym = ctypes.c_ulong
Bool = ctypes.c_int
Status = ctypes.c_int

CurrentTime = 0
PropModeReplace = 0
ClientMessage = 33
KeyPress = 2
KeyRelease = 3
ButtonPress = 4
ButtonRelease = 5
SubstructureRedirectMask = 1 << 20
SubstructureNotifyMask = 1 << 19
KeyPressMask = 1 << 0
KeyReleaseMask = 1 << 1
ButtonPressMask = 1 << 2
ButtonReleaseMask = 1 << 3
AnyPropertyType = 0
ZPixmap = 2
AllPlanes = 0xFFFFFFFF
ShiftMask = 1 << 0
ControlMask = 1 << 2
Mod1Mask = 1 << 3
Button1Mask = 1 << 8


class XClientMessageData(ctypes.Union):
    _fields_ = [("b", ctypes.c_char * 20), ("s", ctypes.c_short * 10), ("l", ctypes.c_long * 5)]


class XClientMessageEvent(ctypes.Structure):
    _fields_ = [
        ("type", ctypes.c_int),
        ("serial", ctypes.c_ulong),
        ("send_event", Bool),
        ("display", ctypes.c_void_p),
        ("window", Window),
        ("message_type", Atom),
        ("format", ctypes.c_int),
        ("data", XClientMessageData),
    ]


class XKeyEvent(ctypes.Structure):
    _fields_ = [
        ("type", ctypes.c_int),
        ("serial", ctypes.c_ulong),
        ("send_event", Bool),
        ("display", ctypes.c_void_p),
        ("window", Window),
        ("root", Window),
        ("subwindow", Window),
        ("time", Time),
        ("x", ctypes.c_int),
        ("y", ctypes.c_int),
        ("x_root", ctypes.c_int),
        ("y_root", ctypes.c_int),
        ("state", ctypes.c_uint),
        ("keycode", ctypes.c_uint),  # 'button' in XButtonEvent — same slot
        ("same_screen", Bool),
    ]


class XEvent(ctypes.Union):
    _fields_ = [
        ("type", ctypes.c_int),
        ("xclient", XClientMessageEvent),
        ("xkey", XKeyEvent),
        ("pad", ctypes.c_long * 24),
    ]


class XWindowAttributes(ctypes.Structure):
    _fields_ = [
        ("x", ctypes.c_int), ("y", ctypes.c_int),
        ("width", ctypes.c_int), ("height", ctypes.c_int),
        ("border_width", ctypes.c_int), ("depth", ctypes.c_int),
        ("visual", ctypes.c_void_p), ("root", Window),
        ("class", ctypes.c_int), ("bit_gravity", ctypes.c_int),
        ("win_gravity", ctypes.c_int), ("backing_store", ctypes.c_int),
        ("backing_planes", ctypes.c_ulong), ("backing_pixel", ctypes.c_ulong),
        ("save_under", Bool), ("colormap", ctypes.c_ulong),
        ("map_installed", Bool), ("map_state", ctypes.c_int),
        ("all_event_masks", ctypes.c_long), ("your_event_mask", ctypes.c_long),
        ("do_not_propagate_mask", ctypes.c_long), ("override_redirect", Bool),
        ("screen", ctypes.c_void_p),
    ]


class XImage(ctypes.Structure):
    _fields_ = [
        ("width", ctypes.c_int), ("height", ctypes.c_int),
        ("xoffset", ctypes.c_int), ("format", ctypes.c_int),
        ("data", ctypes.POINTER(ctypes.c_ubyte)),
        ("byte_order", ctypes.c_int), ("bitmap_unit", ctypes.c_int),
        ("bitmap_bit_order", ctypes.c_int), ("bitmap_pad", ctypes.c_int),
        ("depth", ctypes.c_int), ("bytes_per_line", ctypes.c_int),
        ("bits_per_pixel", ctypes.c_int),
        ("red_mask", ctypes.c_ulong), ("green_mask", ctypes.c_ulong),
        ("blue_mask", ctypes.c_ulong),
        # obdata + the funcs table follow; never touched.
    ]


def _sig(fn, res, *args):
    fn.restype = res
    fn.argtypes = list(args)
    return fn


XOpenDisplay = _sig(_x11.XOpenDisplay, ctypes.c_void_p, ctypes.c_char_p)
XCloseDisplay = _sig(_x11.XCloseDisplay, ctypes.c_int, ctypes.c_void_p)
XDefaultRootWindow = _sig(_x11.XDefaultRootWindow, Window, ctypes.c_void_p)
XInternAtom = _sig(_x11.XInternAtom, Atom, ctypes.c_void_p, ctypes.c_char_p, Bool)
XFlush = _sig(_x11.XFlush, ctypes.c_int, ctypes.c_void_p)
XSync = _sig(_x11.XSync, ctypes.c_int, ctypes.c_void_p, Bool)
XFree = _sig(_x11.XFree, ctypes.c_int, ctypes.c_void_p)
XSendEvent = _sig(_x11.XSendEvent, Status, ctypes.c_void_p, Window, Bool, ctypes.c_long,
                  ctypes.POINTER(XEvent))
XGetWindowProperty = _sig(
    _x11.XGetWindowProperty, ctypes.c_int, ctypes.c_void_p, Window, Atom, ctypes.c_long,
    ctypes.c_long, Bool, Atom, ctypes.POINTER(Atom), ctypes.POINTER(ctypes.c_int),
    ctypes.POINTER(ctypes.c_ulong), ctypes.POINTER(ctypes.c_ulong),
    ctypes.POINTER(ctypes.POINTER(ctypes.c_ubyte)))
XQueryTree = _sig(_x11.XQueryTree, Status, ctypes.c_void_p, Window, ctypes.POINTER(Window),
                  ctypes.POINTER(Window), ctypes.POINTER(ctypes.POINTER(Window)),
                  ctypes.POINTER(ctypes.c_uint))
XGetWindowAttributes = _sig(_x11.XGetWindowAttributes, Status, ctypes.c_void_p, Window,
                            ctypes.POINTER(XWindowAttributes))
XTranslateCoordinates = _sig(
    _x11.XTranslateCoordinates, Bool, ctypes.c_void_p, Window, Window, ctypes.c_int,
    ctypes.c_int, ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_int),
    ctypes.POINTER(Window))
XStringToKeysym = _sig(_x11.XStringToKeysym, KeySym, ctypes.c_char_p)
XKeysymToKeycode = _sig(_x11.XKeysymToKeycode, ctypes.c_ubyte, ctypes.c_void_p, KeySym)
XGetInputFocus = _sig(_x11.XGetInputFocus, ctypes.c_int, ctypes.c_void_p,
                      ctypes.POINTER(Window), ctypes.POINTER(ctypes.c_int))
XSetInputFocus = _sig(_x11.XSetInputFocus, ctypes.c_int, ctypes.c_void_p, Window, ctypes.c_int,
                      Time)
XRaiseWindow = _sig(_x11.XRaiseWindow, ctypes.c_int, ctypes.c_void_p, Window)
XGetImage = _sig(_x11.XGetImage, ctypes.POINTER(XImage), ctypes.c_void_p, Window, ctypes.c_int,
                 ctypes.c_int, ctypes.c_uint, ctypes.c_uint, ctypes.c_ulong, ctypes.c_int)
XDestroyImage = _sig(_x11.XDestroyImage, ctypes.c_int, ctypes.POINTER(XImage))
XSetErrorHandler = _sig(_x11.XSetErrorHandler, ctypes.c_void_p, ctypes.c_void_p)
XWarpPointer = _sig(_x11.XWarpPointer, ctypes.c_int, ctypes.c_void_p, Window, Window, ctypes.c_int,
                    ctypes.c_int, ctypes.c_uint, ctypes.c_uint, ctypes.c_int, ctypes.c_int)

XTestFakeKeyEvent = _sig(_xtst.XTestFakeKeyEvent, ctypes.c_int, ctypes.c_void_p, ctypes.c_uint,
                         Bool, ctypes.c_ulong)
XTestFakeButtonEvent = _sig(_xtst.XTestFakeButtonEvent, ctypes.c_int, ctypes.c_void_p,
                            ctypes.c_uint, Bool, ctypes.c_ulong)
XTestFakeMotionEvent = _sig(_xtst.XTestFakeMotionEvent, ctypes.c_int, ctypes.c_void_p,
                            ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_ulong)

# A window that vanishes between "find" and "use" (the CTS tears its session
# down per section) raises BadWindow; Xlib's default handler exits the process.
# Swallow it — every caller checks its own result.
_ERRHANDLER = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p)(lambda d, e: 0)
XSetErrorHandler(ctypes.cast(_ERRHANDLER, ctypes.c_void_p))


class X11:
    def __init__(self, display=None):
        self.d = XOpenDisplay(display.encode() if display else None)
        if not self.d:
            raise SystemExit("cts_x11: cannot open X display %r" % (display,))
        self.root = XDefaultRootWindow(self.d)

    def close(self):
        XCloseDisplay(self.d)

    # ---- properties ------------------------------------------------------
    def atom(self, name):
        return XInternAtom(self.d, name.encode(), False)

    def _prop(self, win, name, req_type=AnyPropertyType, length=65536):
        at = Atom()
        fmt = ctypes.c_int()
        nitems = ctypes.c_ulong()
        after = ctypes.c_ulong()
        data = ctypes.POINTER(ctypes.c_ubyte)()
        rc = XGetWindowProperty(self.d, win, self.atom(name), 0, length, False, req_type,
                                ctypes.byref(at), ctypes.byref(fmt), ctypes.byref(nitems),
                                ctypes.byref(after), ctypes.byref(data))
        if rc != 0 or not data:
            return None, 0, 0
        n = nitems.value
        f = fmt.value
        if f == 8:
            out = bytes(data[i] for i in range(n))
        elif f == 32:
            arr = ctypes.cast(data, ctypes.POINTER(ctypes.c_ulong))
            out = [arr[i] for i in range(n)]
        else:
            arr = ctypes.cast(data, ctypes.POINTER(ctypes.c_ushort))
            out = [arr[i] for i in range(n)]
        XFree(data)
        return out, f, n

    def name(self, win):
        v, f, _ = self._prop(win, "_NET_WM_NAME")
        if v is None or f != 8:
            v, f, _ = self._prop(win, "WM_NAME")
        if v is None or f != 8:
            return None
        return v.decode("utf-8", "replace")

    def children(self, win):
        root = Window()
        parent = Window()
        kids = ctypes.POINTER(Window)()
        n = ctypes.c_uint()
        if not XQueryTree(self.d, win, ctypes.byref(root), ctypes.byref(parent), ctypes.byref(kids),
                          ctypes.byref(n)):
            return []
        out = [kids[i] for i in range(n.value)]
        if kids:
            XFree(kids)
        return out

    def find(self, name):
        """All windows whose title is exactly @p name, walking the whole tree
        (the XCB window may be reparented under a WM frame)."""
        hits = []
        stack = [self.root]
        while stack:
            w = stack.pop()
            for c in self.children(w):
                if self.name(c) == name:
                    hits.append(c)
                stack.append(c)
        return hits

    def viewable(self, win):
        a = XWindowAttributes()
        if not XGetWindowAttributes(self.d, win, ctypes.byref(a)):
            return False
        return a.map_state == 2  # IsViewable

    def geom(self, win):
        a = XWindowAttributes()
        if not XGetWindowAttributes(self.d, win, ctypes.byref(a)):
            return None
        x = ctypes.c_int()
        y = ctypes.c_int()
        child = Window()
        XTranslateCoordinates(self.d, win, self.root, 0, 0, ctypes.byref(x), ctypes.byref(y),
                              ctypes.byref(child))
        return x.value, y.value, a.width, a.height

    def focus(self):
        w = Window()
        r = ctypes.c_int()
        XGetInputFocus(self.d, ctypes.byref(w), ctypes.byref(r))
        return w.value

    # ---- focus -------------------------------------------------------------
    def activate(self, win, timeout=2.0):
        """EWMH _NET_ACTIVE_WINDOW with source=2 (pager), which Mutter honours
        without focus-stealing prevention — the xdotool windowactivate recipe.
        Returns True once the X input focus is on @p win (or a descendant)."""
        ev = XEvent()
        ev.xclient.type = ClientMessage
        ev.xclient.send_event = True
        ev.xclient.window = win
        ev.xclient.message_type = self.atom("_NET_ACTIVE_WINDOW")
        ev.xclient.format = 32
        ev.xclient.data.l[0] = 2
        ev.xclient.data.l[1] = CurrentTime
        ev.xclient.data.l[2] = 0
        XSendEvent(self.d, self.root, False, SubstructureRedirectMask | SubstructureNotifyMask,
                   ctypes.byref(ev))
        XFlush(self.d)
        t0 = time.time()
        while time.time() - t0 < timeout:
            if self.focus() == win:
                return True
            time.sleep(0.02)
        # No WM (bare X server / Xvfb): take focus directly.
        XRaiseWindow(self.d, win)
        XSetInputFocus(self.d, win, 1, CurrentTime)  # RevertToPointerRoot
        XSync(self.d, False)
        time.sleep(0.05)
        return self.focus() == win

    # ---- input -------------------------------------------------------------
    def keycode(self, keysym_name):
        ks = XStringToKeysym(keysym_name.encode())
        if ks == 0:
            raise SystemExit("cts_x11: unknown keysym %r" % keysym_name)
        kc = XKeysymToKeycode(self.d, ks)
        if kc == 0:
            raise SystemExit("cts_x11: keysym %r has no keycode in this keymap" % keysym_name)
        return kc

    def _send(self, win, etype, code, state):
        g = self.geom(win) or (0, 0, 1, 1)
        ev = XEvent()
        k = ev.xkey
        k.type = etype
        k.send_event = True
        k.display = self.d
        k.window = win
        k.root = self.root
        k.subwindow = 0
        k.time = CurrentTime
        k.x = g[2] // 2
        k.y = g[3] // 2
        k.x_root = g[0] + k.x
        k.y_root = g[1] + k.y
        k.state = state
        k.keycode = code
        k.same_screen = True
        mask = {KeyPress: KeyPressMask, KeyRelease: KeyReleaseMask,
                ButtonPress: ButtonPressMask, ButtonRelease: ButtonReleaseMask}[etype]
        XSendEvent(self.d, win, True, mask, ctypes.byref(ev))
        XFlush(self.d)

    def key(self, win, keysym_name, press, route="send", state=0):
        kc = self.keycode(keysym_name)
        if route == "send":
            self._send(win, KeyPress if press else KeyRelease, kc, state)
        else:
            XTestFakeKeyEvent(self.d, kc, bool(press), 0)
            XFlush(self.d)

    def button(self, win, button, press, route="send", state=0):
        if route == "send":
            self._send(win, ButtonPress if press else ButtonRelease, button, state)
        else:
            XTestFakeButtonEvent(self.d, button, bool(press), 0)
            XFlush(self.d)

    def park_pointer(self, win, route="send"):
        """Put the pointer over the window centre so an XTEST button lands in
        it. Done BEFORE any modifier goes down (§10.6: never move the mouse
        while CTRL/ALT is held — it translates a controller)."""
        if route == "send":
            return
        g = self.geom(win)
        if g is None:
            return
        XTestFakeMotionEvent(self.d, -1, g[0] + g[2] // 2, g[1] + g[3] // 2, 0)
        XFlush(self.d)

    # ---- capture -------------------------------------------------------------
    def shot(self, win, out):
        from PIL import Image

        g = self.geom(win)
        if g is None:
            return False
        img = XGetImage(self.d, win, 0, 0, g[2], g[3], AllPlanes, ZPixmap)
        if not img:
            return False
        im = img.contents
        if im.bits_per_pixel != 32:
            XDestroyImage(img)
            raise SystemExit("cts_x11: unsupported %d bpp window image" % im.bits_per_pixel)
        size = im.bytes_per_line * im.height
        raw = ctypes.string_at(im.data, size)
        pil = Image.frombuffer("RGBX", (im.width, im.height), raw, "raw", "BGRX",
                               im.bytes_per_line, 1).convert("RGB")
        XDestroyImage(img)
        pil.save(out)
        return True


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def _win(s):
    return int(s, 0)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--display", default=None)
    ap.add_argument("--route", choices=("send", "xtest"), default="send")
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("find")
    p.add_argument("--name", default="DisplayXR")
    p.add_argument("--newest", action="store_true")
    for c in ("geom", "activate"):
        sub.add_parser(c).add_argument("--window", type=_win, required=True)
    for c in ("key", "keydown", "keyup"):
        p = sub.add_parser(c)
        p.add_argument("--window", type=_win, required=True)
        p.add_argument("keysyms", nargs="+")
        p.add_argument("--hold-ms", type=int, default=40)
    p = sub.add_parser("click")
    p.add_argument("--window", type=_win, required=True)
    p.add_argument("--button", type=int, default=1)
    p.add_argument("--hold-ms", type=int, default=60)
    p = sub.add_parser("shot")
    p.add_argument("--window", type=_win, required=True)
    p.add_argument("--out", required=True)
    a = ap.parse_args(argv)

    x = X11(a.display)
    try:
        if a.cmd == "find":
            hits = [w for w in x.find(a.name) if x.viewable(w)]
            if not hits:
                return 1
            if a.newest:
                hits = [max(hits)]
            print(" ".join("0x%08x" % w for w in hits))
        elif a.cmd == "geom":
            g = x.geom(a.window)
            if g is None:
                return 1
            print("%d %d %d %d" % g)
        elif a.cmd == "activate":
            ok = x.activate(a.window)
            print("focused" if ok else "NOT focused (focus=0x%08x)" % x.focus())
            return 0 if ok else 1
        elif a.cmd in ("key", "keydown", "keyup"):
            if a.cmd in ("key", "keydown"):
                for k in a.keysyms:
                    x.key(a.window, k, True, a.route)
            if a.cmd == "key":
                time.sleep(a.hold_ms / 1000.0)
            if a.cmd in ("key", "keyup"):
                for k in reversed(a.keysyms):
                    x.key(a.window, k, False, a.route)
            print("ok")
        elif a.cmd == "click":
            x.park_pointer(a.window, a.route)
            time.sleep(0.03)
            x.button(a.window, a.button, True, a.route)
            time.sleep(a.hold_ms / 1000.0)
            x.button(a.window, a.button, False, a.route)
            print("ok")
        elif a.cmd == "shot":
            if not x.shot(a.window, a.out):
                return 1
            print(a.out)
    finally:
        x.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
