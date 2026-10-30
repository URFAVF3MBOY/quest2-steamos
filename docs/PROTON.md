# Windows games (Proton)

Windows games run through Valve's ARM64 build of Proton, the same one the Steam Frame uses. The game's x86-64 code runs through FEX, inside Proton (ARM64EC). Install and start games from Steam as usual. Flat games show up as a window in VR.

What we've tested: **OneShot** (AppID 420530) runs.

## Known limits

- **No sound.** Games that insist on opening a sound device can fail. OneShot shows "Error creating OpenAL context" when the audio system isn't ready yet. It worked once the system had been up for a few minutes.
- **The first start of a game is slow.** Proton sets up a new prefix, and FEX translates the game's code for the first time.
- **Proton logging is on** for every game (see below). It costs a little speed. Set `PROTON_LOG=0 %command%` in a game's launch options to turn it off.

## The Ruby fix

**Which games:** anything built on Ruby for Windows. That's RPG Maker XP, VX and VX Ace games and mkxp-based games such as OneShot. They close right after you start them, with no error message.

**Why:** at startup, Ruby looks for a private table inside the C runtime. It reads the machine code of the `_isatty` function in `ucrtbase.dll` and searches for a specific pattern: `lea rdx,[rip+X]`, then `add rsp,N; ret`. Wine's x86-64 `ucrtbase.dll` contains an `_isatty` written to match that pattern, with the comment "needed for Ruby runtime" in `dlls/msvcrt/file.c`. But that code is switched off for ARM64EC builds. In ARM64 Proton, x86-64 programs get a 16-byte jump stub instead. Ruby doesn't find the pattern and quits with exit code 1. **This also affects the Steam Frame.** As of October 2026 it's the same in Valve's newest Wine branch.

**What we do:** before a game starts, `q2rubyfix` (in `image/src/prof/`) patches Proton's `files/lib/wine/aarch64-windows/ucrtbase.dll` once:

1. It writes 17 bytes of x86-64 code into unused space at the end of the file's x86-64 thunk section (`.hexpthk`):
   ```
   jmp  <the original _isatty thunk>     ; all that actually runs
   lea  rdx,[rip+<the file table>]       ; only read by Ruby
   add  rsp,0x28
   ret
   ```
2. It points the `_isatty` entry of the x86-64 (ARM64EC) export table at that code. The ARM64 export table, used by Wine's own programs, isn't touched.
3. It reads the table address from the ARM64EC code of `_get_osfhandle`, and checks the result with a copy of Ruby's own search before writing anything.

The original file is kept next to it as `ucrtbase.dll.q2orig`. If Proton is updated, the fix is applied again at the next start. "Verify integrity of tool files" on Proton in Steam undoes it. Create `/var/lib/q2/norubyfix` to stop it.

This should really be fixed in Proton itself, by giving Wine's ARM64EC build the same x86-64 `_isatty`. If you report it to Valve, link this page.

### Things that don't work for this (so you don't have to try them)

We tried these, and each one fails for a different reason:

- **Microsoft's x64 `ucrtbase.dll` next to the game:** Wine loads `ucrtbase` from the prefix's `system32` (API sets resolve there), so a copy in the game folder is never used.
- **Microsoft's x64 `ucrtbase.dll` in `system32`:** Wine's own helper programs (wineboot and others) are ARM64 programs and can't load an x64 DLL (`c000007b`). The prefix stops starting.
- **A per-program DLL override:** with an x64 file in `system32`, Wine looks for the built-in copy in `x86_64-windows`. ARM64 programs can't load that either.

If you ran one of the test builds from 312 or 313, the launcher repairs the prefix by itself (`q2ucrt`).

## Game logs

`q2launchwrap` runs Proton with `PROTON_LOG` on and saves a shortened log after the game exits. It's `proton-log.txt` in the logs ([TROUBLESHOOTING.md](TROUBLESHOOTING.md)). It contains the start of the log, every error and process line, and the end of the log. For every process that ended with an error code, it also has that process's last 700 lines.

To log more, set the game's launch options, for example `PROTON_LOG=+process,+wgl,+opengl %command%` for OpenGL problems.

The full logs stay on the headset in `~/q2proton/steam-<AppID>.log`.
