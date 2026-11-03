<a href="README.md">English</a> | <b>中文</b>

# VelaPaw —— 端侧 AI 多宠智能喂食器

**队伍：** CircuitForge（`contest2026_043`） · **赛道：** AI 硬件产品创新
**平台：** openVela（NuttX），运行于 Waveshare **ESP32-S3-Touch-LCD-3.5-C**
**代码仓库：** [github.com/open-vela/contest2026_043_CircuitForge](https://github.com/open-vela/contest2026_043_CircuitForge) —— 源码已提交至大赛专属分支 **`dev-ai-contest-2026`**

---

## 演示视频

https://github.com/user-attachments/assets/7cf3a67b-49a3-45f4-a800-5e8f92b570e9




▶ 如果上方的嵌入视频无法加载，可以[直接下载](VelaPaw_Demo.mp4)（93MB，MP4）。建议您下载视频以获得更好的画质。

---

## 决赛更新 —— 初赛之后新增的内容

以下内容均在初赛提交（2026-09-06）之后加入，并已在真机上验证。详细说明见决赛版技术报告第 3.7.1 节（[中文](docs/VelaPaw_技术报告-决赛版.pdf) · [English](docs/VelaPaw_Technical_Report-Finals.pdf)）。

- **Agent 修改饭点** —— 新增一个工具 `set_meal_schedule`：输入一句话（如 `ask change bob's dinner to 18:20`）即可修改时间并存入闪存，约 5 秒。
- **Agent 上屏** —— 每次 `ask` 提问及其回答以提示条显示在铃铛下方；心跳告警显示红色提示条并使铃铛变红；我的宠物页实时刷新。
- **食欲按当天计划评分** —— 已出粮克数 ÷ 截至此刻应出粮克数，低于 50% 才告警；基线只统计有出粮的日子。按计划出粮满一天现在显示 100%（以前为 65%），拔电也不再误报。
- **稳定性** —— 识别自动重新就绪，控制台打印不再阻塞，`fed.bin` 保证 RESET 前后至多出粮一次，软件卡死监测写入 `health.log` 后复位整板，时间丢失后从下一天 08:00 开始。
- **USB 网络修复** —— 消除了导致 `ask` 卡住的 USB FIFO 重叠（`ESP32S3_FIFO_SIZE` 1280 → 800，另有 OTG 与 CDC-NCM 修复）：连续三次 `ask` 分别用时 9 / 4 / 5 秒，ping 190/190。
- **麦克风修复** —— 音频缓冲按 64 字节对齐，消除了间歇性采集卡死：连续 3 次开机语音均正常。

**固件：** 仓库根目录下的 [`nuttx.bin`](nuttx.bin) 是决赛版固件（1 565 456 B，md5 `0793ff78a1e4933e6c194d2b2b36d02c`），即演示板上运行的固件；烧录方法见[第七节](#七编译与运行真机)第 5 步。决赛版源码也已在本仓库中：应用位于 [`app/velapaw/`](app/velapaw/)，对 openVela 各代码树的改动以补丁形式放在 [`board/patches/`](board/patches/README.md#3-finals-image) 与 [`agent/patches/`](agent/README.md)，按[第七节](#七编译与运行真机)即可编译。USB 与麦克风两项修复尚未提交上游。

---

## 一、作品简介

**VelaPaw 是一款智能宠物喂食器：它能识别出正站在食盆前的是*哪一只*宠物，并只投喂这只宠物当前应吃的那一餐——而所有 AI 全部在设备端本地运行。无需云端、无需手机、无需账号。**

在多宠家庭里，普通的定时喂食器分不清是哪只猫，结果往往是最贪吃的那只把大家的粮都吃了，定量管理根本无从谈起。VelaPaw 用和人一样的方式解决它：先"看"一眼动物、认出是谁，再只投喂它自己的那份口粮，并按它自己的时间表。由于摄像头、神经网络、时钟和投料机构全部自成一体，它在断网时照常工作，也绝不会把家里的画面上传到任何地方。

**核心亮点**

- 🐾 **逐只宠物识别** —— 端侧 INT8 神经网络凭面部识别每一只已登记的宠物，无需项圈或标签。
- 🍽️ **识别门控投喂** —— 只有当"识别到正确的宠物在食盆前"**且**"它的某一餐已到时间且未被跳过"时才投喂，且每餐每天至多一次。每一餐都可单独设置**跳过（skip-meal）**开关。
- 🗣️ **语音设置时间表** —— 投喂时间也可以直接说出来：由端侧关键词识别模型驱动的 MEAL→HOUR→CONFIRM 语音对话，可免触摸设置某只宠物的投喂时间。**没有唤醒词** —— 需先在编辑宠物页面点击开始，再用语音依次回答。VelaPaw 不含常驻语音唤醒功能，因此大赛"参赛须知与规则"中关于统一唤醒词的要求不适用于本项目。
- ⚖️ **份量与每日上限** —— 每只宠物可设定克数份量，并有每日投喂硬上限。
- 🩺 **健康监测** —— 第二个端侧网络估计体况（偏瘦 / 理想 / 偏胖），进食历史分析还会发出**食欲骤降告警**，这是疾病的早期信号。
- 🤖 **会主动开口的端侧 AI Agent** —— 板上运行 openVela `ai_agent` 框架，并加载自定义 Skill 读取*真实*的喂食记录。它按定时**自行推送**提醒：当某只宠物越过预警阈值时自动触发，无人值守、无需有人守在控制台前。设备不出声，就说明一切正常。初赛之后，它还能根据一句输入修改宠物的饭点，回答与告警也会显示在触摸屏上。详见[第四节](#四端侧-ai-agent唯一需要联网的部分)。
- 🕑 **真实计时** —— 硬件实时时钟（RTC）让投喂时间表在断电后依然准确。
- 🌐 **中英双语界面** —— 支持中文/英文，应用内可实时切换语言。
- 💾 **数据持久化** —— 已登记的宠物、时间表与宠物头像都能在断电后保留。
- 🖥️ **即开即用的家电体验** —— 上电直接进入触摸屏界面，无需连接串口控制台。

**为什么是现在：** 2026 年 3 月，小米推出了米家智能宠物喂食器 2 视觉版——一款面向大众市场的量产产品（众筹价 ¥449，零售价 ¥549），专门加入摄像头把 AI 视觉带进喂食场景。这等于是最懂宠物主人会为什么买单的公司，用真金白银的量产产品独立验证了 VelaPaw 立项时的核心判断——摄像头理应长在喂食器上。VelaPaw 在此基础上更进一步：从"看到食盆"到"认出是哪只宠物"，凭面部识别、完全端侧运行，无需 App、账号或云端。完整对比见项目介绍文档[§06 市场与价值](VelaPaw_Project_Intro_zh.pdf)。

---

## 二、端侧 AI（项目的核心）

识别采用的是**开集度量学习（open-set metric learning）**，而非固定类别的分类器。模型是一个通用的宠物特征提取器（MobileNetV3-Small 主干 + 空间注意力 TSFM 模块），它把一帧摄像头画面转换成一个经 L2 归一化的 128 维**嵌入向量**。登记一只宠物即保存其若干张照片的平均嵌入；识别就是用余弦相似度与已登记宠物做匹配。正因如此，**新增一只宠物无需重新训练**——登记本身就是一等公民的功能，识别质量取决于登记照片的质量，而不是被冻结的类别集合。

| | |
|---|---|
| **身份识别模型** | MobileNetV3-Small + TSFM 注意力，INT8，128 维嵌入，128×128 输入 |
| **体况评估模型** | 三分类（偏瘦/理想/偏胖）INT8 CNN |
| **推理运行时** | TensorFlow Lite for Microcontrollers（`apps/mlearning/tflite-micro`） |
| **加速** | 在 Xtensa LX7 上使用 ESP-NN SIMD 算子 → **约 1.3 秒/次推理**（参考算子约 6.75 秒） |
| **模型存放** | 运行时从闪存加载（1.4 MB 身份模型 @ `0x600000`，626 KB 体况模型 @ `0x760000`）——因体积过大无法内存映射，开机时读入 PSRAM |

两个模型均在设备外训练，训练/导出流程见 [`host/`](host/)（TensorFlow/Keras → INT8 训练后量化 → `.tflite`）。

---

## 三、工作原理 —— 从摄像头到猫粮

```
   摄像头画面 ──► 嵌入向量（TFLM，INT8）──► 与已登记宠物做余弦匹配
                                                    │
                                          识别出的宠物 + 分数
                                                    │
                                    这只宠物今天是否有某一餐已到时间且尚未投喂？
                                               │是             │否
                                          投喂 N 克         提示"下一餐 18:00"
                                   （28BYJ-48 步进电机驱动叶轮）
                                               │
                                    记入历史 · 更新趋势 · 体况检测
                                               │
                            littlefs（/data）上的 status.md / feed_log.md
                                               │
                                  由端侧 ai_agent 读取 —— 见第四节
```

中间的那道"门控"正是本作品的核心：只靠识别，会把贪吃的宠物喂上一整天；只靠定时，又会喂到碰巧站在旁边的那只。**两者必须同时满足。**

**软件架构**（清晰的 HAL 分层，位于 [`app/velapaw/`](app/velapaw/)）：

- `ui/` —— LVGL 触摸屏界面（登记 · 识别 · 我的宠物 · 趋势）
- `infer/` —— 推理接口 + TFLite-Micro 后端 + 训练好的模型
- `identity/` —— 登记存储、余弦匹配器、逐宠物闪存持久化、宠物头像
- `store/` —— 进食历史与行为分析；同时写出供 Agent 读取的 Markdown 记录
- `voice/` —— 关键词识别 + MEAL→HOUR→CONFIRM 语音排餐对话
- `hal/` —— 摄像头（OV5640）与投料机构（28BYJ-48 步进电机）的硬件抽象

**Agent 层**（[`agent/`](agent/)）—— 自定义 Skill、心跳任务清单，以及针对 `packages/ai_agent` 的四个补丁。它位于成型投喂器*之上*：只读取 `store/` 写出的记录，从不参与任何投喂决策。决赛版中它还能通过 `set_meal_schedule` 工具修改饭点，作用与在触摸屏上编辑相同；喂食器仍只在识别到该宠物站在食盆前时才投出这一餐。详见[第四节](#四端侧-ai-agent唯一需要联网的部分)。

**板级支持**（[`board/`](board/)）—— 自定义板文件驱动显示屏、通过 ESP32-S3 LCD_CAM 外设驱动 OV5640 摄像头、通过 I²C 驱动 PCF85063 RTC，并经 ULN2003 用四个 GPIO 驱动投料的 **28BYJ-48 步进电机**（IN1–IN3/IN4 用 GPIO9/10/11/**43**）。显示屏运行于 **40 MHz 硬件 SPI + DMA**。ES8311 音频编解码器独占的 I²S 总线（GPIO12–16，未接到任何排针）驱动板载麦克风与扬声器，用于语音设置时间表。

---

## 四、端侧 AI Agent（唯一需要联网的部分）

openVela **`ai_agent`** 框架运行在同一颗 ESP32-S3 上，作为叠加在已完成的喂食器**之上**的一层。它让 VelaPaw 多了一种有用的方式：不必主人走到屏幕前查看，设备会自己读取喂食记录，在情况不对时主动开口。

**设备上实际运行的内容**

| | |
|---|---|
| **自定义 Skill** | 位于 `/data/ai_agent/skills/` 的喂食简报 Skill —— 读取 VelaPaw 每次投喂时写入闪存的喂食记录，生成逐只宠物的摘要：克数、餐次，以及相对该宠物自身基线的食欲变化。 |
| **CLI 提问** | `ask how much did bob eat today` —— Agent 从设备闪存中读取实时记录并据此作答，而不是返回预设的固定回复。 |
| **主动推送** | 心跳任务按固定间隔重新读取记录，**仅在某只宠物越过预警阈值时**才推送提醒 —— 设备不出声，就说明一切正常。它无人值守自动触发，无需有人守在控制台前。（演示固件使用 3 分钟的短间隔，以便在一次演示中就能看到推送触发；量产产品会采用小时级间隔。） |
| **修改饭点**（决赛版） | `ask change bob's dinner to 18:20` —— Agent 调用 `set_meal_schedule` 工具，修改这一餐的时间并存入闪存。仍然只有识别到该宠物在食盆前、且这一餐已到时间时才会出粮。 |
| **屏幕显示**（决赛版） | 每次 `ask` 提问及其回答以提示条显示在铃铛下方；心跳告警显示红色提示条并使铃铛变红，主人无需控制台也能看到。 |

**离线与联网 —— 边界在哪里**

> **喂食器本体从不需要联网。** 识别、投喂时间表、份量与每日上限控制、体况评估、进食历史与趋势、语音对话以及整个界面，全部在 ESP32-S3 端侧运行，不依赖任何网络。拔掉网络，VelaPaw 照常识别宠物并按时投喂，与之前完全一样。
>
> **Agent 层是唯一需要联网的部分**，因为它要通过设备的 USB 网络接口（CDC-NCM）访问云端大模型。断网时，`ask` 与主动推送会停止 —— 而其他一切都不会。任何投喂决策都不会在云端做出，也没有任何图像离开设备。

完整说明 —— 架构、Skill、框架补丁，以及已捕获的主动推送记录：**[docs/AI_AGENT_zh.md](docs/AI_AGENT_zh.md)**（[English](docs/AI_AGENT.md)）。设备端源码与补丁位于 [`agent/`](agent/)。

---

## 五、硬件

- **Waveshare ESP32-S3-Touch-LCD-3.5-C** —— ESP32-S3，8 MB PSRAM，16 MB 闪存，320×480 ST7796 触摸屏，OV5640 摄像头，PCF85063 RTC。

> **为什么选这块板——同一颗 SoC，更合适的外设。** 这块板搭载的是与大赛提供的 ESP32-S3-EYE **完全相同的 ESP32-S3**，因此 openVela 移植、端侧推理和所有驱动都是在同一颗芯片上的工作。VelaPaw 额外需要的是一块**电容触摸屏**——登记宠物、编辑投喂时间、查看趋势都在屏上完成——以及一个**实时时钟**，让投喂时间表在断电后依然有效。ESP32-S3-EYE 是一块面向视觉/语音的开发板，屏幕小且不支持触摸，也没有 RTC；而 Waveshare Touch-LCD-3.5-C 把产品所依赖的 3.5″ 触摸屏、OV5640 摄像头与 PCF85063 RTC 集成在同一块板上。同一平台，选用了更合适的载板。

- **投料机构** —— **28BYJ-48 步进电机**（经 ULN2003 驱动）驱动的**叶轮（paddle-rotor）**投料器：叶轮每转过一格就量出一"兜"固定体积的粮，因此份量取决于转动格数，而与料斗内余量多少无关。完整参数化 3D 模型、可打印 STL 与物料清单见 [`hardware/`](hardware/)。
- **摄像头走 30 cm 排线** —— OV5640 从主板拔下，用 30 cm 排线 + 1:1 转接板引到位于食盆前方的独立 **`cam_mast`** 支架，从而屏幕朝向主人、镜头对准宠物。（排线反插在上电瞬间是致命的——已用万用表核对方向，见 [`hardware/README.md`](hardware/README.md)。）

---

## 六、目录结构

```
app/velapaw/     — 设备端应用（由 manifest 通过 linkfile 映射到 packages/demos/）
agent/           — ai_agent 层：自定义 Skill、心跳任务，以及针对
                   packages/ai_agent 的补丁（详见 agent/README.md）
board/           — 板文件：显示/摄像头/RTC/步进电机（esp32s3_st7789.c）、
                   USB 控制台+网络（esp32s3_composite.c）、开机自启
                   （esp32s3_appinit.c）、板级 defconfig，以及针对 nuttx/、
                   apps/ 与 tflite-micro 的补丁（详见 board/patches/README.md）
host/            — 设备外的模型训练与导出（TensorFlow/Keras）
hardware/        — 可 3D 打印的投料器（OpenSCAD 源文件 + STL + 物料清单）
docs/            — 技术说明、基准测试报告，以及技术报告（初赛版与决赛版，
                   中英文各一份）
logs/            — AI Coding 会话日志（本项目借助 AI 辅助开发完成）
nuttx.bin        — 预编译的决赛版固件（烧录到 0x0，见第七节第 5 步）
```

---

## 七、编译与运行（真机）

需要一台 x86-64 Linux 主机（本队在 Ubuntu 26.04 虚拟机中编译），预留约 30 GB 磁盘空间。第 1 步会创建 openVela 工作区，之后的每一步都在该工作区根目录下、同一个终端中执行。

```bash
# 0. 主机一次性准备：编译工具、Git LFS（openVela 的预编译工具链存放在其中）、
#    Google 的 repo 工具、装进系统 python3 的 esptool（编译的镜像生成步骤会在
#    那里找它），以及串口访问权限。做完后注销并重新登录一次，dialout 组才会生效。
sudo apt-get install -y git git-lfs curl unzip build-essential gperf flex bison \
  gettext texinfo libncurses-dev pkg-config gawk automake libtool python3 \
  python3-pip python3-venv kconfig-frontends u-boot-tools genromfs xxd
git lfs install
curl -sSL https://storage.googleapis.com/git-repo-downloads/repo -o repo \
  && chmod +x repo && sudo mv repo /usr/local/bin/
python3 -m pip install --user --break-system-packages 'esptool>=4.8'
#    （Ubuntu 22.04 及更早版本：去掉 --break-system-packages）
sudo usermod -aG dialout $USER

# 1. 创建工作区，拉取 openVela 与本队仓库。务必保留 --git-lfs：缺少它，repo 会把
#    LFS 文件（包括交叉编译器）留成很小的文本指针文件。已经不带它同步过的
#    工作区，可用下面这条命令修复：
#    git -C prebuilts/gcc/linux-x86_64/xtensa-esp32s3-elf lfs pull
mkdir openvela && cd openvela
repo init --git-lfs -u https://github.com/open-vela/contest2026_043_CircuitForge \
  -b dev-ai-contest-2026 -m contest2026_043_CircuitForge.xml
repo sync -c -j8

VP=$PWD/contest2026_043_CircuitForge
BOARD=nuttx/boards/xtensa/esp32s3/esp32s3-devkit

# 1b. 上游此后改写了 nuttx、apps 和 ai_agent 的分支，第 3 步的补丁与这些
#     分支现在的代码已对不上。按完整哈希值获取制作补丁时所基于的提交（见
#     board/patches/README.md），再检出它们。tflite-micro 可以留在分支上：
#     它的补丁改动的两个文件在分支上没有变化。
git -C nuttx             fetch -q openvela e02f581e235fc7b527d57ff62b668ce625d139ab
git -C apps              fetch -q openvela e81a73794786189f15e6c9fe9931ffddd561fd73
git -C packages/ai_agent fetch -q openvela 41723c61725c4e845bfee724f3ad2fafc416b6e1
git -C nuttx             checkout -q e02f581e235fc7b527d57ff62b668ce625d139ab
git -C apps              checkout -q e81a73794786189f15e6c9fe9931ffddd561fd73
git -C packages/ai_agent checkout -q 41723c61725c4e845bfee724f3ad2fafc416b6e1

# 2. 将板文件放入 ESP32-S3 板级目录。
#    配置目录是新增的，必须先创建，否则 cp 会静默失败。
mkdir -p $BOARD/configs/waveshare_lcd
cp $VP/board/esp32s3_st7789.c    $BOARD/src/
cp $VP/board/esp32s3_composite.c $BOARD/src/
cp $VP/board/esp32s3_appinit.c   $BOARD/src/
cp $VP/board/esp32s3_bringup.c   $BOARD/src/
cp $VP/board/configs/waveshare_lcd/defconfig $BOARD/configs/waveshare_lcd/

# 3. 给上游代码树打补丁。仅把 .c 文件拷进去是不够的：两个新增板级源文件
#    必须注册进 CSRCS，另有若干上游缺陷需要修复，否则链接不过。请按下面的
#    顺序逐个打补丁：有些补丁建立在前一个之上，一次性对全部补丁执行
#    `git apply --check` 会失败。
#    详见 board/patches/README.md 第 3 节与 agent/README.md
(cd nuttx && for p in nuttx-build-registration nuttx-touch-ft5x06 velapaw-audio-nuttx \
                      nuttx-i2s-finals nuttx-usb-ncm-tx nuttx-mic-lib-buffer nuttx-lcd-dc-pin; do
   git apply --check $VP/board/patches/$p.patch && git apply $VP/board/patches/$p.patch \
     || { echo "STOP at $p"; break; }; done)
(cd apps  && for p in apps-mbedtls-isystem velapaw-audio-apps apps-tflm-finals apps-lvgldemo-prototype; do
   git apply --check $VP/board/patches/$p.patch && git apply $VP/board/patches/$p.patch \
     || { echo "STOP at $p"; break; }; done)
(cd apps/mlearning/tflite-micro/tflite-micro \
   && git apply --check $VP/board/patches/tflite-micro-fminmax.patch \
   && git apply $VP/board/patches/tflite-micro-fminmax.patch)
(cd packages/ai_agent && for p in $VP/agent/patches/*.patch; do
   git apply --check "$p" && git apply "$p" || { echo "STOP at $p"; break; }; done)

# 3b. 可选：接入 ESP-NN 算子，每次识别约 1.3 秒，不接入约 6.75 秒。
#     决赛版 nuttx.bin 就是带 ESP-NN 编译的。详见 docs/esp-nn/README.md
git clone https://github.com/espressif/esp-nn apps/mlearning/esp-nn
git -C apps/mlearning/esp-nn checkout d45b843ca5f873ca9d0706ab0e7f14eafd132e98
mkdir -p apps/mlearning/esp-nn/shim
cp $VP/docs/esp-nn/shim/esp_timer.h apps/mlearning/esp-nn/shim/
K=apps/mlearning/tflite-micro/tflite-micro/tensorflow/lite/micro/kernels/esp_nn
mkdir -p $K && cp $VP/docs/esp-nn/kernels/*.cc $K/

# 4. 编译。交叉编译器随工作区一起下载，但没有任何步骤会把它加入 PATH。
export PATH=$PWD/prebuilts/gcc/linux-x86_64/xtensa-esp32s3-elf/bin:$PATH
xtensa-esp32s3-elf-gcc --version   # 应显示 crosstool-NG esp-12.2.0
./build.sh esp32s3-devkit:waveshare_lcd -j8

# 4b. 在全新工作区中，上面这次编译会在 clk_ctrl_os.c 和 modem_clock.c 处报
#     “invalid initializer”而停止。编译途中它会把 esp-hal-3rdparty 下载到
#     nuttx/arch/xtensa/src/esp32s3/，而 nuttx 锁定的这个 HAL 版本早于 NuttX
#     把 spinlock_t 改为结构体，仍用 0 初始化锁。修补 HAL 后再编译一次。
#     执行 distclean 或修改配置后 HAL 会被重新下载，届时需重做 4b。
grep -rlE --exclude-dir=.git 'define[[:space:]]+LOCK_INITIALIZER_UNLOCKED[[:space:]]+\(?0\)?' nuttx/arch/xtensa \
  | xargs -r sed -i -E 's/(define[[:space:]]+LOCK_INITIALIZER_UNLOCKED[[:space:]]+)\(?0\)?/\1SP_UNLOCKED/'
./build.sh esp32s3-devkit:waveshare_lcd -j8

# 5. 烧录固件 + 两个端侧模型。先让板子进入下载模式：按住 BOOT，按一下 RESET，
#    再松开 BOOT。此时串口通常是 /dev/ttyACM0（esptool 4.x 的命令写作
#    write_flash）。
#    $VP/nuttx.bin 是这套源码带 ESP-NN（第 3b 步）预编译的固件，即演示板上
#    运行的固件；如需烧录它，把下面的 nuttx/nuttx.bin 换成 $VP/nuttx.bin 即可。
python3 -m esptool --chip esp32s3 --port <串口> write-flash \
  0x0        nuttx/nuttx.bin \
  0x600000   $VP/app/velapaw/infer/model/velapaw.tflite \
  0x760000   $VP/app/velapaw/infer/model/bcs.tflite
#    烧录完成后按一下 RESET，启动新固件。
```

**6. 写入 Agent 的运行时数据文件 —— 每块板做一次。** 自定义 Skill 与心跳任务清单
是板上 littlefs 的运行时文件，并非编译输入，上面任何一步都不会安装它们：需把
`agent/skills/velapaw-feeding-digest.md` 写入 `/data/ai_agent/skills/`，把
`agent/HEARTBEAT.md` 写入 `/data/ai_agent/`。它们可跨重启、跨重新烧录保留，因为
`/data` 起始于 `0x180000`，而第 5 步只写到约 `0x17ec80`。具体步骤与两个坑见
[`agent/README.md` 第 1 节](agent/README.md)。跳过这一步喂食器照常工作，只是
Agent 的主动推送没有数据可读。

上电后设备直接进入喂食器界面。先在**登记**页录入一只宠物（拍几张贴近、光线良好的正脸照），随后**识别**页即可认出它并按时投喂。

> **关于音频补丁：** `velapaw-audio-nuttx.patch` 在修复之外还带有逐缓冲区的
> `VP: mic` / `VP: RXDUMP` 诊断输出，控制台会很吵；第 3 步随后打上的
> `nuttx-i2s-finals.patch` 会把它们编译掉。见
> [`board/patches/README.md`](board/patches/README.md) 的“Strip before
> shipping”一节。

> **关于推理速度：** 约 1.3 秒的数据基于 ESP-NN 加速算子（第 3b 步，见 [`docs/esp-nn`](docs/esp-nn/README.md)），不接入时约需 6.75 秒。加速配置、硬件 SPI 显示调试与可复现性说明见[工程技术详解](docs/ENGINEERING.md)。

**某一步出错时：**

- **出现 `cp: cannot stat …` 或 `xtensa-esp32s3-elf-gcc: command not found`。** `VP`、`BOARD` 和 `PATH` 的设置只在输入它们的那个终端里有效。换了新终端，先 `cd` 到工作区根目录，再把它们重新执行一遍。
- **`--version` 检查输出的不是版本号**（通常是 `version: command not found`）。工具链仍是 Git LFS 指针文件：执行 `git -C prebuilts/gcc/linux-x86_64/xtensa-esp32s3-elf lfs pull`。
- **第 3 步输出 `STOP at …` 或 `patch does not apply`。** 最常见的原因是本工作区已经执行过第 3 步，而同一个补丁不能打两次。否则就是跳过了第 1b 步，这些补丁与上游分支现在的代码对不上。无论哪种情况，都先撤销补丁，跳过了第 1b 步的话把它补上，再重新执行第 2、3 步：

  ```bash
  for t in nuttx apps apps/mlearning/tflite-micro/tflite-micro packages/ai_agent; do git -C $t reset -q --hard; done
  rm -f apps/mlearning/tflite-micro/float_shim.h packages/ai_agent/src/tools/tool_meal.[ch]
  ```

- **做过第 4b 步后又出现 `invalid initializer`。** HAL 被重新下载了：重做第 4b 步。
- **链接时出现数百个未定义的 `ets_*` / `esp_rom_*` 符号。** 编译留下了空的 `*.ld.tmp` 链接脚本：执行 `find nuttx -name '*.ld.tmp' -size 0 -delete`，再重新编译。
- **访问 `/dev/ttyACM0` 时报 `Permission denied`。** 第 0 步加入的 `dialout` 组要注销并重新登录后才生效。
- **下载很慢或超时（中国大陆）。** 改从 `https://mirrors.tuna.tsinghua.edu.cn/git/git-repo` 下载 `repo`，并在第 1 步之前执行 `export REPO_URL=https://mirrors.tuna.tsinghua.edu.cn/git/git-repo`；在 `pip` 那一行加上 `-i https://pypi.tuna.tsinghua.edu.cn/simple`；再让 `repo sync` 从 Gitee 镜像拉取 openVela。本队仓库只在 GitHub 上，所以需要第三条规则：

  ```bash
  git config --global url."https://gitee.com/open-vela/".insteadOf "https://github.com/open-vela/"
  git config --global url."https://gitee.com/openvela-toolchain-external/".insteadOf "https://github.com/openvela-toolchain-external/"
  git config --global url."https://github.com/open-vela/contest2026_043_CircuitForge".insteadOf "https://github.com/open-vela/contest2026_043_CircuitForge"
  ```

---

## 八、AI 辅助开发说明

本项目全程采用 AI 辅助（AI Coding）开发，完整会话日志已按要求提交于 [`logs/`](logs/)。AI 贯穿了整个开发生命周期：

- **需求与方案设计** —— 打磨"识别门控投喂"模型、开集登记方案，以及健康监测分析。
- **固件与驱动** —— 在 ESP32-S3 上完成显示屏、OV5640 摄像头（LCD_CAM）、RTC 与 28BYJ-48 步进电机投料的驱动；把 TensorFlow Lite Micro + ESP-NN 落到 Xtensa LX7 上运行。
- **硬核调试** —— 最典型的一例：一个硬件 SPI 显示 bug 曾经历约 60 次盲目编译仍无法解决，最终借助**逻辑分析仪**攻克，根因定位为 CS 常低电平的屏在引脚交接时丢失位同步，并顺势把显示从软件位翻转升级为**硬件 SPI + DMA**，UI 大幅提速。
- **产品迭代** —— 完整的宠物生命周期（登记、识别、投喂、编辑、删除、头像），全部在真机上验证。

---

## 九、进展状态

**决赛更新：** 上文[决赛更新](#决赛更新--初赛之后新增的内容)所列六项改动均已在真机上验证，包含在预编译的决赛版 `nuttx.bin` 中，源码也已在本仓库中。

**端侧 `ai_agent`：** 已在真机上运行 —— 自定义喂食简报 Skill、`ask` CLI 交互渠道，以及已捕获到无人值守自动触发的主动推送。它是 VelaPaw 中唯一需要联网的部分；喂食器本体在断网时照常工作。详见上文第四节。

**目前已在真机上运行：** 端侧识别、体况评估、带单餐跳过开关的识别门控每日三餐排程、份量与每日上限控制、带食欲骤降告警的进食历史与趋势、硬件 RTC、带持久化头像的完整宠物生命周期、开机自启、硬件 SPI + DMA 显示、中英双语界面（可实时切换）、已在真机上端到端验证的 MEAL→HOUR→CONFIRM 语音投喂排程对话、**28BYJ-48 步进电机投料机构**（GPIO9/10/11/43 驱动）、**引到 30 cm 排线上的 OV5640**（已通过转接板 + `cam_mast` 支架确认工作正常），以及**外壳 3D 打印件的最终机械装配**——步进电机投料机构与摄像头均已装入打印结构。

---

## 十、数据集

本项目使用的数据集可在此获取：https://drive.google.com/drive/folders/1-HnbS-VStbKuXCXWK-O1Nh2lNLdqMpbI

---

*由 CircuitForge 队为 2026 首届 openVela AI 硬件开发者大赛打造。*
