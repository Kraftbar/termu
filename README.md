# Termu

Tiny SecureCRT-style terminal prototype for Windows.

## Build And Run From WSL

```sh
x86_64-w64-mingw32-gcc termu.c backend_conpty.c -o termu.exe -mwindows -O2 -Wall -municode -lcrypt32 -liphlpapi
./termu.exe &
```

If MinGW is missing, use the installed Visual Studio tools:

```sh
powershell.exe -NoProfile -Command '$root = "C:\Users\Nybo\Downloads\secureCRT_withwinscp"; $vsdev = "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat"; cmd.exe /d /s /c "cd /d `"$root`" && call `"$vsdev`" -arch=x64 -host_arch=x64 && cl /nologo /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0A00 /W4 /O2 termu.c backend_conpty.c /link /SUBSYSTEM:WINDOWS /OUT:termu.exe user32.lib gdi32.lib crypt32.lib iphlpapi.lib"'
powershell.exe -NoProfile -Command 'Start-Process -FilePath "C:\Users\Nybo\Downloads\secureCRT_withwinscp\termu.exe" -WorkingDirectory "C:\Users\Nybo\Downloads\secureCRT_withwinscp"'
```

After any code change, future LLMs must build and launch it from WSL, then tell
the user what changed and what specific behavior to check.

## Notes

`termu.c` owns the UI. `backend_conpty.c` owns local `cmd.exe` ConPTY I/O.
Add SSH later as another `TermBackend`; keep SFTP/SCP separate from terminal I/O.
Hosts load from local-only `termu_hosts.txt` as `name|command`; use
`termu_hosts.example.txt` as the tracked template. Saved passwords are DPAPI
blobs in an optional third field and should stay uncommitted.
`Scan LAN` does a runtime-only IPv4 `/24` ICMP sweep; discovered IPs are not saved.
Defer font zoom and last-line flicker work for now; both are tricky rabbit holes.

## Roadmap

- [ ] Usable terminal layer
- [ ] SecureCRT-style hosts
- [ ] WinSCP-style file drop

Do not add SecureCRT/WinSCP features like hosts, SSH profiles, SCP, or SFTP until
the user agrees the terminal layer is ready.
