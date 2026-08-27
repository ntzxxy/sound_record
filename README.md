# 多线程语音交互与音频流传输系统

一个面向 Linux 开发板与 PC 的离线语音交互原型。开发板负责按键触发、麦克风采集、TCP 音频传输和语音播放；PC 端负责流式语音识别、本地大模型对话、语义预分类和语音合成。`ASR_model` 分支在基础语音闭环之上，加入了面向智能家居场景的设备指令、记忆和异常记录处理。

## 功能

- 使用 ALSA 采集开发板麦克风音频，并在板端播放服务端下发的 TTS PCM。
- 通过 TCP 长连接传输双向音频流：MIC 音频上行、TTS 音频下行。
- PC 端基于 sherpa-onnx 进行流式 ASR，使用 llama.cpp 加载本地 GGUF 模型完成意图预分类与对话生成。
- 对话回复按文本片段送入 TTS 管线，合成的 PCM 随即回传到开发板播放。
- 对语音交互中的 TTS 打断、ASR 重置、播放结束后的冷却期、网络重连和录音/网络线程解耦做了基础处理。
- 支持普通对话、设备控制、设备故障记录、用户偏好与物品位置的记忆写入、查询和删除。

## 处理流程

```text
开发板按键
  └─ ALSA 采集线程
       └─ RingBuffer ── TCP / AIV1 音频帧 ──> PC 服务端
                                                   ├─ sherpa-onnx 流式 ASR
                                                   ├─ 语义预分类与规则校正
                                                   │    ├─ 设备控制 / 澄清
                                                   │    ├─ 设备异常记录
                                                   │    └─ 记忆写入 / 查询 / 删除
                                                   └─ llama.cpp 对话或固定回复
                                                        └─ TTS 管线
PC 服务端 <── TCP / AIV1 TTS 帧 ─────────────────────┘
  └─ 开发板 ALSA 播放线程
```

录音的业务提交以开发板发送的 `MIC_END` 为准；ASR 在录音期间的端点用于内部断句。新一轮录音开始时，服务端会尝试中断尚未播放完的 TTS。

## 语义与记忆模块

`libassistant` 将本地 LLM 的 JSON 输出解析为下列任务类型，并在进入普通对话前完成相应处理：

| 类型 | 处理方式 |
| --- | --- |
| `GENERAL_CHAT` | 交给对话模型生成回复。 |
| `DEVICE_CONTROL` | 补全并校验房间、设备、动作和温度；当前仅返回模拟控制结果。 |
| `DEVICE_FAULT` | 保存异常描述，并将“尚未修复”的约束作为上下文交给对话模型。 |
| `MEMORY_WRITE` | 保存用户偏好或物品位置。 |
| `MEMORY_QUERY` | 选取相关记忆，作为运行时上下文交给对话模型。 |
| `MEMORY_DELETE` | 删除指定记忆或清空全部记忆。 |
| `CLARIFY` | 对不完整的设备指令追问缺失槽位。 |

为减小意图误判对控制流程的影响，服务中还会对包含“打开、关闭、设置、调到”等词的空调/灯指令做确定性补全；对“删除、清除、忘掉”等表达优先按删除记忆处理。待补全的设备指令只保留一个后续轮次，用户说“取消”“算了”等可直接终止该状态。

当前内置设备表仅包含客厅和卧室的空调、灯。支持打开、关闭，以及空调 16～30 ℃的温度设置；灯具不支持温度设置。没有命中的设备或不合法的动作会直接返回说明，不会调用模型，也不会产生真实设备操作。

记忆以 TSV 文件保存到运行目录：

```text
runtime/assistant_memory_v2.tsv
runtime/device_fault_events.tsv
```

记忆条目仅接受 `USER_PREFERENCE`（用户偏好）和 `OBJECT_LOCATION`（物品位置）两类，最多保留 50 条。文件中会对制表符、换行符和反斜杠进行转义；写入时先生成临时文件再替换原文件。

## 目录

```text
.
├── src/                 # 开发板入口、按键与 LED
├── libaudio/            # ALSA 采集/播放、RingBuffer、音频线程
├── libnet/              # TCP 连接和 AIV1 音频帧定义
├── server/              # PC 端流式接收、ASR/LLM/TTS 调度
├── libasr/              # sherpa-onnx 流式 ASR 封装
├── libllm/              # llama.cpp 推理与对话上下文管理
├── libtts/              # sherpa-onnx TTS 与异步合成/播放管线
├── libassistant/        # 意图解析、设备注册表、记忆和异常日志
├── models/              # ASR、LLM、TTS 模型目录
├── tests/               # assistant 核心逻辑测试与 LLM stub
├── tools/               # 助手上下文演示程序
├── third_party/         # sherpa-onnx、llama.cpp 等第三方依赖
├── run.sh               # 开发板启动脚本
└── run_server.sh        # PC 服务启动脚本
```

## 依赖与模型

PC 服务端需要 Linux、CMake 3.17+、支持 C++17 的编译器、ALSA 开发库，以及仓库中的 `third_party/sherpa-onnx` 和 `third_party/llama.cpp` 源码。以 Debian/Ubuntu 为例，系统依赖可先安装：

```bash
sudo apt update
sudo apt install build-essential cmake libasound2-dev
```

完整服务默认从以下位置读取模型。GGUF 文件通常因体积较大被 `.gitignore` 忽略，需要自行准备；如路径不同，可修改 `run_server.sh` 中的变量，或在启动前设置 `BUILD_DIR`、`TTS_MODEL`。

```text
models/
├── sherpa-onnx-streaming-zipformer-small-bilingual-zh-en-2023-02-16/
│   ├── encoder-epoch-99-avg-1.onnx
│   ├── decoder-epoch-99-avg-1.onnx
│   ├── joiner-epoch-99-avg-1.onnx
│   └── tokens.txt
├── gemma-4-E4B-it-Q4_0.gguf
├── gemma-4-E4B_q4_0-it.gguf  # Google 官方 QAT 版，可通过 LLM_MODEL 选择
├── christina-tts-1.5-q4/
│   ├── qwen3-tts-0.6b-f16.gguf
│   ├── qwen3-tts-tokenizer-f16.gguf
│   ├── christina.spk          # 英文音色
│   └── christina-zh.spk       # 中文音色
└── vits-tts-zh/
    ├── model.int8.onnx 或 model.onnx
    └── tokens.txt
```

### Christina Qwen3-TTS（默认）

默认 TTS 已替换为 `models/christina-tts-1.5-q4`。模型目录只保留中英文
speaker embedding；核心 GGUF 权重由两种语言共享。`libtts` 会按文本是否包含
汉字自动选择 `christina-zh.spk` / `zh` 或 `christina.spk` / `en`，并输出
24 kHz、单声道 S16 PCM。

推理程序位于 `third_party/qwen3-tts.cpp`。该程序在本项目中增加了 `-s` 参数，
以加载模型随附的预计算 speaker embedding。首次在 Jetson Orin NX 上部署时，必须
用 Jetson 的 CUDA 工具链构建 GGML 和推理程序：

```bash
cmake -S third_party/qwen3-tts.cpp/ggml -B third_party/qwen3-tts.cpp/ggml/build \
  -DCMAKE_BUILD_TYPE=Release -DGGML_CUDA=ON
cmake --build third_party/qwen3-tts.cpp/ggml/build -j
cmake -S third_party/qwen3-tts.cpp -B third_party/qwen3-tts.cpp/build \
  -DCMAKE_BUILD_TYPE=Release
cmake --build third_party/qwen3-tts.cpp/build -j
```

随后按正常方式启动服务即可；在 Orin 上显式使用 CUDA：

```bash
QWEN3_TTS_BACKEND=cuda ./run_server.sh
```

`run_server.sh` 会自动设置 `TTS_QWEN_CLI`。若将二进制部署到其他位置，可通过同名
环境变量传入绝对路径。Qwen3-TTS 的 CPU 模式仅用于开发验证，不适合实时对话。

## 构建

### PC 服务端

在仓库根目录执行：

```bash
cmake -S . -B cmake-build-wsl-local
cmake --build cmake-build-wsl-local --target stream_receiver -j
```

生成的服务端程序位于 `cmake-build-wsl-local/bin/stream_receiver`。顶层 CMake 默认同时启用 ASR、LLM、服务端和 assistant 工具；在交叉编译环境中，ASR、LLM、服务端会自动跳过。

### 开发板客户端

开发板端使用交叉工具链配置 CMake，工具链文件和部署方式取决于板卡 BSP，不在仓库中固定。构建目标为 `out`；构建目录的 `bin/` 下还会同步 `run.sh`、`mic_in_config.sh` 和 `mic_in_config_usb.sh`，应一并复制到开发板。

```bash
cmake --build <交叉编译构建目录> --target out -j
```

板端程序依赖 ALSA、GPIO 按键事件设备 `/dev/input/by-path/platform-gpio_keys@0-event`，以及脚本中配置的声卡。USB 麦克风配置默认使用 `plughw:1,0`，16 kHz、单声道。

## 运行

### 1. 启动 PC 服务

确认模型路径和构建目录后运行：

```bash
./run_server.sh
```

脚本默认监听 8080 端口，构建目录为 `cmake-build-wsl-local`，录音保存到 `voice_records/`，TTS 线程数默认为 4。开发板客户端的端口当前也固定为 8080，因此按默认方式启动时不需要在命令末尾追加端口号。默认 LLM 为实测响应更快的 `gemma-4-E4B-it-Q4_0.gguf`（约 4.3 GB）；Google 官方 QAT 版 `gemma-4-E4B_q4_0-it.gguf`（约 4.8 GB）可通过 `LLM_MODEL` 切换。可按需覆盖构建目录、LLM、TTS 模型路径和 TTS 线程数：

```bash
BUILD_DIR=/path/to/build LLM_MODEL=/path/to/model.gguf TTS_MODEL=/path/to/tts TTS_NUM_THREADS=4 ./run_server.sh
```

脚本会为 ONNX Runtime 设置 `LD_LIBRARY_PATH`。如 `stream_receiver` 不在预期位置，先按上节命令构建，或通过 `BUILD_DIR` 指向实际构建目录。

### 2. 启动开发板客户端

在开发板上的 `bin/` 目录编辑 `run.sh`：

```sh
SERVER_IP="192.168.0.14"
MIC_TYPE="usb"      # 可选：usb 或 wm8960
```

然后执行：

```bash
./run.sh
```

按住按键开始录音，松开后发送本轮音频；服务端处理完成后，TTS 音频会经同一条 TCP 连接返回播放。

ASR 封装以 16 kHz 单声道 PCM 输入为前提。USB 默认配置与其一致；仓库中的 WM8960 配置为 44.1 kHz 双声道，若使用该配置，应先在板端完成重采样和单声道转换，或调整声卡/采集参数，否则识别结果和时长统计可能不正确。

## AIV1 帧协议

上下行都使用 `AiFrameHeader_t`，字段依次为：

```text
magic | version | type | seq | timestamp | sample_rate | channels | format | payload_size
```

| 帧类型 | 用途 |
| --- | --- |
| `MIC_START` / `MIC_END` | 一轮录音的开始与结束。 |
| `MIC_PCM` | 上行 PCM 音频数据。 |
| `TTS_START` / `TTS_END` | 一轮下行语音的开始与结束。 |
| `TTS_PCM` | 下行 PCM 音频数据。 |
| `TTS_CANCEL` | 清空开发板播放队列并停止当前播放。 |

当前版本号为 1，音频格式为 `S16_LE`。序号、采样率和声道数均随帧携带，服务端会记录序号跳变；该协议目前用于本项目的板端与服务端配对通信。

## 测试

`test_assistant_core` 覆盖 JSON 解析、设备校验、澄清补全、取消、记忆写删查、异常日志和上下文选择。它使用 LLM stub，不加载 ASR、LLM 或 TTS 模型：

```bash
cmake -S . -B cmake-build-assistant-test \
  -DBUILD_ASR=OFF -DBUILD_LLM=OFF -DBUILD_SERVER=OFF
cmake --build cmake-build-assistant-test --target test_assistant_core -j
./cmake-build-assistant-test/bin/test_assistant_core
```

成功时会输出：

```text
[TestAssistantCore] all tests passed
```

## 后续计划

- 推进模型和推理链路向端侧部署，减少对 PC 服务端的依赖。
- 使用 Qt 完善交互页面，提供可视化的设备状态和语音交互界面。
- 增加 benchmark，记录并对比 ASR、LLM、TTS 和端到端链路的耗时。
