"""Hand mutation check for root_validate (COH-517): each mutant must make tests/test_roots.c fail.
Usage: python tests/mutate_roots.py"""
import os, shutil, subprocess, sys, tempfile
HERE = os.path.dirname(os.path.abspath(__file__)); ROOT = os.path.dirname(HERE)
src = open(os.path.join(ROOT, "ai-helper.c"), encoding="utf-8", newline="").read()
MUT = [
 ("drop drive-root check",   "if (wcslen(c) <= 3) return", "if (0) return"),
 ("drop duplicate check",    "if (_wcsicmp(e, c) == 0) return", "if (0) return"),
 ("limit >= to >",           "if (n >= MAX_ROOTS) return", "if (n > MAX_ROOTS) return"),
 ("drop directory check",    "!(at & FILE_ATTRIBUTE_DIRECTORY))", "0)"),
 ("compare raw existing (no canon)", "if (!canon_path(existing[i], e, MAX_PATH)) wcscpy(e, existing[i]);", "wcscpy(e, existing[i]);"),
]
bad = 0
for name, a, b in MUT:
    assert src.count(a) == 1, name
    d = tempfile.mkdtemp(prefix="aih-mut-"); os.makedirs(os.path.join(d, "tests"))
    open(os.path.join(d, "ai-helper.c"), "w", encoding="utf-8", newline="").write(src.replace(a, b))
    for f in os.listdir(ROOT):
        if f.endswith((".h", ".rc", ".ico")): shutil.copy(os.path.join(ROOT, f), d)
    shutil.copy(os.path.join(HERE, "test_roots.c"), os.path.join(d, "tests"))
    exe = os.path.join(d, "t.exe")
    c = subprocess.run(["gcc", "-O0", "-municode", os.path.join(d, "tests", "test_roots.c"), "-o", exe, "-lws2_32", "-lshell32", "-lcomdlg32", "-ladvapi32", "-luser32", "-lcomctl32", "-lole32", "-luuid", "-lgdi32"], capture_output=True, text=True)
    if c.returncode: print("BUILD-ERR", name, c.stderr[-300:]); bad += 1; continue
    r = subprocess.run([exe, os.path.join(d, "scratch")], capture_output=True, text=True)
    killed = r.returncode != 0
    print(("KILLED   " if killed else "SURVIVED ") + name, flush=True)
    bad += (not killed)
print("ALL MUTANTS KILLED" if not bad else f"{bad} PROBLEM(S)")
sys.exit(1 if bad else 0)
