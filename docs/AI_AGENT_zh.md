# VelaPaw × ai_agent — 端侧 Agent 集成说明

> 赛道：**AI 硬件产品创新** · 队伍 **contest2026_043_CircuitForge**
> 本文即赛题要求的**完整应用场景说明**：用户故事、功能清单、技术实现。

VelaPaw 是一台端侧多宠物智能喂食器（见[主 README](../README_zh.md)）。本文说明在它之上增加的一层能力：openVela **`ai_agent`** 框架运行在同一颗 ESP32-S3 上，让一台只会**记录**发生了什么的喂食器，变成一台会**发现问题**并**主动开口**的设备。

> **范围说明 —— 这是 VelaPaw 中唯一需要联网的部分。**
> 喂食器本体完全自包含：宠物识别、定时喂食、份量与每日上限控制、体况评分、语音对话、触摸屏 UI，全部在端侧运行，断网照常工作。Agent 层是**可选的增量能力**——它需要访问云端大模型，因此需要网络，但**喂食主链路不依赖它**。拔掉网络，VelaPaw 依然能识别宠物并正确投喂，只是不再提供健康简报。这是一条**刻意划出的边界**，而不是没来得及去掉的限制：安全攸关的链路始终保持离线。

---

## 一、用户故事

> **小美养了两只猫，Jay 和 Bob。** VelaPaw 已经能正确喂食——刷脸识别每只猫，只投放属于它的那一份口粮。但小美白天要上班，她真正关心的并不是"投喂了多少克"这条日志，而是日志回答不了的那个问题：**它们俩有没有哪只不好好吃饭了？**
>
> 猫不吃东西是生病的早期信号，而且极易被忽略——因为"Bob 今天吃了 10 克"这句话本身没有意义，除非你还记得 Bob 平时一天吃 30 克。要自己判断，小美得打开 App、翻表格、回忆基线、再做一次心算，每天、每只猫都来一遍。
>
> 有了端侧 Agent，她不用了。**是喂食器主动告诉她。** 设备每 30 分钟重新读一次自己的喂食记录，把每只宠物和**它自己的**基线、以及主人设定的每日上限做对比，没问题就保持沉默；一旦越线，就用大白话把数字讲清楚：
>
> > **Bob** —— 今天需要留意。⚠️ 全天只吃了 **10 克、1 顿**，是它 30 克/天基线的 **33%**。最后一次进食是今早 09:24，之后再没有记录。请观察它傍晚会不会来吃第二顿；留意是否有精神萎靡、躲藏等表现；如果明天食欲仍然偏低，建议就医检查。
>
> 而当她想主动问的时候，也可以——用自己的话，在设备上问，不需要 App：
> `ask which of my pets should I be worried about today`

**它解决的问题：** 原始喂食数据不等于健康洞察。"与该宠物自身基线对比"这一步，正是"主人会忽略的数据"和"主人会采取行动的预警"之间的分水岭。Agent 把这一步放在了设备本地完成。

---

## 二、功能清单

### 2.1 让 Agent 在硬件上跑起来（基础要求 §1）

| | |
|---|---|
| **硬件** | Waveshare ESP32-S3-Touch-LCD-3.5-C —— Xtensa LX7 双核、8 MB PSRAM、16 MB Flash |
| **固件** | openVela + `ai_agent`，与 VelaPaw 的 LVGL 应用编译进同一镜像（两个独立 NSH builtin） |
| **LLM 后端** | DeepSeek（`deepseek-chat`），TLS 1.2（`vela_tls` + mbedTLS） |
| **交互渠道** | **CLI** —— 在 `vela>` 控制台执行 `ask <问题>` |
| **网络** | USB **CDC-NCM** 连接主机（板端 `192.168.137.2`），经主机共享上网 |
| **持久化** | **littlefs**（Flash `0x180000`，挂载于 `/data`）—— 配置、会话、记忆、Skill 全部掉电不丢 |
| **已注册工具** | 16 个内置工具（`read_file`、`write_file`、`edit_file`、`list_dir`、`get_current_time`、`cron_add/list/remove`、`fetch_url`、`web_search`、`news_search`、`get_weather`、`analyze_image`、`run_shell`、`get_battery`、`get_screen_state`），工具 JSON 共 4 706 字节 |

### 2.2 自定义 Skill（基础要求 §2）

**`velapaw-feeding-digest`** —— 安装在 `/data/ai_agent/skills/velapaw-feeding-digest.md`，与 10 个内置 Skill 并列。

它把 VelaPaw 的领域知识教给 Agent：喂食记录在哪、每个字段是什么意思，以及最关键的——**什么才算"有问题"**：

```
4. Flag a concern ONLY if any of these is true:
   - appetite below 50 percent of baseline
   - grams today reached the owner daily limit
   - body condition is not ideal
   - no feed event today in the log
5. Otherwise say the pet is on track, one sentence.
6. Use only numbers from those files. Never invent.
```

写死的阈值，是"可执行的提醒"与"把日志复述一遍"的区别所在；而第 6 条，是防止大模型编造出看起来很合理的克数的关键。

**数据桥接**是一个刻意的设计选择：VelaPaw 的 C 应用把喂食与体况历史写成纯 Markdown，落在 `/data/ai_agent/velapaw/status.md` 和 `feed_log.md`；Agent 用最普通的 `read_file` 工具去读。**不需要自定义工具、不需要 IPC、不需要共享内存**——应用与 Agent 保持解耦，而且这两个文件在调试时人眼可读。

### 2.3 「主动 + 执行」场景（基础要求 §3）

**类型：定时主动，并内置阈值主动的判定逻辑。**

真正的主动通路是 `ai_agent` 的 **heartbeat** 服务（不是 cron）。它每 30 分钟把 `/data/ai_agent/HEARTBEAT.md` 作为 `system` 消息投给 Agent：

```markdown
# Heartbeat
周期性检查任务（agent 定期执行）：
- [ ] Run the VelaPaw Feeding Digest skill.
If it finds a concern, notify the owner with numbers.
If all is on track, reply HEARTBEAT_OK only.
```

每一次触发都是一次完整的 agentic 运行：加载 Skill → `read_file` 读两个数据文件 → 套用阈值规则 → 要么推送预警，要么回一句 `HEARTBEAT_OK`。**沉默才是常态**——设备只在越过阈值时开口，这正是这条提醒值得被认真看待的原因。

硬件无人值守实测：`iters=4 tools=3`，4 次 `read_file` 调用，请求体随工具结果累积从 15 592 → 16 209 → 17 697 → 18 760 字节增长。为验证"每次触发都是真的调了模型、而不是回放缓存"，我们在运行中途新注册了一只宠物，下一次触发确实发现了它。

### 2.4 加分项：跨会话记忆（进阶要求 §2）

`session_mgr` 把每一路对话持久化到 littlefs 上的 `/data/ai_agent/sessions/tg_<chat_id>.jsonl`，因此上下文能扛过**掉电**，而不只是进程重启——正好对应赛题所说的"端侧存储受限"场景。会话按 `chat_id` 分命名空间（`console` / `heartbeat`），CLI 对话与主动推送各自保留独立历史。

跨会话记忆是把双刃剑，处理好它本身就是工作量的一部分：一台**状态会随时间变化**的设备，绝不能用昨天的回答来答今天的问题。见 §3.3。

### 2.5 加分项：独立 LVGL 应用（进阶要求 §3）

VelaPaw 本身就是一个完整的触摸屏 LVGL 应用——宠物注册、喂食计划、趋势图表、中英双语 UI，详见[主 README](../README_zh.md)。Agent 与它运行在同一台设备上，读取 UI 所展示的同一份数据。

---

## 三、技术实现 —— 用到了 `ai_agent` 的哪些核心能力

| 能力 | VelaPaw 的用法 |
|---|---|
| **Agent 循环（ReAct）** | 多轮工具调用；一次真实回答需要 2–4 轮迭代 |
| **工具系统** | 对实时数据文件调用 `read_file` / `list_dir` / `get_current_time` |
| **Skills 系统** | 1 个自定义领域 Skill + 内置 Skill 集；系统提示中 Skill 摘要 916 字节 |
| **Heartbeat 服务** | 主动推送通路（30 分钟周期） |
| **会话管理器** | 按 `chat_id` 持久化到 littlefs |
| **配置存储** | LLM 后端与 API Key 存于 `/data/ai_agent/config/config.json` |
| **消息总线** | `system:heartbeat` 与 `cli:console` 的分发 |
| **`vela_tls`** | 到 LLM 的 HTTPS，带连接池复用 |
| **LLM 缓存** | 纯 RAM 响应缓存；对 heartbeat 会话**刻意绕过** |

### 3.1 先让框架在这块板子上跑起来

在 16 MB 的 ESP32-S3 上，Agent 能跑起来之前必须先解决四个问题：

1. **PSRAM 栈导致整机死机。** `ask` 会让整颗芯片硬冻：Agent 循环的 pthread 栈落在了 PSRAM（`0x3c…`），而一次 littlefs 读会关闭 PSRAM 所依赖的 cache。修复方式是三个 Kconfig 符号（`XTENSA_IMEM_USE_SEPARATE_HEAP`、`XTENSA_IMEM_REGION_SIZE=0x48000`、`ARCH_HAVE_EXTRA_HEAPS`）。默认的 0x18000 栈根本带不动这个应用；`0x30000` 虽然能消除死机，但在 ai_agent 的 6 个 pthread 与 velapaw 共用该堆之后，`pthread_create` 仍会返回 `ENOMEM`。
2. **遍历堆会把板子搞死。** 把 `mallinfo()` 放在 Agent 任务的第一条语句，会在刚启动的板子上持锁挂死。改为不遍历的状态上报。
3. **IOB 耗尽。** 15 KB 的请求在默认 IOB 池下会无超时地卡死。调整为 `IOB_NBUFFERS`/`IOB_NCHAINS=64`、`THROTTLE=24`。
4. **LLM 超时。** 一次耗时 62.6 秒的**正常**回复被 60 秒看门狗丢弃（`AGENT_LLM_TIMEOUT_SEC`，现为 420 秒；socket 480 秒）。

### 3.2 再让它变快、变稳

**裁掉 20 个用不到的工具，是收益最大的一处改动。** 每一个已注册工具的 JSON schema 都会被序列化进**每一次**请求，而一台喂食器根本用不到可穿戴传感器、音乐播放器、飞书文档或快应用启动器。真机实测：**19 391 字节的请求体中，有 9 946 字节是工具 schema**；而 IOB 池只有 `NBUFFERS(64) × BUFSIZE(196)` = 12 544 字节，扣除 `THROTTLE(24)` 后发送方实际可用约 7 840 字节。也就是说，单次请求约为可用池的 **2.5 倍**，只能靠阻塞等待 ACK 回收缓冲区来一点点排空。

**正确的解法是把请求变小，而不是加发送超时**——加超时只会把这个"本来就该等"的过程变成硬失败。（我们确实先试了超时方案，结果更糟，那版镜像在我们的构建谱系里被标记为 regression。）把工具表精简到本产品真正需要的 16 个之后，可靠性从"时好时坏"变成 **8/8**，延迟从 **150–300 秒降到 3.6–35.6 秒**。

一个值得记录的**非问题**（因为它实实在在浪费了时间）：所谓"三次里有一次 HTTP 返回 0 字节"并不是 bug——那是 keep-alive 空闲关闭，`vela_tls` 会在带内自动恢复，3/3 全部成功。

### 3.3 最后，让答案**正确** —— 新鲜度问题

这里最难的一类 bug 不是崩溃，而是 Agent 给出**自信、通顺、但错误**的回答。共发现三个独立缺陷，全部靠读 trace 定位，而不是靠"读起来对不对"：

1. **Heartbeat 回放了自己的历史。** 由于会话跨重启持久化，第 *N* 次触发可能直接照抄第 *N−1* 次的文本，一个文件都不打开——表现为 `iters=1 tools=0` 却给出一份看似合理的简报。落到产品上就是：14:00 已经喂过的猫，14:30 的检查仍报告"未进食"。修复方式是对 heartbeat 会话跳过历史注入。
2. **`list_dir` 返回空，而这个"空"被当成答案交给了用户。** 前缀被拿去和**绝对路径**比对，而模型传的是**相对路径**，于是所有条目都被过滤掉；同时 `list_dir` 又恰好在"本地工具短路"名单里——该机制会直接结束本轮并把工具原始输出丢给用户。结果是：主人问"哪只宠物需要担心"，收到的回答字面就是 `(no files found)`。修复方式是**两种路径形式都接受**、始终递归、并把 `list_dir` 移出短路名单。三次硬件实测中，模型把这个参数写成了三种形式（`"velapaw"`、`"/data/ai_agent/"`、干脆不传）——**只处理你恰好观察到的那一种写法的工具，会看起来已经修好，然后再次失败。**
3. **陈旧对话历史 vs. 变化的设备状态。** 控制台**应该**保留历史，所以不能简单清空。改为在 `build_messages` 中、用户消息之前注入一条数组中部的 `system` 消息，声明设备状态可能已改变、此前提到的任何数字都已过期、涉及当前状态的问题必须重新读取文件后再回答。裁剪历史长度是备选方案，实测证明并不需要。

顺带修复：`ask` 会在第 7 个词处静默截断（`nsh_commands.c` 中的 `MAX_ARGS 8`，已提到 32）；以及工具输出以 `Error: ` 开头时不再走短路。

### 3.4 验证

上述每一条结论都来自 Agent 自己的 trace 行，而不是它措辞的可信度。下面是**刻意保留陈旧历史**的最终控制台实测：

```
tools executed : 3 ['list_dir', 'read_file', 'read_file']
    /data/ai_agent/velapaw/status.md (476 B)
    /data/ai_agent/velapaw/feed_log.md (241 B)
req bytes      : 11312, 12667, 14008
cache replay   : no
END status=ok iters=3 tools=2 llm_ms=5260 elapsed=7s
```

随后即是 §1 中引用的那段针对 Bob 的正确预警。`cache replay: no` 证明回复来自网络而非缓存；两次 `read_file` 的字节数证明数字来自文件。

**两个值得复用的手法：**

- **`llm_cache` 命中，等于一次免费的逐字节相等性检验。** 缓存以前 64 个字符为键，命中即证明两次运行送出的内容完全一致——正是它证明了 `ask` 的截断是固定缓冲区问题，而非串口链路问题。
- **`Skills system ready (N built-in)` 是编译期常量**，它完全无法证明 Skill 被加载了。`Skills summary: NNN bytes` 才是真正的信号。

---

## 四、代码位置

| 组成部分 | 位置 |
|---|---|
| 自定义 Skill | [`agent/skills/velapaw-feeding-digest.md`](../agent/skills/velapaw-feeding-digest.md) → 板端 `/data/ai_agent/skills/` |
| Heartbeat 任务 | [`agent/HEARTBEAT.md`](../agent/HEARTBEAT.md) → 板端 `/data/ai_agent/HEARTBEAT.md` |
| 框架改动 | [`agent/patches/`](../agent/patches/) —— 针对 `packages/ai_agent` @ `41723c6` 的三个补丁 |
| 应用与验证方法 | [`agent/README.md`](../agent/README.md) |
| 数据桥接 | VelaPaw `store` 模块 → `/data/ai_agent/velapaw/{status,feed_log}.md` |
| 板级配置 | [`board/configs/`](../board/configs/) |

---

*CircuitForge 队 · 2026 openVela AI 硬件开发者大赛*
