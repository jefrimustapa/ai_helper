"""Mutation test: break one bridge check at a time, rebuild to a temp exe, run test_bridge.py.
Every mutant must make the tests FAIL (killed). Usage: python tests/mutate.py [port] [lo] [hi]  (batch = mutants lo..hi-1)"""
import os, sys, subprocess, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(os.path.dirname(HERE), "ai-helper.c")
PORT = sys.argv[1] if len(sys.argv) > 1 else "5072"
LO = int(sys.argv[2]) if len(sys.argv) > 2 else 0      # batch: mutants [LO, HI)
HI = int(sys.argv[3]) if len(sys.argv) > 3 else None
src = open(SRC, encoding="utf-8").read()

M = [
    ("host check removed", 'strcmp(val, want) != 0) {', '0) {'),
    ("host: accept any", 'hdr_get(req, "Host", val, sizeof val) != 1 ||', 'hdr_get(req, "Host", val, sizeof val) < 0 ||'),
    ("Sec-Fetch-User not required", 'strcmp(val, "?1") != 0 ||', '0 ||'),
    ("Sec-Fetch-Mode not required", 'strcmp(val, "navigate") != 0 ||', '0 ||'),
    ("Sec-Fetch-Dest not required", 'strcmp(val, "document") != 0) {', '0) {'),
    ("cross-site allowed", '(strcmp(val, "same-site") != 0 && strcmp(val, "same-origin") != 0)', '0'),
    ("token check removed", '!ct_equal(tok, token)', '0'),
    ("ct_equal ignores length", 'unsigned d = (unsigned)(la ^ lb);', 'unsigned d = 0;'),
    ("bridge always on", 'int on = g_bridgeOn;', 'int on = 1;'),
    ("Enabled>=1 turns on", 'L"Enabled", 0, g_ini) == 1', 'L"Enabled", 0, g_ini) >= 1'),
    ("root check removed", 'if (!allowed) return "outside allowed roots";', ''),
    ("root boundary not enforced", 'return path[rl] == 0 || path[rl] == L\'\\\\\';', 'return 1;'),
    ("'..' not rejected", 'if (len == 2 && seg[0] == L\'.\' && seg[1] == L\'.\') return "bad path";', ''),
    ("relative path allowed", 'w[1] != L\':\' || w[2] != L\'\\\\\') return "bad path";', '0) return "bad path";'),
    ("canonicalisation skipped (junction)", 'if (!canon_path(w, ok, MAX_PATH)) return "not found";', 'if (!canon_path(w, ok, MAX_PATH)) return "not found"; wcscpy(ok, w);'),
    ("wildcards allowed", 'wcschr(L"*?<>|\\"", *p)', '0'),
    ("duplicate query key allowed", 'if (found != -1) return -2;', ''),
    ("default Root0 changed", 'D:\\\\ai_project\\\\paperclip_setup', 'C:\\\\'),
    ("window.close removed", 'window.close()', 'void(0)'),
    ("legacy: GET detection loosened", 'strncmp(buf, "GET ", 4) == 0) {', 'strncmp(buf, "GET", 3) == 0) {'),
    ("file launched directly (no /select)", 'L"/select,\\"%ls\\""', 'L"\\"%ls\\""'),
]

res = []
t0 = time.time()
tmp = tempfile.mkdtemp(prefix="aih-mut-")
for i, (name, old, new) in enumerate(M):
    if i < LO or (HI is not None and i >= HI):
        continue
    if src.count(old) != 1:
        res.append((name, "MUTATION-NOT-APPLIED (pattern count %d)" % src.count(old)))
        print(res[-1], flush=True)
        continue
    c = os.path.join(tmp, "m.c")
    exe = os.path.join(tmp, f"m{i}.exe")
    open(c, "w", encoding="utf-8").write(src.replace(old, new))
    b = subprocess.run(["gcc", "-O2", "-municode", "-mwindows", c, "-o", exe, "-lws2_32", "-lshell32", "-ladvapi32", "-luser32"],
                       capture_output=True, text=True)
    if b.returncode:
        res.append((name, "BUILD-ERROR " + b.stderr[:100]))
    else:
        try:
            t = subprocess.run([sys.executable, "-I", os.path.join(HERE, "test_bridge.py"), exe, PORT],
                               capture_output=True, text=True, timeout=180)
            res.append((name, "killed" if t.returncode != 0 else "SURVIVED"))
        except subprocess.TimeoutExpired:
            res.append((name, "killed (timeout/hang)"))
    print(res[-1], f"{time.time() - t0:.0f}s", flush=True)

k = sum(1 for _, r in res if r.startswith("killed"))
print(f"\nmutants: {len(res)}  killed: {k}  survived/other: {len(res) - k}")
for n, r in res:
    if not r.startswith("killed"):
        print("  NOT KILLED:", n, "->", r)
sys.exit(0 if k == len(res) else 1)
