"""Builds and runs tests/test_roots.c (unit tests for root_validate, COH-517).
Usage: python tests/test_roots.py"""
import os, subprocess, sys, tempfile
HERE = os.path.dirname(os.path.abspath(__file__))
tmp = tempfile.mkdtemp(prefix="aih-roots-")
exe = os.path.join(tmp, "test_roots.exe")
b = subprocess.run(["gcc", "-O0", "-municode", os.path.join(HERE, "test_roots.c"), "-o", exe,
                    "-lws2_32", "-lshell32", "-lcomdlg32", "-ladvapi32", "-luser32", "-lcomctl32", "-lole32", "-luuid", "-lgdi32"],
                   capture_output=True, text=True)
if b.returncode:
    print(b.stderr); sys.exit(1)
r = subprocess.run([exe, os.path.join(tmp, "scratch")])
sys.exit(r.returncode)
