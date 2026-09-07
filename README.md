# Jetson 端侧离线智能家居终端

面向本地、离线部署的智能交互项目。PC 端已完成 **音频采集 → ASR → 任务处理 / LLM → TTS 播放** 的完整语音交互验证；Jetson 当前主要完成 Qt 文字链路、模型推理、Task Router 与 Benchmark 验证。

项目采用“模块独立验证、能力逐步集成”的开发方式：Qt 控制台用于 Jetson 文字流程验证；Jetson 语音链路尚未完成完整实机验证；视觉工具目前仅完成 PC 端初步验证。

## 技术栈

`C/C++`   `Linux`  `多线程`  `ALSA`  `RingBuffer`  `Sherpa-ONNX`  `llama.cpp`  `Qt`  `CMake`

## 交互示例

Qt 控制台与语音运行时复用 `ConversationRuntime`，因此下面的文字请求与识别后的语音文本走同一任务处理路径：

```text
用户：打开客厅灯
系统：识别为设备请求 → 校验参数 → 返回模拟执行结果

用户：记住钥匙在玄关柜里
系统：写入物品位置记录，后续可通过自然语言查询
```

## 系统架构

```text
                         ┌──────────────── Qt 文字控制台 ────────────────┐
                         │                  文字请求                      │
                         └─────────────────────┬─────────────────────────┘
                                               │
[Mic / ALSA] → [Audio Pipeline] → [ASR] → [ConversationRuntime / Task Router]
    │                 │                         │              │
    │                 │                         │              ├─ 明确请求：本地规则与状态处理
    │                 │                         │              │      ├─ 参数校验 / 追问 / 取消 / 续句
    │                 │                         │              │      └─ 模拟设备执行 / 记忆 / 事件
    │                 │                         │              │
    │                 │                         │              └─ 开放对话或复杂表达：本地 LLM
    │                 │                         │                         │
    └── 新一轮录音可打断正在播放的 TTS ◄────────┴──────────── [TTS] → [Speaker / ALSA]

                         [Benchmark]
                           └─ 通过 Qt 控制台记录按轮次性能与资源采样

                         [Vision Tools]
                           └─ 独立的相机 / 检测 / 视觉对话工具（PC初步验证,待上板验证）
```

## 核心能力

- **端侧、本地、离线链路**：语音运行时在 Linux 环境内串联采集、识别、任务处理、对话与播报。
- **C++ 多模块工程**：音频、语音、对话、助手逻辑、运行时和 UI 分层组织，可根据目标平台按需构建。
- **音频输入与播放控制**：支持按键触发录音；开始新的录音时，可打断仍在播放的回复，避免多轮交互的播放堆积。
- **混合任务路由**：明确的结构化请求优先走本地规则与状态逻辑；开放式对话和复杂表达才进入 LLM 路径。
- **业务状态管理**：支持设备请求的参数校验、缺失信息追问、取消和后续补充；支持用户偏好、物品位置和设备异常记录的维护。
- **工程验证基础**：提供  Benchmark 脚本和不依赖真实模型、麦克风的核心测试入口。

> 设备控制当前为模拟执行。项目现阶段的重点是“自然语言解析 → 参数校验 → 状态管理 → 控制接口”的完整处理流程。待后续接入真实设备。

## 关键工程设计

### 音频链路与并发处理

`libaudio` 负责 ALSA 采集、播放和兼容传输路径，使用 `pthread` 将采集、发送、下行和播放拆分处理；其中 RingBuffer 用于缓冲音频数据。面向本地运行时的 `libpipeline` 使用事件队列，将采集事件异步交给 ASR 和后续对话流程。

### 播放打断

新的录音开始时，语音运行时会请求中断当前 TTS，重置 ASR。

### 任务路由：业务逻辑与 LLM 分工

`libassistant` 先识别可确定处理的设备、记忆和记录请求，并完成本地校验与状态转换；`libllm` 负责本地模型的对话生成与上下文管理。参数不完整的设备请求会进入有限多轮追问状态，用户可取消或在下一句补全信息。

### 统一对话运行时与可测试边界

`libconversation` 提供统一的请求队列和事件回调。`cockpit_runtime` 通过它提交语音识别结果，Qt 控制台通过它提交文字请求；两条入口复用任务路由、记忆和对话逻辑。

## 已完成与后续目标

### 已完成

- [x] Jetson 本地文字运行时与 C++ 模块化构建。
- [x] PC 端音频采集、ASR、任务处理 / LLM、TTS 的完整语音交互验证。
- [x] Qt 文字控制台，复用语音链路的核心对话逻辑。
- [x] 基于规则与状态的任务路由、模拟设备控制、记忆和异常记录。
- [x] 按轮次记录的文字链路 Benchmark 脚本。
- [x] 两轮文字链路 Benchmark 数据与结果汇总。

### 进行中

- [ ] Jetson 语音链路的实机稳定性、端到端延迟与整体交互体验验证。
- [ ] 重新设计视觉链路及测试方案，覆盖视觉输入、推理、交互融合与对应性能指标；当前仅保留为后续重点。
- [ ] 对接真实设备执行层；当前控制结果仍是模拟执行。
- [ ] 在实际设备条件具备后，持续完善端侧语音与视觉的集成验证。


### Benchmark Results（文字直连）

两轮测试均为 8 条预热、52 条计入统计的文字请求。时间均为毫秒；P95 按最近秩计算。`< 1 ms` 表示该请求在当前毫秒级计时精度内完成，不应解读为绝对零延迟。

| 轮次      | 平台 / 推理 | 模型 | 冷启动 | 样本 | Local Fast Path / Structured Extraction（混合）P50 / P95 | LLM Chat：TTFT P50 / P95 | LLM Chat：完整轮次 P50 / P95 | LLM Chat：生成速度 P50 / P95 | 意图标签匹配 |
|---------| --- | --- | --- | -- | --- |--------------| --- | --- | --- |
| `test2` | Jetson aarch64，40 W | `gemma-4-E4B-it-Q4_0.gguf` | 3,171 ms | 52 | < 1 / 8,613 ms | 591 / 639 ms | 2,868 / 6,488 ms | 16.86 / 18.17 tok/s | 47 / 52 |
| `test1` | Jetson aarch64，40 W | `gemma-4-E4B-it-Q4_0.gguf` | 3,171 ms | 52 | < 1 / 9,449 ms | 491 / 779 ms | 6,976 / 7,827 ms | 17.41 / 18.52 tok/s | 51 / 52 |



| 应用进程 CPU | 应用进程 RSS 峰值 | 系统 RAM | GPU 利用率 |
|-| --- | --- | --- |
| 平均 17.6%，峰值 100% | 7.50 GiB | 平均 7,341 MB，峰值 7,580 / 15,537 MB | 平均 93.6%，峰值 100% |

### 结果说明

- 多数本地快路径在当前毫秒级计时精度内完成，因此 P50 为 `< 1 ms`。
- **P95 较大来自语义回退请求。** 部分请求最终返回固定回复，但会先调用 `llm_generate_once` 完成结构化意图提取；这一步的耗时记录在 `intent_latency_ms`，却不会产生对话生成的 TTFT、token/s 等字段。两轮中该类请求约为 6–10 秒，因而抬高了 P95。
- **分类口径：** `Local Fast Path` 为本地规则和状态处理；`Structured Extraction` 为 `llm_generate_once` 提取结构化意图后返回固定回复；`LLM Chat` 为 `llm_chat` 生成自然语言回复。前两者在表中混合统计，不能视为纯控制命令时延或端到端语音性能。
- **意图标签匹配：** test1 为 51/52，唯一错误是普通温度建议被误判为记忆查询；test2 为 47/52，错误主要集中于拼图相关的隐式记忆/多轮上下文，以及“关灯但保持另一设备状态”的复合控制表达。两轮用例不完全相同，标签结果反映当前路由边界的波动，不代表完整质量结论。


### 测试集中代表性实际链路

泛化偏好/习惯选择先用一次模型提取结构化字段，避免本地规则错误地把偏好、条件或上下文解析为控制命令。对象位置等稳定句式已有本地快路径。模型在结构化输出中可提供回复文本，但系统仍会先完成校验和持久化，再向用户确认“已记住”。

下表来自实际记录：

| 用例 | 输入（节选） | 实际链路 | 结果与记录耗时                                                                  |
| --- | --- | --- |--------------------------------------------------------------------------|
| C013 | 打开客厅的灯 | 本地设备命令匹配 → 设备注册表/参数校验 → 模拟执行回复 | `DEVICE_CONTROL`；< 1 ms；未接入真实设备。                                         |
| C035 | 请记住，我的钥匙放在客厅鞋柜第二层 | 本地对象位置解析 → 记忆校验与写入 → 固定确认 | `MEMORY_WRITE`；< 1 ms；“钥匙/位置/客厅鞋柜第二层”已写入。                                |
| C043 | 请记住，我睡觉时喜欢把空调设为二十五度 | 显式偏好标记 → `llm_generate_once` 提取记忆字段 → 校验、写入 → 使用结构化输出中的回复 | `MEMORY_WRITE`；9,882 ms；写入“空调温度=25度”，条件“睡觉时”。                            |
| C087 | 阅读时我不喜欢太刺眼的光 | 隐式偏好候选 → `llm_generate_once` 提取条件化记忆 → 校验、写入 → 回复 | `MEMORY_WRITE`；9,326 ms；写入“灯光=不喜欢太刺眼”，条件“阅读时”。                           |
| C051 | 请记录，客厅空调不制冷 | 本地故障解析 → 事件校验与写入 → 固定确认 | `DEVICE_FAULT`；< 1 ms；故障事件已写入。                                           |
| C067 → C068 | 打开空调。→ 客厅的。 | 本地不完整命令匹配 → 保存待补全状态并追问 → 下一轮补齐房间 → 模拟执行回复 | `CLARIFY → DEVICE_CONTROL`；两轮均 < 1 ms。                                   |
| C088 | 如果我说开始阅读，你理解为我想做什么？ | 本地对话路由 → `llm_chat` 根据已写入的阅读记忆生成解释性回复 | `GENERAL_CHAT`；回复避免刺眼灯光。TTFT 211 ms，完成 1,752 ms，16.74 tok/s；未请求、未执行设备控制。 |
| C092 | 你还记得我不喜欢哪种光吗？ | 本地记忆查询匹配 → 读取已持久化的偏好 → 固定回复 | `MEMORY_QUERY`；< 1 ms；返回“不喜欢太刺眼”。                                        |

因此，C043/C087 的长耗时来自结构化意图提取本身，而非后续的记忆写入或固定回复。若要让这类明确偏好也走本地快路径，需要新增并持续维护“偏好 + 条件 + 值”的本地解析规则；这属于后续的策略优化，而不是当前链路缺少模型回复。


### 测试方法

文字用例覆盖普通对话、设备请求、参数校验、记忆读写、异常记录、追问与多轮上下文。记录时将带 LLM 生成指标的请求与无 LLM 生成请求分开统计；后者包含设备控制、记忆、追问和固定回复等路径，可能仍包含路由或状态处理，不能简单视为纯控制命令或纯规则函数耗时。

Jetson 上可用以下脚本启动交互式测试：

```bash
./scripts/run_jetson_qt_llm_benchmark.sh \
  <qt_hmi程序路径> <本地模型文件> <结果目录>
```

### 日志与结果

每个会话目录包含 `turn_metrics.csv`、冷启动记录、用例规格和对话转录；具备采样条件的运行还会保存环境说明、进程资源采样、设备侧状态记录和原始日志。`raw/` 目录用于复核异常轮次。脚本只负责记录和归档，不替代功能测试。

## 模块关系

| 模块 | 目录 | 职责与关系 |
| --- | --- | --- |
| 板端交互 | `src/`、`libaudio/`、`libnet/` | 提供按键、ALSA 音频和保留的板端—PC 传输能力；本地运行时不依赖该网络传输。 |
| 语音能力 | `libasr/`、`libtts/` | 分别封装识别与播报，由 `cockpit_runtime` 的适配层接入本地语音流程。 |
| 本地语音编排 | `libpipeline/`、`apps/cockpit_runtime/` | `libpipeline` 将采集事件、ASR 结果、对话提交和 TTS 打断串联；`cockpit_runtime` 负责具体的 Jetson 运行入口。 |
| 助手与对话 | `libassistant/`、`libconversation/`、`libllm/` | `libassistant` 处理路由、校验、记忆和状态；`libllm` 负责本地对话生成；`libconversation` 将二者组织为统一的异步请求与事件接口。 |
| 文字控制台 | `apps/qt_hmi/` | 通过 `ConversationRuntime` 复用助手与对话逻辑，不经过旧 TCP 服务，并承担文字 Benchmark 入口。 |
| 视觉工具 | `libvision/`、`tools/` | 独立的相机、检测和视觉对话验证工具；当前仅完成 PC 初步测试。 |
| 兼容服务 | `server/`、`desktop_client/` | 保留早期 PC 接收服务与桌面控制客户端，便于已有双端演示环境继续使用。 |
| 测试与脚本 | `tests/`、`scripts/` | 包含核心逻辑测试、环境脚本和 Benchmark 启动脚本。 |

## 编译与运行

### Jetson 语音运行时构建入口

`cockpit_runtime` 是面向 Jetson 的本地运行入口。运行前需要准备本地模型、音频设备和按键环境；完整部署步骤见 [Jetson 部署说明](docs/JETSON_ORIN_NX_DEPLOYMENT.md)。

```bash
cmake -S . -B build-jetson -DBUILD_COCKPIT_RUNTIME=ON
cmake --build build-jetson --target cockpit_runtime -j
```

### Qt 文字控制台

Qt 控制台可用于验证对话、任务路由和历史记录，不依赖音频采集与播放。

```bash
cmake -S . -B build-qt -DBUILD_QT_HMI=ON
cmake --build build-qt --target qt_hmi -j
./build-qt/bin/qt_hmi <本地模型文件>
```

### 兼容的板端—PC 演示

早期双端语音演示仍可通过 `run_server.sh` 和 `run.sh` 使用。新建 Jetson 体验建议优先使用本地语音运行时。

## 自动化测试

核心业务逻辑可通过 mock/stub 与真实模型、音频硬件解耦：`test_assistant_core` 覆盖任务路由、校验、追问、记忆和事件；`test_chat_agent` 覆盖对话历史与上下文；`test_local_voice_pipeline` 覆盖模拟 PCM、对话提交和 TTS 打断。

仓库还保留 `test_conversation_runtime` 与 `test_control_gateway` 测试目标，用于覆盖运行时事件和控制接口。以下是当前已验证可在无模型、无音频硬件环境下构建和运行的核心测试：

```bash
cmake -S . -B build-tests \
  -DBUILD_ASR=OFF -DBUILD_LLM=OFF -DBUILD_SERVER=OFF \
  -DBUILD_VISION=OFF -DBUILD_COCKPIT_RUNTIME=OFF \
  -DBUILD_ASSISTANT_RUNTIME=ON -DBUILD_BOARD_CLIENT=OFF \
  -DBUILD_DESKTOP_CLIENT=OFF
cmake --build build-tests --target \
  test_assistant_core test_chat_agent test_local_voice_pipeline -j

./build-tests/bin/test_assistant_core
./build-tests/bin/test_chat_agent
./build-tests/bin/test_local_voice_pipeline
```

## 目录概览

```text
apps/           应用入口：Jetson 本地语音运行时与 Qt 控制台
lib*/           音频、语音、对话、助手、视觉等功能模块
server/         保留的 PC 端接收与控制服务
tools/          独立功能验证工具
tests/          自动化测试
scripts/        启动、环境准备和 Benchmark 脚本
docs/           部署与使用文档
models/         本地模型存放位置（模型文件不随仓库提交）
```
