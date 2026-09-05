# `agent/` — VelaPaw's `ai_agent` layer

Everything VelaPaw adds on top of the stock openVela `ai_agent` framework.
The narrative write-up is in **[../docs/AI_AGENT.md](../docs/AI_AGENT.md)**
([中文](../docs/AI_AGENT_zh.md)); this file is the operational index.

```
agent/
├── skills/velapaw-feeding-digest.md   the custom Skill
├── HEARTBEAT.md                       the proactive task list
└── patches/                           changes to packages/ai_agent
```

---

## 1. Board assets (`skills/`, `HEARTBEAT.md`)

These two files are **runtime data on the board's littlefs**, not source that
the build consumes. At runtime they live at:

| Repo file | Board path |
|---|---|
| `skills/velapaw-feeding-digest.md` | `/data/ai_agent/skills/velapaw-feeding-digest.md` |
| `HEARTBEAT.md` | `/data/ai_agent/HEARTBEAT.md` |

> **They are not installed by the build or by the manifest linkfile.** The
> linkfile only makes them visible inside the openVela tree. Provisioning is a
> separate step — they must be written to `/data` once per board, over the NSH
> console or by mounting the littlefs image. They then persist across reboots
> and across firmware reflashes, because `write-flash 0x0 nuttx.bin` writes
> 0x0–~0x17ec80 and the `/data` partition starts at **0x180000**.

Two gotchas if you provision over the console: NSH truncates an input line at
~80 characters (`CONFIG_NSH_LINELEN`), so long `echo … >> /long/path` commands
are silently cut mid-string and leave a corrupt file; and verify by comparing
`ls -l`'s byte count against what you intended, not by reading the console echo
— the echo shows the *command*, not the file.

## 2. Framework patches (`patches/`)

Against `packages/ai_agent` at commit **`41723c6`**. The three patches touch
**disjoint file sets**, so each applies independently and in any order.

| Patch | Files | What it fixes |
|---|---|---|
| `0001-platform-bringup.patch` | 6 | Makes the agent run at all on a 16 MB ESP32-S3: PSRAM-stack freeze, the heap walk that kills a freshly-booted board, LLM/socket timeouts (420 s / 480 s), task stacks |
| `0002-request-size-and-transport.patch` | 4 | Makes it fast and reliable: excludes 20 tools irrelevant to a feeder (schemas were 9 946 B of a 19 391 B request, against a ~7 840 B usable IOB budget), plus HTTP retry and transport handling |
| `0003-agent-correctness.patch` | 8 | Makes the answers *right*: heartbeat session replay, the `list_dir` prefix and local-tool-shortcut defects, the stale-history freshness directive, `ask` truncating at 7 words, and the natural-language fast paths left dispatching to tools `0002` removed |

Applying them:

```bash
cd <openvela-workspace>/packages/ai_agent
git apply --check ../../contest2026_043_CircuitForge/agent/patches/*.patch  # dry run
git apply         ../../contest2026_043_CircuitForge/agent/patches/*.patch
```

Then force a rebuild of the C files — **header-only edits do not recompile
dependents in this tree**, which will silently give you a stale image:

```bash
find packages/ai_agent/src -name '*.c' -exec touch {} +
./build.sh esp32s3-devkit:waveshare_lcd -j4
```

`0001` also depends on three Kconfig symbols that are **not** in these patches
because they belong to the board config, not the framework — they are in
[`../board/configs/`](../board/configs/):
`XTENSA_IMEM_USE_SEPARATE_HEAP`, `XTENSA_IMEM_REGION_SIZE=0x48000`, and the
easily-missed `ARCH_HAVE_EXTRA_HEAPS`. Without all three the agent task's stack
lands in PSRAM and the first `ask` freezes the whole chip.

`0x30000` was the value that first stopped the freeze, but it is **not** enough
for the shipped build: every task and pthread stack is carved from this heap,
and with `ai_agent`'s task plus its 6 pthreads alongside velapaw's 48 KB stack
`pthread_create` came back `ENOMEM`. The flashed firmware uses `0x48000` — the
value in `board/configs/waveshare_lcd/defconfig`. `ARCH_HAVE_EXTRA_HEAPS` is a
`select`ed symbol, so it never appears in a defconfig; check it in `.config`.

## 3. Verifying a build really has the changes

This board has burned us twice with silently stale builds, so verify rather
than assume:

```bash
grep AI_AGENT nuttx/.config
strings nuttx/nuttx.bin | grep -i ai_agent
md5sum nuttx/nuttx.bin        # image size is NOT a usable identifier
```

At runtime, on the console:

- `[tools] Tools JSON built (16 builtin + 0 providers)` — the tool cut is in.

If you ever change the `s_excluded_tools[]` denylist in `0002`, re-run this
grep and require it to come back **empty** — cutting a tool leaves callers
behind, and `handle_nl_fast_path()` runs *before* the LLM, so a shortcut
pointing at an unregistered tool returns its result to the user as the final
answer:

```bash
grep -n 'get_heartrate\|get_steps\|get_wear_state\|vibrate\|music_\|feishu_\|quickapp' \
  packages/ai_agent/src/core/agent_loop.c
```
- `[skills] Skills summary: NNN bytes` — the real signal that skills loaded.
  **`Skills system ready (N built-in)` is a compile-time constant and proves
  nothing.**
- `END status=ok iters=N tools=M` — read this, not the prose. An answer that
  reads perfectly with `iters=1 tools=0` was replayed from history, not derived
  from the data files.
