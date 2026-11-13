"""E2E for Bridge folders UI (COH-517) through the real Settings window. Usage: python tests/test_e2e_roots.py <exe> <screenshot-prefix>"""
import sys, os, time, subprocess, socket, tempfile, urllib.request, configparser, win32gui, win32con, win32process
from pywinauto import Application, Desktop
from PIL import ImageGrab
EXE=os.path.abspath(sys.argv[1]); OUT=sys.argv[2]; PORT=5092; TOKEN="00112233445566778899aabbccddeeff"
tmp=tempfile.mkdtemp(prefix="aih-e2e-"); base=os.path.join(tmp,"base"); newf=os.path.join(tmp,"newfolder")
os.makedirs(base); os.makedirs(newf); open(os.path.join(newf,"a.txt"),"w").write("x")
INI=os.path.join(tmp,"t.ini")
open(INI,"w").write(f"[Settings]\nInitialized=1\n[Apps]\nApp0=C:\\Windows\\explorer.exe\n[Bridge]\nEnabled=1\nToken={TOKEN}\nRoot0={base}\n")
res=[]
def chk(n,c): print(("PASS " if c else "FAIL ")+n,flush=True); res.append(c)
def get(path):
    u=f"http://127.0.0.1:{PORT}/open?token={TOKEN}&path={urllib.request.quote(path)}"
    r=urllib.request.Request(u,headers={"Sec-Fetch-Site":"same-site","Sec-Fetch-Mode":"navigate","Sec-Fetch-Dest":"document","Sec-Fetch-User":"?1"})
    try: return urllib.request.urlopen(r,timeout=5).status
    except urllib.error.HTTPError as e: return e.code
def roots():
    cp=configparser.ConfigParser(interpolation=None); cp.read(INI); return [v for k,v in cp.items("Bridge") if k.startswith("root")]
env=dict(os.environ,AIHELPER_TEST_NOLAUNCH="1")
p=subprocess.Popen([EXE,f"/port:{PORT}","/ini:"+INI],env=env)
try:
    for _ in range(60):
        try: socket.create_connection(("127.0.0.1",PORT),0.2).close(); break
        except OSError: time.sleep(0.1)
    h=None
    for _ in range(50):
        def cb(hw,_):
            global h
            if win32process.GetWindowThreadProcessId(hw)[1]==p.pid and win32gui.GetClassName(hw)=="AiHelperWindow": h=hw
        win32gui.EnumWindows(cb,None)
        if h: break
        time.sleep(0.1)
    win32gui.ShowWindow(h,win32con.SW_SHOW); win32gui.SetForegroundWindow(h); time.sleep(0.5)
    w=Application(backend="win32").connect(handle=h).window(handle=h)
    w.child_window(class_name="SysTabControl32").select("Bridge"); time.sleep(0.3)
    lst=w.child_window(class_name="ListBox",visible_only=True)
    chk("default root listed", base in lst.texts()[1:] or any(base.lower()==t.lower() for t in lst.item_texts()))
    chk("newfolder blocked before Add", get(newf)==403)
    # Add folder via real dialog
    w.child_window(title="Add folder...",class_name="Button").click(); time.sleep(1.2)
    dlg=Desktop(backend="uia").window(title_re=".*(Select|Choose|Folder|Bridge).*",visible_only=True) if False else None
    from pywinauto.keyboard import send_keys
    send_keys(newf.replace("(","{(}").replace(")","{)}").replace("~","{~}").replace("+","{+}").replace("^","{^}").replace("%","{%}"),with_spaces=True,pause=0.01); time.sleep(0.3)
    send_keys("{ENTER}"); time.sleep(0.5); send_keys("{ENTER}"); time.sleep(1.0)
    items=lst.item_texts()
    chk("Add put folder in list", any(newf.lower()==t.lower() for t in items))
    w.child_window(title="Save",class_name="Button").click(); time.sleep(1.0)
    chk("Save wrote it to ini", any(newf.lower()==r.lower() for r in roots()))
    chk("newfolder works WITHOUT restart", get(newf)==200)
    ImageGrab.grab(win32gui.GetWindowRect(h)).save(OUT+"-added.png")
    chk("traversal '..' still rejected", get(base+"\\..\\newfolder")!=200)
    chk("UNC still rejected", get("\\\\localhost\\c$\\Windows")!=200)
    # Remove
    lst.select([i for i,t in enumerate(lst.item_texts()) if t.lower()==newf.lower()][0]); time.sleep(0.2)
    w.child_window(title="Remove",class_name="Button",visible_only=True).click(); time.sleep(0.2)
    w.child_window(title="Save",class_name="Button").click(); time.sleep(1.0)
    chk("Remove+Save -> blocked again", get(newf)==403)
    chk("ini no longer has it", not any(newf.lower()==r.lower() for r in roots()))
    # root drive
    w.child_window(title="Add folder...",class_name="Button").click(); time.sleep(1.2)
    send_keys("C:\\",with_spaces=True,pause=0.01); send_keys("{ENTER}"); time.sleep(0.5); send_keys("{ENTER}"); time.sleep(1.0)
    warn=None
    for t in Desktop(backend="win32").windows():
        if win32process.GetWindowThreadProcessId(t.handle)[1]==p.pid and t.handle!=h and t.class_name()=="#32770": warn=t
    chk("root drive C:\\ -> warning shown", warn is not None)
    if warn is not None: ImageGrab.grab(warn.rectangle().width and (warn.rectangle().left,warn.rectangle().top,warn.rectangle().right,warn.rectangle().bottom)).save(OUT+"-warn.png"); warn.close() if False else send_keys("{ENTER}")
    time.sleep(0.5)
    chk("list unchanged after root drive attempt", not any(t.lower().startswith("c:\\") and len(t)<=3 for t in lst.item_texts()))
finally:
    p.kill()
print("ALL PASSED" if all(res) else "SOME FAILED")

