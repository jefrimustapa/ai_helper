"""Unit test for Cato enable/disable workflow and ai-helper integration.
Usage: python tests/test_cato_unit.py
"""
import os, sys, socket, subprocess, tempfile, time

# 1. Test Cato Status Detection Logic
def evaluate_cato_status(adapter_up, has_ip, default_route, vpn_running):
    is_connected = adapter_up and has_ip and default_route and (vpn_running == 'Running')
    return 'CONNECTED' if is_connected else 'DISCONNECTED'

def test_status_logic():
    print("--- 1. Testing Cato Status Detection Logic ---")
    assert evaluate_cato_status(True, True, True, 'Running') == 'CONNECTED', "Should be CONNECTED"
    assert evaluate_cato_status(False, False, False, 'Stopped') == 'DISCONNECTED', "Should be DISCONNECTED"
    assert evaluate_cato_status(True, True, False, 'Running') == 'DISCONNECTED', "Should be DISCONNECTED without route"
    assert evaluate_cato_status(True, True, True, 'Stopped') == 'DISCONNECTED', "Should be DISCONNECTED if VPN stopped"
    print("PASS: Cato status determination logic verified (4/4 states)")

# 2. Test ai-helper Enable/Disable Cato Commands (via Task and IPC)
def test_aihelper_cato_commands():
    print("\n--- 2. Testing ai-helper Cato Enable/Disable Commands ---")
    EXE = os.path.abspath("ai-helper-test.exe")
    PORT = 5075
    tmp = tempfile.mkdtemp(prefix="aih-cato-test-")
    ini = os.path.join(tmp, "cato_test.ini")

    lines = [
        "[Settings]",
        "Initialized=1",
        "[Apps]",
        r"App0=C:\Windows\explorer.exe",
        "[Tasks]",
        "Task0=CatoEnable",
        "Task1=CatoDisable",
        "Task2=PowerToolGovern",
        "[IPC]",
        "Target0=PowerToolWindow",
    ]
    with open(ini, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")

    env = dict(os.environ, AIHELPER_TEST_NOLAUNCH="1")
    proc = subprocess.Popen([EXE, f"/port:{PORT}", f"/ini:{ini}"], env=env)
    
    # Wait for helper to start
    started = False
    for _ in range(50):
        try:
            socket.create_connection(("127.0.0.1", PORT), 0.2).close()
            started = True
            break
        except OSError:
            time.sleep(0.1)
    assert started, "ai-helper test instance failed to start"

    def send(cmd):
        s = socket.create_connection(("127.0.0.1", PORT), 3)
        s.sendall(cmd.encode("utf-8") + b"\r\n")
        out = b""
        while True:
            c = s.recv(1024)
            if not c:
                break
            out += c
        s.close()
        return out.decode("utf-8", errors="replace").strip()

    try:
        # A. Enable Cato via Task Trigger
        r_enable_task = send("task CatoEnable")
        print(f"Task Enable Command: {r_enable_task}")
        assert r_enable_task == "OK", f"Expected OK, got {r_enable_task}"

        # B. Disable Cato via Task Trigger
        r_disable_task = send("task CatoDisable")
        print(f"Task Disable Command: {r_disable_task}")
        assert r_disable_task == "OK", f"Expected OK, got {r_disable_task}"

        # C. Enable Cato via IPC Bridge (govern:off)
        r_enable_ipc = send("ipc PowerToolWindow govern:off")
        print(f"IPC Enable Command (govern:off): {r_enable_ipc}")
        assert r_enable_ipc == "OK", f"Expected OK, got {r_enable_ipc}"

        # D. Disable Cato via IPC Bridge (govern:on)
        r_disable_ipc = send("ipc PowerToolWindow govern:on")
        print(f"IPC Disable Command (govern:on): {r_disable_ipc}")
        assert r_disable_ipc == "OK", f"Expected OK, got {r_disable_ipc}"

        # E. Verify unallowlisted task is rejected
        r_unauth = send("task UnauthorizedCatoAction")
        print(f"Disallowed Task Command: {r_unauth}")
        assert r_unauth == "ERR task not allowed", f"Expected ERR task not allowed, got {r_unauth}"

        # F. Verify unallowlisted IPC target is rejected
        r_unauth_ipc = send("ipc UnknownAppWindow govern:off")
        print(f"Disallowed IPC Command: {r_unauth_ipc}")
        assert r_unauth_ipc == "ERR ipc target not allowed", f"Expected ERR ipc target not allowed, got {r_unauth_ipc}"

        print("PASS: ai-helper Cato Enable/Disable Task and IPC commands verified (6/6 checks)")
    finally:
        proc.kill()
        proc.wait()
        import shutil
        shutil.rmtree(tmp, ignore_errors=True)

# 3. Test cato-control Script Dry-Run Integration
def test_cato_control_dryrun():
    print("\n--- 3. Testing cato-control Script Dry-Run ---")
    script_path = r"D:\ai_skills\cato-control\scripts\cato_control.ps1"
    assert os.path.exists(script_path), f"Script missing: {script_path}"

    # Dry-run enable
    r_en = subprocess.run(['pwsh', '-File', script_path, '-Action', 'enable'], capture_output=True, text=True)
    assert r_en.returncode == 0, f"Enable dry-run failed: {r_en.stderr}"
    assert "PLAN: enable CATO" in r_en.stdout, "Enable plan text missing"
    assert "DRY-RUN only" in r_en.stdout, "Dry-run guardrail missing"
    print("PASS: cato-control enable dry-run verified")

    # Dry-run disable
    r_dis = subprocess.run(['pwsh', '-File', script_path, '-Action', 'disable'], capture_output=True, text=True)
    assert r_dis.returncode == 0, f"Disable dry-run failed: {r_dis.stderr}"
    assert "PLAN: disable CATO" in r_dis.stdout, "Disable plan text missing"
    assert "DRY-RUN only" in r_dis.stdout, "Dry-run guardrail missing"
    print("PASS: cato-control disable dry-run verified")

if __name__ == "__main__":
    try:
        test_status_logic()
        test_aihelper_cato_commands()
        test_cato_control_dryrun()
        print("\n==========================================")
        print(" ALL CATO ENABLE/DISABLE UNIT TESTS PASSED ")
        print("==========================================")
    except AssertionError as e:
        print(f"\nTEST FAILED: {e}")
        sys.exit(1)
