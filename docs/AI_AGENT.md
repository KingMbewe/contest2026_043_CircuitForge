# VelaPaw × ai_agent — on-device agent integration

> Track: **AI 硬件产品创新** · Team **contest2026_043_CircuitForge**
> This document is the 交付完整的应用场景说明 required by the track guide:
> user story, feature list, and which `ai_agent` capabilities were used.

VelaPaw is an on-device multi-pet smart feeder (see the [main README](../README.md)).
This document covers the layer added on top of it: the openVela **`ai_agent`**
framework running on the same ESP32-S3, turning a feeder that *records* what
happened into one that **notices** and **speaks up**.

> **Scope — this is the only part of VelaPaw that needs an internet connection.**
> The feeder itself is fully self-contained: recognition, meal scheduling,
> portion and daily-limit control, body-condition scoring, the voice dialog and
> the touchscreen UI all run on-device and keep working with the network down.
> The agent layer is **additive and optional** — it talks to a hosted LLM, so it
> needs connectivity, but nothing in the feeding path depends on it. Pull the
> network and VelaPaw still recognizes pets and feeds them correctly; it simply
> stops offering the digest. This is a deliberate boundary, not a limitation we
> ran out of time to remove: the safety-critical path stays offline.

---

## 1. User story

> **Mei has two cats, Jay and Bob.** VelaPaw already feeds them correctly — it
> recognizes each cat by face and dispenses only that cat's ration. But Mei
> works long days, and the thing that actually matters to her isn't the log of
> what was dispensed. It's the question she can't answer from a log: *is either
> of them off their food?*
>
> A cat that stops eating is an early illness sign, and it is easy to miss,
> because "Bob ate 10 g today" means nothing without knowing Bob normally eats
> 30 g. Mei would have to open an app, read a table, remember the baseline, and
> do the arithmetic — every day, for each cat.
>
> With the agent on board, she doesn't. **The feeder tells her.** Twice an hour
> the device re-reads its own feeding records, compares each pet against that
> pet's own baseline and the owner's daily limit, and stays silent unless
> something is actually wrong. When something is wrong, it says so in plain
> language, with the numbers:
>
> > **Bob** — the one to watch today. ⚠️ Only **10 g in 1 meal** (33 % of his
> > 30 g/day baseline). His last meal was this morning at 09:24; no feeding
> > since. Watch whether he comes for a second meal this evening; monitor for
> > lethargy or hiding; if appetite stays low into tomorrow, consider a vet check.
>
> And when she does want to ask, she can — in her own words, on the device,
> without an app: `ask which of my pets should I be worried about today`.

**The problem it solves:** raw feeding telemetry is not health insight. A
per-pet baseline comparison is the difference between data a busy owner ignores
and a warning she acts on. The agent closes that gap on the device itself.

---

## 2. Feature list

### 2.1 Agent on the hardware (基础要求 §1)

| | |
|---|---|
| **Hardware** | Waveshare ESP32-S3-Touch-LCD-3.5-C — Xtensa LX7 dual-core, 8 MB PSRAM, 16 MB flash |
| **Firmware** | openVela + `ai_agent`, built as one image alongside VelaPaw's LVGL app (separate NSH builtins) |
| **LLM backend** | DeepSeek (`deepseek-chat`) over TLS 1.2 (`vela_tls` + mbedTLS) |
| **Interaction channel** | **CLI** — `ask <question>` at the `vela>` console |
| **Networking** | USB **CDC-NCM** to the host (board `192.168.137.2`), host-shared internet |
| **Persistence** | **littlefs** at flash `0x180000`, mounted `/data` — config, sessions, memory and skills all survive power loss |
| **Registered tools** | 16 built-in (`read_file`, `write_file`, `edit_file`, `list_dir`, `get_current_time`, `cron_add/list/remove`, `fetch_url`, `web_search`, `news_search`, `get_weather`, `analyze_image`, `run_shell`, `get_battery`, `get_screen_state`) — 4 706 B of tool JSON |

### 2.2 Custom Skill (基础要求 §2)

**`velapaw-feeding-digest`** — installed at `/data/ai_agent/skills/velapaw-feeding-digest.md`,
alongside the 10 built-in skills.

It teaches the agent VelaPaw's domain: where the feeding records live, what the
columns mean, and — critically — **what counts as a concern**:

```
4. Flag a concern ONLY if any of these is true:
   - appetite below 50 percent of baseline
   - grams today reached the owner daily limit
   - body condition is not ideal
   - no feed event today in the log
5. Otherwise say the pet is on track, one sentence.
6. Use only numbers from those files. Never invent.
```

The explicit thresholds are what make the output actionable rather than a
restatement of the log, and rule 6 is what keeps a language model from
inventing plausible-looking grams.

**The data bridge** is a deliberate design choice: VelaPaw's C application
writes its feeding and body-condition history to plain Markdown at
`/data/ai_agent/velapaw/status.md` and `feed_log.md`. The agent reads them with
the ordinary `read_file` tool — no custom tool, no IPC, no shared memory. The
app and the agent stay decoupled, and the same files are human-readable when
debugging.

### 2.3 Proactive + execution scenario (基础要求 §3)

**Type: 定时主动 (scheduled) with 阈值主动 (threshold) decision logic.**

The `ai_agent` **heartbeat** service — not `cron` — is the proactive path. Every
30 minutes it feeds `/data/ai_agent/HEARTBEAT.md` to the agent as a `system`
message:

```markdown
# Heartbeat
周期性检查任务（agent 定期执行）：
- [ ] Run the VelaPaw Feeding Digest skill.
If it finds a concern, notify the owner with numbers.
If all is on track, reply HEARTBEAT_OK only.
```

Each firing is a full agentic run: the agent loads the skill, calls `read_file`
on the two data files, applies the threshold rules, and either pushes a warning
or answers `HEARTBEAT_OK`. **Silence is the normal case** — the device speaks
only when a threshold is crossed, which is what makes the alert worth reading.

Captured unattended on hardware: `iters=4 tools=3`, four `read_file` calls,
request bodies growing 15 592 → 16 209 → 17 697 → 18 760 B as tool results
accumulated. Verified genuinely live, not replayed, by enrolling a new pet
mid-run and watching the next firing find it.

### 2.4 Bonus: memory across sessions (进阶要求 §2)

`session_mgr` persists every conversation to
`/data/ai_agent/sessions/tg_<chat_id>.jsonl` on littlefs, so context survives a
**power cut**, not merely a process restart — the constrained-storage case the
guide asks about. Chats are namespaced by `chat_id` (`console`, `heartbeat`),
so the CLI conversation and the proactive channel keep separate histories.

Cross-session memory cuts both ways, and handling that is part of the work: a
device whose *state changes between turns* must not answer today's question
from yesterday's reply. See §3.3.

### 2.5 Bonus: custom LVGL application (进阶要求 §3)

VelaPaw ships a complete touchscreen LVGL application — enrollment, schedules,
trends, bilingual EN/中文 UI — described in the [main README](../README.md). The
agent runs on the same device and reads the same data the UI displays.

---

## 3. Technical implementation — which `ai_agent` capabilities were used

| Capability | How VelaPaw uses it |
|---|---|
| **Agent loop (ReAct)** | Multi-round tool-calling; a real answer takes 2–4 iterations |
| **Tool system** | `read_file` / `list_dir` / `get_current_time` on the live data files |
| **Skills system** | One custom domain skill + the built-in set; 916 B skills summary in the system prompt |
| **Heartbeat service** | The proactive channel (30 min interval) |
| **Session manager** | Persistent per-`chat_id` history on littlefs |
| **Config store** | LLM backend + API key at `/data/ai_agent/config/config.json` |
| **Message bus** | `system:heartbeat` and `cli:console` dispatch |
| **`vela_tls`** | HTTPS to the LLM with connection pooling |
| **LLM cache** | RAM-only response cache, deliberately **bypassed** for the heartbeat chat |

### 3.1 Making the framework run on this board

Four fixes were needed before the agent would run at all on a 16 MB ESP32-S3:

1. **PSRAM stack freeze.** `ask` hard-froze the whole chip: the agent loop's
   pthread stack landed in PSRAM (`0x3c…`), and a littlefs read disables the
   cache that PSRAM is reached through. Fixed with three Kconfig symbols
   (`XTENSA_IMEM_USE_SEPARATE_HEAP`, `XTENSA_IMEM_REGION_SIZE=0x48000`,
   `ARCH_HAVE_EXTRA_HEAPS`). The stock 0x18000 stack cannot boot this app, and
   `0x30000` — enough to stop the freeze — still left `pthread_create` returning
   `ENOMEM` once ai_agent's 6 pthreads shared the heap with velapaw.
2. **Heap walk kills the board.** `mallinfo()` as the agent task's first
   statement hangs holding the heap lock on a freshly-booted board. Replaced
   with a non-walking status report.
3. **IOB exhaustion.** A 15 KB request against the default IOB pool stalled with
   no timeout. `IOB_NBUFFERS`/`IOB_NCHAINS=64`, `THROTTLE=24`.
4. **LLM timeouts.** A good 62.6 s reply was being discarded by a 60 s watchdog
   (`AGENT_LLM_TIMEOUT_SEC`, now 420 s; socket 480 s).

### 3.2 Making it fast and reliable

**Cutting 20 unused tools was the single highest-impact change.** Every
registered tool's JSON schema is serialized into *every* request, and a pet
feeder has no use for wearable sensors, a music player, Feishu documents or a
quickapp launcher. Measured on hardware: **9 946 B of a 19 391 B request body
was tool schemas**, against an IOB pool of `NBUFFERS(64) × BUFSIZE(196)` =
12 544 B — of which, after `THROTTLE(24)`, only ~7 840 B is usable by a sender.
A single request was ~2.5× the pool and could complete only by blocking and
draining as ACKs recycled buffers.

**Shrinking the request is the fix; a send timeout is not** — a timeout turns
the load-bearing wait into a hard failure. (We tried the timeout first. It made
things worse, and that image is marked as a regression in our build lineage.)
Trimming the registry to the 16 tools this product actually needs took
reliability from intermittent to **8/8** and latency from **150–300 s to
3.6–35.6 s**.

A related non-finding, recorded because it cost real time: the "1-in-3
zero-byte HTTP failure" was **not a bug** — an idle keep-alive close that
`vela_tls` recovers from in-band, 3/3.

### 3.3 Making the answers *correct* — the freshness problem

The hardest class of bug here was not a crash. It was the agent returning a
**confident, well-formed, wrong** answer. Three distinct defects, all found by
reading traces rather than trusting the prose:

1. **Heartbeat replayed its own history.** Because sessions persist across
   reboot, firing *N* could answer from firing *N−1*'s text without opening a
   single file — `iters=1 tools=0` and a plausible digest. As a product bug: a
   pet fed at 14:00 still reported unfed at 14:30. Fixed by skipping history for
   the heartbeat chat.
2. **`list_dir` returned nothing, and that nothing became the answer.** The
   prefix was matched against the absolute path while the model passes a
   relative one, so every entry was filtered out; and `list_dir` sat in the
   "local tool shortcut" set, which ends the turn and hands raw tool output
   straight to the user. An owner asking which pet to worry about literally
   received `(no files found)`. Fixed by matching **either** path form, always
   recursing, and removing `list_dir` from the shortcut set. Across three
   hardware runs the model spelled the argument three different ways
   (`"velapaw"`, `"/data/ai_agent/"`, omitted) — a tool that handles only the
   spelling you happened to observe will look fixed and then fail.
3. **Stale conversation history vs. changing device state.** The console
   *should* keep history, so it can't simply be cleared. Instead
   `build_messages` now injects a mid-array `system` message ahead of the user
   turn stating that device state may have changed, that any figure quoted
   earlier is stale, and that current-state questions must be answered by
   re-reading the files. Trimming history was the fallback and proved
   unnecessary.

Also fixed along the way: `ask` silently truncated questions at seven words
(`MAX_ARGS 8` in `nsh_commands.c`, raised to 32), and the agent no longer takes
the shortcut when tool output begins with `Error: `.

### 3.4 Verification

Every claim above was verified from the agent's own trace lines, not from the
readability of its prose. The final console run, with stale history deliberately
left in place:

```
tools executed : 3 ['list_dir', 'read_file', 'read_file']
    /data/ai_agent/velapaw/status.md (476 B)
    /data/ai_agent/velapaw/feed_log.md (241 B)
req bytes      : 11312, 12667, 14008
cache replay   : no
END status=ok iters=3 tools=2 llm_ms=5260 elapsed=7s
```

— then the correct Bob warning quoted in §1. `cache replay: no` proves the reply
came off the wire; the two `read_file` sizes prove the numbers came from the
files.

**Two techniques worth reusing:**

- **An `llm_cache` hit is a free byte-for-byte equality test between two runs.**
  The cache keys on the first 64 characters, so a hit proves two runs delivered
  identical content — that is what proved `ask` truncation was a fixed buffer
  rather than a serial-link problem.
- **`Skills system ready (N built-in)` is a compile-time constant** and proves
  nothing about whether a skill loaded. `Skills summary: NNN bytes` is the real
  signal.

---

## 4. Where the code lives

| Component | Location |
|---|---|
| Custom Skill | [`agent/skills/velapaw-feeding-digest.md`](../agent/skills/velapaw-feeding-digest.md) → board `/data/ai_agent/skills/` |
| Heartbeat task | [`agent/HEARTBEAT.md`](../agent/HEARTBEAT.md) → board `/data/ai_agent/HEARTBEAT.md` |
| Framework changes | [`agent/patches/`](../agent/patches/) — three patches against `packages/ai_agent` @ `41723c6` |
| How to apply / verify | [`agent/README.md`](../agent/README.md) |
| Data bridge | VelaPaw `store` module → `/data/ai_agent/velapaw/{status,feed_log}.md` |
| Board config | [`board/configs/`](../board/configs/) |

---

*Team CircuitForge · 2026 openVela AI Hardware Developer Contest.*
