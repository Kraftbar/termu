# Termu

Tiny SecureCRT-style terminal prototype for Windows.

## Build And Run From WSL

```sh
x86_64-w64-mingw32-gcc sw/termu.c sw/backend_conpty.c -o sw/termu.exe -mwindows -O2 -Wall -municode
./sw/termu.exe &
```

If MinGW is missing, use the installed Visual Studio tools:

```sh
powershell.exe -NoProfile -Command '$root = "C:\Users\Nybo\Downloads\secureCRT_withwinscp"; $vsdev = "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat"; cmd.exe /d /s /c "cd /d `"$root`" && call `"$vsdev`" -arch=x64 -host_arch=x64 && cl /nologo /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0A00 /W4 /O2 sw\termu.c sw\backend_conpty.c /link /SUBSYSTEM:WINDOWS /OUT:sw\termu.exe user32.lib gdi32.lib"'
powershell.exe -NoProfile -Command 'Start-Process -FilePath "C:\Users\Nybo\Downloads\secureCRT_withwinscp\sw\termu.exe" -WorkingDirectory "C:\Users\Nybo\Downloads\secureCRT_withwinscp"'
```

After any code change, future LLMs must build and launch it from WSL, then tell
the user what changed and what specific behavior to check.

## Notes

`sw/termu.c` owns the UI. `sw/backend_conpty.c` owns local `cmd.exe` ConPTY I/O.
Add SSH later as another `TermBackend`; keep SFTP/SCP separate from terminal I/O.

## Roadmap

- [ ] Usable terminal layer
- [ ] SecureCRT-style hosts
- [ ] WinSCP-style file drop

Do not add SecureCRT/WinSCP features like hosts, SSH profiles, SCP, or SFTP until
the user agrees the terminal layer is ready.
