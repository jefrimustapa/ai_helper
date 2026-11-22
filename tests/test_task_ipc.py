"""Tests for ai-helper task trigger and IPC bridge.
Usage: python tests/test_task_ipc.py <exe> [port]
"""
import os, sys, socket, subprocess, tempfile, time

EXE = os.path.abspath(sys.argv[1])
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 5073
tmp = tempfile.mkdtemp(prefix="aih-test-ipc-")

def write_ini():
    ini = os.path.join(tmp, "test.ini")
    lines = [
        "[Settings]",
        "Initialized=1",
        "[Apps]",
        r"App0=C:\Windows\System32\hostname.exe",
        "[Tasks]",
        "Task0=PowerToolGovern",
        "Task1=CatoConnect",
        "[IPC]",
        "Target0=PowerToolWindow",
        "Target1=SampleAppWindow",
    ]
    with open(ini, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    return ini

proc = None

def stop():
    global proc
    if proc:
        proc.kill()
        proc.wait()
        proc = None

def start():
    global proc
    stop()
    ini = write_ini()
    env = dict(os.environ, AIHELPER_TEST_NOLAUNCH="1")
    proc = subprocess.Popen([EXE, f"/port:{PORT}", f"/ini:{ini}"], env=env)
    for _ in range(50):
        try:
            socket.create_connection(("127.0.0.1", PORT), 0.2).close()
            return ini
        except OSError:
            time.sleep(0.1)
    raise SystemExit("helper did not start")

def send(cmd, timeout=3):
    s = socket.create_connection(("127.0.0.1", PORT), timeout)
    s.sendall(cmd.encode("utf-8") + b"\r\n")
    out = b""
    while True:
        c = s.recv(1024)
        if not c:
            break
        out += c
    s.close()
    return out.decode("utf-8", errors="replace").strip()

fails = []
def check(desc, ok, extra=""):
    if not ok:
        print(f"FAIL: {desc} {extra}")
        fails.append(desc)
    else:
        print(f"PASS: {desc}")

try:
    start()
    
    # 1. Standard application launch
    r = send("hostname")
    check("Legacy command: hostname", r == "OK", r)

    r = send("notepad")
    check("Disallowed command: notepad", r == "ERR application not allowed", r)

    # 2. Task trigger
    r = send("task PowerToolGovern")
    check("Task: allowlisted task PowerToolGovern", r == "OK", r)

    r = send("task CatoConnect")
    check("Task: allowlisted task CatoConnect", r == "OK", r)

    r = send("task \"PowerToolGovern\"")
    check("Task: quoted allowlisted task", r == "OK", r)

    r = send("task UnauthorizedTask")
    check("Task: disallowed task", r == "ERR task not allowed", r)

    r = send("task")
    check("Task: empty task name", r == "ERR empty task name", r)

    # 3. IPC command
    r = send("ipc PowerToolWindow govern:off")
    check("IPC: allowlisted target with message", r == "OK", r)

    r = send("ipc \"PowerToolWindow\" govern:on")
    check("IPC: quoted target with message", r == "OK", r)

    r = send("ipc DisallowedWindow hello")
    check("IPC: disallowed target", r == "ERR ipc target not allowed", r)

    r = send("ipc")
    check("IPC: empty ipc command", r == "ERR empty ipc command", r)

    r = send("ipc PowerToolWindow")
    check("IPC: missing message", r == "ERR empty ipc message", r)

finally:
    stop()
    import shutil
    shutil.rmtree(tmp, ignore_errors=True)

print(f"\n{12 - len(fails)}/12 passed")
sys.exit(1 if fails else 0)
