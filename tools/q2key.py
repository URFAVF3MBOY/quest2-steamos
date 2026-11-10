#!/usr/bin/env python3
# q2key - press keys in a game running in VR, from SSH (no keyboard on the headset needed).
# Steam's games run in gamescope's own Xwayland servers (:1, :2, ...); this sends X test-extension key presses to the
# display whose active window matches (default: the first display that has a steam_app_* window).
# Usage (as root on the headset): q2key.py KEY [KEY ...] [--display :2] [--hold SECONDS]
#   KEY = an X keysym name: z, x, Return, Escape, Up, Down, Left, Right, space, shift ... ; "a+b" presses together
import ctypes, os, subprocess, sys, time

args = sys.argv[1:]
disp = None; hold = 0.06
if '--display' in args: i = args.index('--display'); disp = args[i + 1]; del args[i:i + 2]
if '--hold' in args: i = args.index('--hold'); hold = float(args[i + 1]); del args[i:i + 2]
if not args: sys.exit(__doc__ if __doc__ else 'usage: q2key.py KEY [KEY ...] [--display :N] [--hold S]')

if os.geteuid() == 0:   # run as the user owning the X servers
    sys.exit(subprocess.call(['runuser', '-u', 'steamos', '--', sys.executable, os.path.abspath(__file__)] + sys.argv[1:]))

x11 = ctypes.CDLL('libX11.so.6'); xtst = ctypes.CDLL('libXtst.so.6')
x11.XOpenDisplay.restype = ctypes.c_void_p; x11.XOpenDisplay.argtypes = [ctypes.c_char_p]
x11.XStringToKeysym.restype = ctypes.c_ulong; x11.XStringToKeysym.argtypes = [ctypes.c_char_p]
x11.XKeysymToKeycode.restype = ctypes.c_ubyte; x11.XKeysymToKeycode.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
x11.XFlush.argtypes = [ctypes.c_void_p]; x11.XCloseDisplay.argtypes = [ctypes.c_void_p]
xtst.XTestFakeKeyEvent.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_int, ctypes.c_ulong]

def game_display():
    for n in range(1, 6):
        try:
            out = subprocess.run(['xprop', '-display', ':%d' % n, '-root', '_NET_ACTIVE_WINDOW'], capture_output=True, text=True, timeout=2).stdout
            w = out.split()[-1] if out.strip() else ''
            if w.startswith('0x'):
                cls = subprocess.run(['xprop', '-display', ':%d' % n, '-id', w, 'WM_CLASS'], capture_output=True, text=True, timeout=2).stdout
                if 'steam_app_' in cls: return ':%d' % n
        except Exception:
            pass
    return None

disp = disp or game_display()
if not disp: sys.exit('no display with a game window found (use --display :N)')
d = x11.XOpenDisplay(disp.encode())
if not d: sys.exit('cannot open display %s' % disp)
for key in args:
    codes = []
    for name in key.split('+'):
        kc = x11.XKeysymToKeycode(d, x11.XStringToKeysym(name.encode()))
        if not kc: sys.exit('unknown key %r' % name)
        codes.append(kc)
    for kc in codes: xtst.XTestFakeKeyEvent(d, kc, 1, 0)
    x11.XFlush(d); time.sleep(hold)
    for kc in reversed(codes): xtst.XTestFakeKeyEvent(d, kc, 0, 0)
    x11.XFlush(d); time.sleep(0.05)
    print('%s: pressed %s' % (disp, key))
x11.XCloseDisplay(d)
