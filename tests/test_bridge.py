"""Tests for ai-helper HTTP bridge (GET /open). Usage: python tests/test_bridge.py <exe> [port]
Throw-away instance: own ini, random token, no registry writes, Explorer launch stubbed
(AIHELPER_TEST_NOLAUNCH=1 -> reply contains <!--would-launch-->)."""
import os, sys, socket, subprocess, tempfile, time, random, secrets, shutil, urllib.parse as up

EXE = os.path.abspath(sys.argv[1])
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 5071
tmp = tempfile.mkdtemp(prefix="aih-test-")
root = os.path.join(tmp, "root")
outside = os.path.join(tmp, "outside")
sib = os.path.join(tmp, "root_x")
for d in (root, outside, sib, os.path.join(root, "sub")):
    os.makedirs(d)
for f in (os.path.join(root, "sub", "f.txt"), os.path.join(outside, "s.txt"), os.path.join(sib, "s.txt")):
    open(f, "w").write("x")
subprocess.run(["cmd", "/c", "mklink", "/J", os.path.join(root, "jct"), outside], capture_output=True)
TOKEN = secrets.token_hex(16)


def write_ini(enabled=1, token=TOKEN, roots=None):
    ini = os.path.join(tmp, "t.ini")
    roots = [root] if roots is None else roots
    lines = ["[Settings]", "Initialized=1", "[Apps]", r"App0=C:\Windows\explorer.exe",
             r"App1=C:\Windows\System32\hostname.exe", "[Bridge]"]
    if enabled is not None:
        lines.append(f"Enabled={enabled}")
    if token:
        lines.append(f"Token={token}")
    lines += [f"Root{i}={r}" for i, r in enumerate(roots)]
    open(ini, "w").write("\n".join(lines) + "\n")
    return ini


proc = None


def stop():
    global proc
    if proc:
        proc.kill()
        proc.wait()
        proc = None


def start(**kw):
    global proc
    stop()
    ini = write_ini(**kw)
    env = dict(os.environ, AIHELPER_TEST_NOLAUNCH="1")
    proc = subprocess.Popen([EXE, f"/port:{PORT}", f"/ini:{ini}"], env=env)
    for _ in range(50):
        try:
            socket.create_connection(("127.0.0.1", PORT), 0.2).close()
            return ini
        except OSError:
            time.sleep(0.1)
    raise SystemExit("helper did not start")


def raw(data, timeout=3):
    s = socket.create_connection(("127.0.0.1", PORT), timeout)
    s.sendall(data)
    out = b""
    try:
        while True:
            c = s.recv(4096)
            if not c:
                break
            out += c
    except OSError:
        pass
    s.close()
    return out


def status(resp):
    return int(resp.split(b" ", 2)[1]) if resp.startswith(b"HTTP/") else None


GOOD = {"Host": f"127.0.0.1:{PORT}", "Sec-Fetch-Site": "same-site", "Sec-Fetch-Mode": "navigate",
        "Sec-Fetch-Dest": "document", "Sec-Fetch-User": "?1"}


def http(path=None, token=TOKEN, hdr=None, target="/open", method="GET", raw_query=None):
    h = dict(GOOD)
    h.update(hdr or {})
    h = {k: v for k, v in h.items() if v is not None}
    if raw_query is None:
        q = {}
        if path is not None:
            q["path"] = path
        if token is not None:
            q["token"] = token
        raw_query = up.urlencode(q, quote_via=up.quote)
    t = target + ("?" + raw_query if raw_query else "")
    req = f"{method} {t} HTTP/1.1\r\n" + "".join(f"{k}: {v}\r\n" for k, v in h.items()) + "\r\n"
    return raw(req.encode("latin-1"))


fails = []
n = 0


def check(name, cond, info=""):
    global n
    n += 1
    print(("PASS " if cond else "FAIL ") + name + ("" if cond else f"  [{info}]"))
    if not cond:
        fails.append(name)


def expect(name, resp, code):
    s = status(resp)
    check(name, s == code, f"got {s} {resp[:80]!r}")
    if code != 200:
        check(name + " (nothing launched)", b"would-launch" not in resp)


def enc(p):
    return up.quote(p, safe="")


try:
    start()
    # ---- positives
    r = http(root)
    check("folder -> 200 + window.close", status(r) == 200 and b"window.close()" in r and b"would-launch" in r, r[:200])
    check("folder -> Explorer opens it (no /select)", b"would-launch:open" in r, r[:200])
    check("200 is text/html", b"Content-Type: text/html" in r)
    rf = http(os.path.join(root, "sub", "f.txt"))
    expect("file -> 200", rf, 200)
    check("file -> /select (highlight, never execute)", b"would-launch:select" in rf, rf[:200])
    expect("subfolder with forward slashes", http(os.path.join(root, "sub").replace("\\", "/")), 200)
    expect("root + trailing backslash", http(root + "\\"), 200)
    expect("same-origin allowed", http(root, hdr={"Sec-Fetch-Site": "same-origin"}), 200)
    expect("cross-site + user navigation allowed (click from Paperclip)", http(root, hdr={"Sec-Fetch-Site": "cross-site"}), 200)
    r = http(target="/ping")
    check("/ping -> ai-helper", status(r) == 200 and r.endswith(b"ai-helper"))
    # ---- negatives: token
    expect("wrong token", http(root, token="0" * 32), 403)
    expect("empty token", http(root, token=""), 403)
    expect("missing token", http(root, token=None), 403)
    expect("token 1 char short", http(root, token=TOKEN[:-1]), 403)
    expect("token + extra char", http(root, token=TOKEN + "0"), 403)
    # ---- negatives: path
    expect("path outside root", http(outside), 403)
    expect("sibling prefix dir (root_x)", http(sib), 403)
    expect("path with ..", http(root + "\\sub\\..\\..\\outside"), 400)
    expect(".. escaping to outside", http(root + "\\..\\outside"), 400)
    expect("encoded %2e%2e", http(None, raw_query=f"token={TOKEN}&path=" + enc(root) + "%5C%2e%2e%5Coutside"), 400)
    expect("junction pointing outside", http(os.path.join(root, "jct", "s.txt")), 403)
    expect("relative path", http("sub\\f.txt"), 400)
    expect("UNC path", http("\\\\127.0.0.1\\c$"), 400)
    expect("wildcard", http(root + "\\*"), 400)
    expect("control char", http(root + "\\a\x01b"), 400)
    expect("embedded NUL", http(None, raw_query=f"token={TOKEN}&path=" + enc(root) + "%00x"), 400)
    expect("nonexistent inside root", http(root + "\\nope"), 404)
    expect("missing path", http(None), 400)
    expect("empty path", http(""), 400)
    expect("bad percent escape", http(None, raw_query=f"token={TOKEN}&path=%ZZ"), 400)
    expect("duplicate path param", http(None, raw_query=f"token={TOKEN}&path=" + enc(root) + "&path=" + enc(outside)), 400)
    expect("path too long", http("D:\\" + "a" * 600), 400)
    # ---- negatives: headers
    expect("no Sec-Fetch-User", http(root, hdr={"Sec-Fetch-User": None}), 403)
    expect("Sec-Fetch-User ?0", http(root, hdr={"Sec-Fetch-User": "?0"}), 403)
    expect("no Sec-Fetch-Mode", http(root, hdr={"Sec-Fetch-Mode": None}), 403)
    expect("Mode no-cors", http(root, hdr={"Sec-Fetch-Mode": "no-cors"}), 403)
    expect("Dest image", http(root, hdr={"Sec-Fetch-Dest": "image"}), 403)
    expect("no Sec-Fetch-Dest", http(root, hdr={"Sec-Fetch-Dest": None}), 403)
    X = {"Sec-Fetch-Site": "cross-site"}
    expect("cross-site without Sec-Fetch-User", http(root, hdr=dict(X, **{"Sec-Fetch-User": None})), 403)
    expect("cross-site Sec-Fetch-User ?0", http(root, hdr=dict(X, **{"Sec-Fetch-User": "?0"})), 403)
    expect("cross-site Mode cors (fetch)", http(root, hdr=dict(X, **{"Sec-Fetch-Mode": "cors"})), 403)
    expect("cross-site Dest image", http(root, hdr=dict(X, **{"Sec-Fetch-Dest": "image"})), 403)
    expect("cross-site Dest script", http(root, hdr=dict(X, **{"Sec-Fetch-Dest": "script"})), 403)
    expect("cross-site wrong token", http(root, token="x" * len(TOKEN), hdr=X), 403)
    expect("cross-site no token", http(root, token=None, hdr=X), 403)
    expect("cross-site path outside root", http("C:\\Windows", hdr=X), 403)
    expect("Site unknown value", http(root, hdr={"Sec-Fetch-Site": "cross-origin"}), 403)
    expect("Site none (typed URL)", http(root, hdr={"Sec-Fetch-Site": "none"}), 403)
    expect("no Sec-Fetch-Site", http(root, hdr={"Sec-Fetch-Site": None}), 403)
    expect("Host localhost", http(root, hdr={"Host": f"localhost:{PORT}"}), 403)
    expect("Host evil.com", http(root, hdr={"Host": f"evil.com:{PORT}"}), 403)
    expect("Host wrong port", http(root, hdr={"Host": "127.0.0.1:1"}), 403)
    expect("Host missing", http(root, hdr={"Host": None}), 403)
    expect("Host 127.0.0.1 no port", http(root, hdr={"Host": "127.0.0.1"}), 403)
    expect("/ping bad Host", http(target="/ping", hdr={"Host": "evil.com"}), 403)
    tail = ("Sec-Fetch-Mode: navigate\r\nSec-Fetch-Dest: document\r\nSec-Fetch-User: ?1\r\n\r\n")
    base = f"GET /open?token={TOKEN}&path={enc(root)} HTTP/1.1\r\n"
    expect("duplicate Host header", raw((base + f"Host: 127.0.0.1:{PORT}\r\nHost: 127.0.0.1:{PORT}\r\nSec-Fetch-Site: same-site\r\n" + tail).encode()), 403)
    expect("duplicate Sec-Fetch-Site", raw((base + f"Host: 127.0.0.1:{PORT}\r\nSec-Fetch-Site: same-site\r\nSec-Fetch-Site: cross-site\r\n" + tail).encode()), 403)
    # ---- negatives: request shape
    r = http(root, method="POST")
    check("POST rejected (not 200, nothing launched)", status(r) != 200 and b"would-launch" not in r, r[:80])
    expect("unknown path /x", http(root, target="/x"), 404)
    expect("/favicon.ico", http(target="/favicon.ico"), 404)
    expect("/open/ (trailing slash)", http(root, target="/open/"), 404)
    expect("bad HTTP version", raw(f"GET /open?token={TOKEN}&path={enc(root)} FTP/9\r\nHost: 127.0.0.1:{PORT}\r\n\r\n".encode()), 400)
    r = raw(b"GET /open?a=" + b"b" * 9000 + b" HTTP/1.1\r\n\r\n")
    check("oversized request rejected", status(r) in (414, 431, None), r[:60])
    r = raw(b"GET /open HTTP/1.1\r\nHost: x\r\n")
    check("incomplete headers -> 431/closed (no hang)", status(r) in (431, None), r[:60])
    # ---- legacy protocol unchanged
    r = raw(b"hostname\r\n")
    check("legacy: allowlisted app -> OK", r.strip() == b"OK", r)
    r = raw(b"hostname\n")
    check("legacy: LF only -> OK", r.strip() == b"OK", r)
    r = raw(b"not_listed C:\\x\r\n")
    check("legacy: unknown app -> ERR application not allowed", r.strip() == b"ERR application not allowed", r)
    r = raw(b"\r\n")
    check("legacy: empty line -> ERR", r.startswith(b"ERR"), r)
    r = raw(b"get /open\r\n")
    check("legacy: lower-case 'get' is old protocol", r.startswith(b"ERR"), r)
    r = raw(b"GETX\r\n")
    check("legacy: 'GETX' (no space) is old protocol", r.startswith(b"ERR"), r)
    # ---- property-based / fuzz: messy paths must never launch outside root and never crash
    rnd = random.Random(1234)
    bad = 0
    rroot = os.path.realpath(root).lower()
    for i in range(300):
        k = i % 4
        if k == 0:
            p = "".join(chr(rnd.choice([rnd.randint(0, 0x7f), rnd.randint(0x80, 0x2ff)])) for _ in range(rnd.randint(0, 60)))
        elif k == 1:
            p = rnd.choice([outside, sib, "C:\\Windows", "D:\\", "\\\\?\\C:\\", "C:/Windows/../Windows"]) + "".join(rnd.choice("\\/.. ") for _ in range(rnd.randint(0, 6)))
        elif k == 2:
            p = root + "".join(rnd.choice("\\/.:~$ ") + rnd.choice(["", "..", "sub", "jct"]) for _ in range(rnd.randint(1, 5)))
        else:
            p = root + "\\sub" + "\\.." * rnd.randint(1, 4) + "\\" + rnd.choice(["outside", "x", ""])
        r = http(p)
        s = status(r)
        if s is None:
            check("fuzz: helper dropped connection", False, repr(p))
            break
        if s == 200:
            rp = os.path.realpath(p).lower()
            inside = rp == rroot or rp.startswith(rroot + "\\")
            if ".." in p.replace("/", "\\").split("\\") or not inside:
                bad += 1
                print("  fuzz unexpected 200:", repr(p))
    check("fuzz 300 paths: no 200 for '..'/outside-root", bad == 0)
    check("fuzz: helper alive", proc.poll() is None and status(http(target="/ping")) == 200)
    for i in range(100):
        blob = bytes(rnd.randrange(256) for _ in range(rnd.randint(1, 400)))
        blob = blob.replace(b" ", b"_").replace(b"\r", b"").replace(b"\n", b"")
        raw(b"GET /open?" + blob + b" HTTP/1.1\r\n\r\n", 1)
    check("fuzz: 100 random-byte requests, helper alive", proc.poll() is None and status(http(target="/ping")) == 200)
    # ---- config states
    start(enabled=0)
    r = http(root)
    check("Enabled=0 -> 403 bridge disabled", status(r) == 403 and b"bridge disabled" in r)
    check("Enabled=0: /ping also 403", status(http(target="/ping")) == 403)
    r = raw(b"hostname\r\n")
    check("Enabled=0: legacy protocol still OK", r.strip() == b"OK", r)
    start(enabled=2)
    check("Enabled=2 (only 1 turns on) -> off", status(http(root)) == 403)
    start(enabled=None)
    check("Enabled missing -> off (default OFF)", status(http(root)) == 403)
    start(roots=[])
    check("no roots -> 403", status(http(root)) == 403)
    start(roots=[root, outside])
    check("two roots: second root allowed", status(http(outside + "\\s.txt")) == 200)
    start(roots=[root + "\\"])
    check("root with trailing backslash", status(http(root + "\\sub")) == 200)
    ini0 = start(token=None, enabled=None, roots=[])
    itxt = open(ini0).read()
    check("first run: default Root0 written", r"Root0=D:\ai_project\paperclip_setup" in itxt, itxt)
    check("first run: Enabled=0 written", "Enabled=0" in itxt, itxt)
    ini = start(token=None)
    gen = [l for l in open(ini).read().splitlines() if l.startswith("Token=")]
    check("no Token -> 32-hex token generated", len(gen) == 1 and len(gen[0]) == 6 + 32)
    check("generated token works", bool(gen) and status(http(root, token=gen[0][6:])) == 200)
    t1 = gen[0]
    start(token=None)
    gen2 = [l for l in open(os.path.join(tmp, "t.ini")).read().splitlines() if l.startswith("Token=")]
    check("regenerate: deleting Token line -> different token", len(gen2) == 1 and gen2[0] != t1)
    start(token="short")
    txt = open(os.path.join(tmp, "t.ini")).read()
    check("invalid Token -> regenerated, old value refused", "Token=short" not in txt and status(http(root, token="short")) == 403)
finally:
    stop()
    shutil.rmtree(tmp, ignore_errors=True)
print(f"\n{n - len(fails)}/{n} passed")
sys.exit(1 if fails else 0)
