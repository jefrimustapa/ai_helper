"""Redraw regression (COH-517): resize/drag the Settings window and switch tabs; the final
rendering at the original size must equal the initial rendering (no lost edges/ghost controls).
Usage: python tests/test_resize_redraw.py <exe> [shot-prefix]"""
import ctypes, os, sys, time, subprocess, tempfile, socket, win32gui, win32con, win32process
import win32ui
from PIL import Image, ImageChops
from pywinauto import Application
EXE = os.path.abspath(sys.argv[1]); SHOT = sys.argv[2] if len(sys.argv) > 2 else None
PORT = 5093; tmp = tempfile.mkdtemp(prefix="aih-rs-"); INI = os.path.join(tmp, "t.ini")
open(INI, "w").write("[Settings]\nInitialized=1\n[Apps]\nApp0=C:\Windows\explorer.exe\n[Bridge]\nEnabled=0\nToken=00112233445566778899aabbccddeeff\nRoot0=" + tmp + "\n")
p = subprocess.Popen([EXE, f"/port:{PORT}", "/ini:" + INI], env=dict(os.environ, AIHELPER_TEST_NOLAUNCH="1"))
try:
    h = None
    for _ in range(80):
        def cb(hw, _):
            global h
            if win32process.GetWindowThreadProcessId(hw)[1] == p.pid and win32gui.GetClassName(hw) == "AiHelperWindow": h = hw
        win32gui.EnumWindows(cb, None)
        if h: break
        time.sleep(0.1)
    win32gui.ShowWindow(h, win32con.SW_SHOW); time.sleep(0.6)
    w = Application(backend="win32").connect(handle=h).window(handle=h)
    tab = w.child_window(class_name="SysTabControl32")
    x, y, r, b = win32gui.GetWindowRect(h); W, H = r - x, b - y
    def shot():   # PrintWindow: renders the window itself, independent of what overlaps it on screen
        time.sleep(0.4)
        l, t, r_, b_ = win32gui.GetWindowRect(h); w_, h_ = r_ - l, b_ - t
        dc = win32gui.GetWindowDC(h); mdc = win32ui.CreateDCFromHandle(dc); mem = mdc.CreateCompatibleDC()
        bmp = win32ui.CreateBitmap(); bmp.CreateCompatibleBitmap(mdc, w_, h_); mem.SelectObject(bmp)
        ctypes.windll.user32.PrintWindow(h, mem.GetSafeHdc(), 2)
        info = bmp.GetInfo(); bits = bmp.GetBitmapBits(True)
        img = Image.frombuffer("RGB", (info["bmWidth"], info["bmHeight"]), bits, "raw", "BGRX", 0, 1)
        mem.DeleteDC(); mdc.DeleteDC(); win32gui.ReleaseDC(h, dc); win32gui.DeleteObject(bmp.GetHandle())
        return img
    fails = []
    for name in ("General", "Bridge"):
        tab.select(name); base = shot()
        for i in range(25):   # emulate drag-resize in small steps, both axes
            win32gui.SetWindowPos(h, 0, x, y, W + (i * 9) % 260, H + (i * 7) % 200, win32con.SWP_NOZORDER); time.sleep(0.02)
        tab.select("Bridge" if name == "General" else "General"); time.sleep(0.1); tab.select(name)
        win32gui.SetWindowPos(h, 0, x, y, W, H, win32con.SWP_NOZORDER)
        after = shot()
        diff = ImageChops.difference(base, after).getbbox()
        print(("PASS " if diff is None else "FAIL ") + f"tab {name}: identical after drag-resize + tab switches", diff or "")
        if diff: fails.append(name); after.save((SHOT or os.path.join(tmp, "x")) + f"-{name}-after.png"); base.save((SHOT or os.path.join(tmp, "x")) + f"-{name}-base.png")
    print("ALL PASSED" if not fails else "FAILED")
    sys.exit(1 if fails else 0)
finally:
    p.kill()
