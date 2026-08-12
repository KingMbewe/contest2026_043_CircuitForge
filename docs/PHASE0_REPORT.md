# Phase 0 Report — openVela Build & Emulator Bring-up on Native Windows

**Project:** VelaPaw (contest2026_043_CircuitForge) — multi-pet recognition smart feeder
**Goal of Phase 0:** prove the openVela toolchain can build *and* run an emulator image on this machine before writing any application code.
**Date:** 2026-06-27
**Result:** ✅ Success — full emulator image builds end-to-end and boots in QEMU to an interactive NuttShell prompt (`openvela-ap>`).

---

## 1. Environment

| Item | Value |
|---|---|
| Host OS | Windows 11 Pro (10.0.26200) |
| Shell | Git Bash (POSIX) + PowerShell |
| Workspace | `D:\openVela` (moved from C: — see §2.0) |
| Build system | CMake 3.31.7 + Ninja 1.12.1 (the `build.sh` Make path is Linux-only) |
| Cross toolchain | `arm-none-eabi-gcc` 13.4.0 (bundled Windows prebuilt) |
| Emulator | bundled Android/goldfish `emulator.exe` → `qemu-system-armel-headless.exe` |
| Board config | `vendor/openvela/boards/vela/configs/goldfish-armeabi-v7a-ap` (chosen for `goldfish_camera` virtual camera) |

**Key environment decision:** openVela has migrated from GNU Make to **CMake**, and the CMake path supports Windows natively. All required tools ship as Windows prebuilts (`cmake.exe`, `ninja.exe`, `kconfig-*.exe`, `genromfs.exe`, `arm-none-eabi-*`, `qemu`). The legacy `build.sh -m` flow was **not** usable because it mounts a FUSE union filesystem (`unionfs-fuse`/`fusermount`) that does not exist on Windows.

A helper script, `D:\openVela\vela-env.sh`, was created to put the toolchain on `PATH` and provide `vela_configure` and `vela_olddefconfig` functions.

---

## 2. Issues encountered and resolutions

12 distinct blockers were hit and resolved, spanning repo sync, environment setup, Windows shell incompatibilities, missing Python deps, and config trimming.

### Summary table

| # | Phase | Symptom | Root cause | Resolution |
|---|---|---|---|---|
| 0a | Sync | `repo init` GPG failure | Windows keyring path malformed in `repo` launcher | `repo init --no-repo-verify` |
| 0b | Sync | `.repo/repo.tmp`→`repo` rename Access Denied | Windows Defender holding handles | Retry rename via PowerShell `Move-Item` |
| 0c | Sync | symlink "required privilege not held" | `repo` creates dir-symlinks; Windows blocks unprivileged symlinks | Enabled **Developer Mode** (registry, elevated) |
| 0d | Sync | `No space left on device` at 23/263 repos | C: had only ~20 GB free | Relocated checkout to `D:\openVela` (650 GB free) |
| 1 | Configure | `Kconfig depends on kconfiglib` | `kconfiglib` not installed | `pip install kconfiglib` |
| 2 | Configure | still "depends on kconfiglib" | `find_program(olddefconfig)` — console script not on PATH | Added Python `Scripts` dir to PATH |
| 3 | Configure | `No config file found` | board config path resolved against `nuttx/`, not `vendor/` | Pass absolute Windows path to defconfig dir |
| 4 | Build | `romfs_etc.c`: `"etc" was unexpected at this time` | bash `if [ ]` run under `cmd.exe` | Patched cmake to use `bash -c` |
| 5 | Build | `mkallsyms.py` not executed | invoked bare; `.py` not runnable via `cmd.exe` | Prefix `${Python3_EXECUTABLE}` |
| 6 | Build | `mkallsyms.py` exit 22 (silent) | missing `cxxfilt` module | `pip install cxxfilt` |
| 7 | Build | `ft2build.h: No such file` | LVGL FreeType enabled; headers only exported to kernel target | Disabled `LV_USE_FREETYPE` |
| 8 | Build | `qjsc` `No CMAKE_C_COMPILER` | QuickJS builds a host tool; no native host compiler | Disabled `INTERPRETERS_QUICKJS` |
| 9 | Build | `png.h: No such file` | LVGL libpng enabled; source not populated | Disabled `LV_USE_LIBPNG` (lodepng remains) |
| 10 | Link | `undefined reference to sem_open/sem_close` | alsa-lib needs named semaphores | Enabled `FS_NAMED_SEMAPHORES` (+ `olddefconfig`) |
| 11 | Post-link | `mkallsyms.py` crash: `cxxfilt LibraryNotFound: ('c',)` | cxxfilt loads `libc` to demangle — none on Windows | Disabled `ALLSYMS` |
| 12 | Post-link | `System.map`: `' was unexpected` | `nm | grep '...' | sort` run under `cmd.exe` | Patched cmake to use `bash -c` |

### Detail

#### 0. Repository sync (getting the source)
- **0a — GPG self-verification.** The `repo` launcher (v2.54) tries to GPG-verify its own release tag, but the gnupg keyring path was mangled by Windows path handling. Fixed with `--no-repo-verify` (disables verification of the repo tool itself; does not affect source integrity).
- **0b — Rename lock.** `repo init` clones the tool into `.repo/repo.tmp` then renames it to `.repo/repo`; the rename failed with `Access denied` because Windows Defender was scanning the freshly-cloned files. Resolved by retrying the rename via PowerShell after a short delay.
- **0c — Symlink privilege.** `repo` creates a directory symlink for each project's internal `.git`. Windows blocks symlink creation without privilege (`A required privilege is not held by the client`). `repo` passes `SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE`, so enabling **Developer Mode** (registry `AllowDevelopmentWithoutDevLicense=1`, set via an elevated prompt) made unprivileged symlinks work — no admin/reboot needed.
- **0d — Disk space.** The full tree (263 git projects incl. prebuilt toolchains) exhausted C:'s ~20 GB. The workspace was moved to `D:\openVela` (650 GB free) and re-synced cleanly. `repo sync -c -j8` completed all 263 projects.

#### 1–3. CMake configure
- **1 — kconfiglib.** openVela's CMake uses kconfiglib (Python) for Kconfig processing; `pip install kconfiglib` (into the exact interpreter CMake auto-selects).
- **2 — console scripts on PATH.** The check is `find_program(KCONFIGLIB olddefconfig)` — it looks for the **`olddefconfig` executable**, not the module. pip placed `olddefconfig.exe` in a `Scripts` dir not on PATH; adding that dir fixed it.
- **3 — board config resolution.** `-DBOARD_CONFIG=vela/goldfish-...` resolved against `nuttx/`, but the vela board lives under `vendor/`. Passing the **absolute Windows path** to the defconfig directory (`D:/openVela/vendor/openvela/boards/vela/configs/goldfish-armeabi-v7a-ap`) resolved correctly. Configure then succeeded (and FetchContent auto-downloaded speexdsp).

#### 4, 12. Windows shell vs. POSIX shell (cmd.exe codegen)
openVela's CMake emits some build-time codegen commands as bash/`sed`/`grep` one-liners. On Windows, CMake runs custom commands through **`cmd.exe`**, which cannot parse POSIX syntax.
- **4 — `romfs_etc.c`** (`cmake/nuttx_add_romfs.cmake`): the `if [ "etc" != "" ]; then … fi` guard plus a `genromfs | xxd | sed` pipeline. Patched: replaced the literal guard with a plain copy and routed `genromfs`/`xxd`/`sed` through a single `bash -c`.
- **12 — `System.map`** (`CMakeLists.txt` ~line 865): `nm -C nuttx | grep -v '…' | sort > System.map`. Patched: wrapped the pipeline in `bash -c`.

#### 5, 6, 11. Python tooling on Windows
- **5 — interpreter prefix** (`cmake/nuttx_multiple_link.cmake`): `mkallsyms.py` was invoked bare; `cmd.exe` can't run a `.py` directly. Prefixed with `${Python3_EXECUTABLE}` (matching how the rest of the tree invokes Python tools).
- **6 — missing module.** With the prefix, `mkallsyms.py` exited 22 (EINVAL) with no message because it printed the error then called `os._exit()` (which skips stdout flush). Cause: missing `cxxfilt`. `pip install cxxfilt`.
- **11 — cxxfilt unusable on Windows.** On the *real* ELF, `mkallsyms.py` crashed in `cxxfilt.demangle`: `LibraryNotFound: Cannot find any of libraries: ('c',)`. `cxxfilt` demangles by loading the system **`libc`** to call the C++ ABI demangler — Windows has no such shared library, so cxxfilt fundamentally cannot run here. This tooling is only used by the **ALLSYMS** feature (runtime symbol table for backtraces), which is not needed for the MVP. Disabling `CONFIG_ALLSYMS` removed the dependency entirely. (The main `nuttx` link had already succeeded; without ALLSYMS that first link *is* the final image.)

#### 7, 8, 9, 10. Configuration trimming
The chosen goldfish config is feature-rich; several optional components either reference headers that aren't populated or need a host compiler.
- **7 — FreeType** (`LV_USE_FREETYPE`): FreeType exports its include dir only to the **kernel** (`nuttx`) target, but `lv_freetype_private.h` compiles on the **apps** side, so `ft2build.h` was never on the include path. Disabled — LVGL keeps its built-in bitmap fonts (Montserrat).
- **8 — QuickJS** (`INTERPRETERS_QUICKJS`): builds `qjsc`, a *host* bytecode compiler, via a nested cmake that failed with `No CMAKE_C_COMPILER`. No native Windows host compiler exists on this machine (gcc/clang/cl/mingw all absent). QuickJS is the only host-tool build in this config, and VelaPaw doesn't use JS — disabled.
- **9 — libpng** (`LV_USE_LIBPNG`): `apps/external/libpng` has no `png.h` (source not populated). Disabled; LVGL's built-in `lodepng` remains for PNG.
- **10 — named semaphores** (`FS_NAMED_SEMAPHORES`): the link failed on `undefined reference to sem_open/sem_close` from `alsa-lib/pcm_dmix.c`. Enabling named semaphores provided the symbols.

**Config-change gotcha learned:** `kconfig-tweak` toggles a single symbol but does **not** fill *dependent* defaults. After enabling `FS_NAMED_SEMAPHORES`, the build failed on undeclared `CONFIG_FS_NAMED_SEMAPHORES_VFS_PATH`. The fix is to run `olddefconfig` (with the correct Kconfig env: `KCONFIG_CONFIG`, `APPSDIR`, `APPSBINDIR`, `BINDIR`, `srctree`, etc.) so dependent defaults materialize, then re-run `cmake -B build …` to regenerate `config.h`. **Never delete `config.h` manually** — it is a configure-step output, not a build rule (doing so yields `ninja: error: ... missing and no known rule to make it`).

---

## 3. Final configuration deltas

Relative to the stock `goldfish-armeabi-v7a-ap` defconfig:

| Symbol | Change | Reason |
|---|---|---|
| `CONFIG_LV_USE_FREETYPE` | off | ft2build.h not on apps include path |
| `CONFIG_LV_USE_LIBPNG` | off | png.h source not populated (lodepng remains) |
| `CONFIG_INTERPRETERS_QUICKJS` | off | host compiler required, none installed |
| `CONFIG_ALLSYMS` | off | post-link cxxfilt cannot run on Windows |
| `CONFIG_FS_NAMED_SEMAPHORES` | on | alsa-lib needs sem_open/sem_close |

## 4. Source patches (build glue only)

Three files in `nuttx/` were patched to make Windows codegen work. These are local build-glue fixes; if upstreamed they would help any Windows builder.

1. `nuttx/cmake/nuttx_multiple_link.cmake` — prefix `mkallsyms.py` with `${Python3_EXECUTABLE}`.
2. `nuttx/cmake/nuttx_add_romfs.cmake` — `romfs_etc` guard → plain copy; `genromfs/xxd/sed` via `bash -c`.
3. `nuttx/CMakeLists.txt` — `System.map` `nm|grep|sort` via `bash -c`.

## 5. One-time host setup

- Enabled Windows **Developer Mode** (for `repo` symlinks).
- `pip install kconfiglib pyelftools cxxfilt` into the CMake-selected Python; Python `Scripts` dir added to PATH.
- `git config --global user.name/user.email` set.

---

## 6. How to reproduce

```bash
# 1. Environment (toolchain on PATH + helpers)
source /d/openVela/vela-env.sh

# 2. Configure (out-of-tree CMake build for the goldfish ARM emulator)
cd /d/openVela/nuttx
cmake -B build -DBOARD_CONFIG="$VELA_BOARD_CONFIG" -GNinja     # or: vela_configure

# 3. Build
cmake --build build -j8
#   → build/nuttx, build/nuttx.bin, build/vela_ap.elf/.bin, build/System.map

# 4. Run in emulator (boots to NSH: openvela-ap>)
cd /d/openVela
export HOST_OS=windows HOST_ARCH=x86_64
./emulator.sh nuttx/build -no-window        # drop -no-window for the GUI/LCD
```

To change configuration:
```bash
kconfig-tweak --file build/.config -e SOME_SYMBOL    # or -d to disable
vela_olddefconfig                                    # fill dependent defaults
cmake -B build -DBOARD_CONFIG="$VELA_BOARD_CONFIG" -GNinja   # regenerate config.h
cmake --build build -j8
```

---

## 7. Verified result

```
NuttX 0.0.0 a6defdb4255 Jun 27 2026 03:03:24 arm armv7a
nsh: mount: mount failed: 19          # non-fatal (trimmed config)
adbd [9:100]
telnetd [10:100]

NuttShell (NSH)
openvela-ap>
```

Build artifacts (`nuttx/build/`): `nuttx` 26.7 MB ARM ELF, `nuttx.bin` 1.88 MB, `vela_ap.elf/.bin`, `nuttx.hex`, `System.map`, FAT `vela_data.bin`. The emulator also enumerated the host webcam (NV12), confirming a camera path is available for VelaPaw.

**Phase 0 acceptance: met.** The toolchain builds and runs openVela on this machine. Remaining non-fatal boot messages (`coredump … -2`, one `mount failed: 19`) stem from the trimmed config and do not block development.

---

## 8. Notes / risks carried forward

- **No native host compiler** on this machine. Components that build host-side tools will fail; install MinGW-w64 if ever needed. VelaPaw's core (LVGL, camera, TFLite-Micro) is all cross-compiled, so this is not expected to block the MVP.
- Source patches and config deltas live in the build tree, not in the team repo; they are documented here for reproducibility.

## 9. Next steps

1. **TFLite-Micro latency spike** — measure INT8 inference time + arena on this emulator (the project's biggest risk: `<300 ms` target on the S3's LX7).
2. **Scaffold VelaPaw** into the contest layout (`contest2026_043_CircuitForge/app/…` mapped via manifest `<linkfile>`).
3. **GUI run** — launch without `-no-window` to bring up LVGL on the virtual LCD.
