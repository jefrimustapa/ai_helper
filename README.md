# ai-helper

A small Windows tray app that listens on a localhost TCP port and launches allowlisted applications on request.

## Build

Requires MinGW-w64 GCC (for example WinLibs: `winget install BrechtSanders.WinLibs.POSIX.UCRT`).

```bat
build.bat
```

## Use

Run `ai-helper.exe`. The settings page has "Start with Windows", the listening port (default 5055, bound to 127.0.0.1 only) and the list of allowed applications. Settings are stored in `%APPDATA%\AiHelper\settings.ini`.

Send one line per connection: `<app> [params]`, e.g. `explorer C:\Temp`. The reply is `OK` or `ERR <reason>`. Only allowlisted apps can be launched.

## HTTP bridge: `GET /open` (clickable links)

Lets a link in a chat open a folder (or highlight a file) in Explorer:

```
http://127.0.0.1:5055/open?path=D%3A%5Cai_project%5Cpaperclip_setup&token=<token>
```

Use `127.0.0.1`, not `localhost` (the helper binds IPv4 only). The first line of a connection starting with `GET ` is served as HTTP; anything else is the old one-line protocol, unchanged.

**Off by default.** Switch it on in Settings: tick **Enable HTTP bridge** and press **Save**. It takes effect immediately (no restart) and only `Enabled=` in `settings.ini` is rewritten (Token and Root entries are never touched by the UI). If there is no token (random generator failed) the box is disabled and the bridge stays closed. The `[Bridge]` section of `%APPDATA%\AiHelper\settings.ini` can still be edited by hand (read at start-up; restart the helper after editing):

```ini
[Bridge]
Enabled=0                      ; set to 1 to switch the bridge on
Token=<32 hex chars>           ; generated on first start, permanent
Root0=D:\ai_project\paperclip_setup   ; allowed roots, one per line: Root0..Root15
Root1=...
```

- **Regenerate the token:** exit the helper from the tray, delete the `Token=` line from `settings.ini`, start it again (a new random token is written). Old links stop working.
- **Allowed roots:** add `Root1=`, `Root2=` ... A path must resolve (symlinks/junctions followed) to inside a root, on a path-component boundary (`D:\ai_project_x` is not inside `D:\ai_project`).
- Folder = opened in Explorer. File = `explorer /select,` (highlight only; nothing is executed). Only the allowlisted `explorer.exe` is launched.
- Checks (all must pass, otherwise 4xx and nothing launches): bridge enabled; `Host` is exactly `127.0.0.1:<port>`; `Sec-Fetch-Mode: navigate`, `Sec-Fetch-Dest: document`, `Sec-Fetch-User: ?1`, `Sec-Fetch-Site` is `same-site` or `same-origin`; token matches; path is drive-absolute, has no `..`, wildcards or control characters, and is inside a root. `Origin`/`Referer` are deliberately not used (empty when a link is clicked from Paperclip). A link opened from a page on another site (cross-site) or typed in the address bar is refused.
- Reply is `200` + a tiny HTML page that calls `window.close()`, so the tab opened by the click closes itself. `GET /ping` returns `ai-helper` (same Host check) for health checks.

## Tests

`tests/test_bridge.py <exe> [port]` runs a throw-away instance (own ini, random token, other port, no registry writes, Explorer launch stubbed) and checks positives, negatives, boundary values, fuzzed paths/requests and that the old protocol still works. `tests/test_ui_toggle.py <exe> [port] [shot.png]` drives the Settings checkbox with pywinauto (env `AIHELPER_TEST_NORNG=1` simulates a failed RNG). `tests/mutate.py [port] [lo] [hi]` (run in batches of ~3 mutants, ~60s each) breaks one check at a time and requires the tests to fail for every mutant. Test instance flags: `ai-helper.exe /port:5071 /ini:C:\path\test.ini`; env `AIHELPER_TEST_NOLAUNCH=1` makes `/open` reply `would-launch:open` (folder) or `would-launch:select` (file) instead of starting Explorer.
