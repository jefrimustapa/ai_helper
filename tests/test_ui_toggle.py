"""UI tests for the 'Enable HTTP bridge' checkbox (COH-491). Usage: python tests/test_ui_toggle.py <exe> [port] [shot.png]
Drives a throw-away instance (own ini via /ini, /port; no registry writes) with pywinauto (win32 backend).
Covers: init state, check+Save -> ini + live /open without restart, uncheck+Save, empty token
(AIHELPER_TEST_NORNG=1), Token/Root untouched, BVA (no [Bridge] section, Enabled=2), many toggles and a
seeded random property test."""
import os, sys, socket, subprocess, tempfile, time, random, secrets, configparser, urllib.request, urllib.error
import win32gui, win32process, win32con
from pywinauto import Application

EXE = os.path.abspath(sys.argv[1])
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 5073
SHOT = sys.argv[3] if len(sys.argv) > 3 else None
tmp = tempfile.mkdtemp(prefix="aih-ui-")
root = os.path.join(tmp, "root")
os.makedirs(root)
TOKEN = secrets.token_hex(16)
INI = os.path.join(tmp, "t.ini")
fails = []


def check(name, cond, info=""):
    print(("PASS " if cond else "FAIL ") + name + ("" if cond else "  " + str(info)), flush=True)
    if not cond:
        fails.append(name)


def write_ini(enabled="0", token=TOKEN, section=True):
    lines = ["[Settings]", "Initialized=1", "[Apps]", r"App0=C:\Windows\explorer.exe"]
    if section:
        lines.append("[Bridge]")
        if enabled is not None:
            lines.append(f"Enabled={enabled}")
        if token:
            lines.append(f"Token={token}")
        lines.append(f"Root0={root}")
    open(INI, "w").write("\n".join(lines) + "\n")


def ini_get(key):
    cp = configparser.ConfigParser(interpolation=None)
    cp.read(INI)
    return cp.get("Bridge", key, fallback=None)


def open_status():
    url = f"http://127.0.0.1:{PORT}/open?token={ini_get('token') or TOKEN}&path={urllib.request.quote(root)}"
    req = urllib.request.Request(url, headers={
        "Sec-Fetch-Site": "same-site", "Sec-Fetch-Mode": "navigate",
        "Sec-Fetch-Dest": "document", "Sec-Fetch-User": "?1"})
    try:
        return urllib.request.urlopen(req, timeout=3).status
    except urllib.error.HTTPError as e:
        return e.code


def open_body():
    url = f"http://127.0.0.1:{PORT}/open?token={ini_get('token')}&path={urllib.request.quote(root)}"
    req = urllib.request.Request(url, headers={
        "Sec-Fetch-Site": "same-site", "Sec-Fetch-Mode": "navigate",
        "Sec-Fetch-Dest": "document", "Sec-Fetch-User": "?1"})
    try:
        return urllib.request.urlopen(req, timeout=3).read()
    except urllib.error.HTTPError as e:
        return e.read()


class Inst:
    def __init__(self, norng=False):
        env = dict(os.environ, AIHELPER_TEST_NOLAUNCH="1")
        if norng:
            env["AIHELPER_TEST_NORNG"] = "1"
        self.p = subprocess.Popen([EXE, f"/port:{PORT}", f"/ini:{INI}"], env=env)
        for _ in range(60):
            try:
                socket.create_connection(("127.0.0.1", PORT), 0.2).close()
                break
            except OSError:
                time.sleep(0.1)
        else:
            raise SystemExit("helper did not start")
        self.h = None
        for _ in range(50):
            def cb(h, _):
                if win32process.GetWindowThreadProcessId(h)[1] == self.p.pid and \
                        win32gui.GetClassName(h) == "AiHelperWindow":
                    self.h = h
            win32gui.EnumWindows(cb, None)
            if self.h:
                break
            time.sleep(0.1)
        assert self.h, "settings window not found"
        win32gui.ShowWindow(self.h, win32con.SW_SHOW)
        time.sleep(0.3)
        self.w = Application(backend="win32").connect(handle=self.h).window(handle=self.h)
        self.tab = self.w.child_window(class_name="SysTabControl32")
        self.tab.select("Bridge")   # COH-517: bridge checkbox lives on the Bridge tab
        time.sleep(0.2)
        self.chk = self.w.child_window(title_re="Enable HTTP bridge.*", class_name="Button")
        self.save = self.w.child_window(title="Save", class_name="Button")

    def checked(self):
        return self.chk.get_check_state() == 1

    def set_checked(self, v):
        for _ in range(3):
            if self.checked() == v:
                return
            self.chk.click()
            time.sleep(0.15)

    def do_save(self):
        self.save.click()
        time.sleep(0.5)

    def status_text(self):
        return win32gui.GetWindowText(win32gui.GetDlgItem(self.h, 106))  # ID_STATUS

    def close(self):
        win32gui.PostMessage(self.h, win32con.WM_COMMAND, 201, 0)   # ID_TRAY_EXIT: clean exit (flushes gcov data)
        try:
            self.p.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.p.kill()
            self.p.wait()


def run(name, fn, **kw):
    i = Inst(**kw)
    try:
        fn(i)
    finally:
        i.close()
        time.sleep(0.3)


def t_basic(i):
    check("(i) Enabled=0 -> checkbox unchecked", not i.checked())
    check("(i) Enabled=0 -> /open 403", open_status() == 403)
    if SHOT:
        try:
            i.w.capture_as_image().save(SHOT)
        except Exception as e:
            print("screenshot failed:", e)
    i.set_checked(True)
    check("click alone does not enable (needs Save)", open_status() == 403)
    i.do_save()
    check("(ii) check+Save -> ini Enabled=1", ini_get("enabled") == "1", ini_get("enabled"))
    check("(ii) /open 200 without restart", open_status() == 200)
    check("(v) Token unchanged", ini_get("token") == TOKEN)
    check("(v) Root0 unchanged", ini_get("root0") == root)
    i.set_checked(False)
    i.do_save()
    check("(iii) uncheck+Save -> ini Enabled=0", ini_get("enabled") == "0", ini_get("enabled"))
    check("(iii) /open 403 again", open_status() == 403)
    check("(v) Token still unchanged", ini_get("token") == TOKEN)


def t_many(i):
    ok = True
    for n in range(10):  # Many
        want = n % 2 == 0
        i.set_checked(want)
        i.do_save()
        ok &= (open_status() == (200 if want else 403)) and ini_get("enabled") == ("1" if want else "0")
    check("toggle 10x stays consistent", ok)


def t_prop(i):
    rnd = random.Random(20261112)
    saved = False  # persisted/applied state
    ui = False
    bad = []
    for step in range(200):
        if rnd.random() < 0.6:
            ui = rnd.random() < 0.5
            i.set_checked(ui)
        else:
            i.do_save()
            saved = ui
        exp = 200 if saved else 403
        got = open_status()
        if got != exp or ini_get("token") != TOKEN or ini_get("root0") != root:
            bad.append((step, exp, got))
            break
    check("property: 200 random toggle/Save steps (seed fixed), /open <=> saved ON; Token/Root unchanged", not bad, bad)


def t_norng(i):
    check("(iv) no token -> checkbox disabled", not i.chk.is_enabled())
    check("(iv) no token -> unchecked", not i.checked())
    check("(iv) /open stays closed", open_status() == 403)
    time.sleep(0.5)
    check("(iv) status mentions no token", "no token" in i.status_text(), i.status_text())
    win32gui.SendMessage(i.chk.handle, win32con.BM_SETCHECK, 1, 0)   # force-check the disabled box
    i.do_save()
    check("(iv) forced ON + Save with no token stays closed", open_status() == 403)
    check("(iv) box unchecked again after refused enable", not i.checked())
    check("(iv) Save keeps bridge closed", open_status() == 403)
    check("(iv) ini Enabled=0 after Save", ini_get("enabled") == "0", ini_get("enabled"))


def t_noseg(i):
    check("BVA Zero: no [Bridge] section -> generated, unchecked", not i.checked())
    i.set_checked(True)
    i.do_save()
    # generated default Root0 is not our temp root, so expect "outside allowed roots" (403), not "bridge disabled"
    body = open_body()
    check("BVA Zero: enable works (no longer 'bridge disabled')", ini_get("enabled") == "1" and b"bridge disabled" not in body, body)


def t_two(i):
    check("BVA Enabled=2 -> unchecked / 403", not i.checked() and open_status() == 403)
    i.do_save()
    check("BVA Enabled=2 -> Save normalises to 0", ini_get("enabled") == "0", ini_get("enabled"))


def t_on_start(i):
    check("Enabled=1 at start -> checked", i.checked())
    check("Enabled=1 at start -> /open 200", open_status() == 200)


write_ini("0")
run("basic", t_basic)
write_ini("0"); run("many", t_many)
write_ini("0"); run("prop", t_prop)
write_ini("1"); run("on_start", t_on_start)
write_ini(None, section=False); run("noseg", t_noseg)
write_ini("2"); run("two", t_two)
write_ini("1", token=None); run("norng", t_norng, norng=True)
print(f"\n{'ALL PASSED' if not fails else 'FAILED: ' + str(fails)}")
sys.exit(1 if fails else 0)
