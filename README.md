# 多线程语音交互与音频流传输系统

这个项目主要做的是一个离线语音交互原型：开发板端负责录音、播放和网络传输，PC 端负责语音识别、大模型回复和语音合成。整体流程是：按键开始录音，板端采集 MIC 音频并通过 TCP 发到 PC；PC 端完成 ASR、LLM、TTS 后，再把合成的音频流发回板端播放。

项目目前更偏工程验证，重点放在 Linux 音频、多线程、Socket 流传输和离线推理链路打通上。

## 功能概览

- 板端使用 ALSA 采集 MIC PCM 音频，支持 USB 麦克风和 WM8960 配置。
- 使用 pthread 将录音、上传、下行接收和播放拆成独立线程。
- 使用 RingBuffer 解耦音频采集和 TCP 网络发送，降低网络阻塞对录音的影响。
- 基于 TCP 长连接实现双向音频流传输，支持 MIC 音频上行和 TTS 音频下行。
- PC 端集成 sherpa-onnx 流式 ASR、llama.cpp 本地推理和 TTS 语音合成。
- 加入了断线重连、播放打断、ASR 状态重置和 TTS 回声冷却等处理。

## 整体链路

```text
开发板 MIC
  -> ALSA 采集线程
  -> RingBuffer
  -> TCP 上行线程
  -> PC 端 stream_receiver
  -> sherpa-onnx ASR
  -> llama.cpp / Qwen2.5 本地推理
  -> TTS 合成
  -> TCP 下行
  -> 开发板播放线程
  -> ALSA 播放
```

## 目录说明

```text
.
├── src/              # 板端主程序、按键、LED
├── libaudio/         # ALSA 采集/播放、RingBuffer、音频线程
├── libnet/           # TCP 连接、音频帧协议、发送接口
├── server/           # PC 端音频流接收、ASR/LLM/TTS 调度
├── libasr/           # sherpa-onnx ASR 封装
├── libllm/           # llama.cpp 推理与对话封装
├── libtts/           # TTS 模型和 TTS 管线
├── models/           # ASR、LLM、TTS 模型文件
├── third_party/      # sherpa-onnx、llama.cpp 等第三方库
├── voice_records/    # PC 端保存的录音文件
├── voice_answers/    # TTS 输出文件目录
├── run.sh            # 板端启动脚本
└── run_server.sh     # PC 端启动脚本
```

## 技术栈

- C/C++
- Linux / pthread / std::thread
- ALSA
- TCP Socket
- RingBuffer
- sherpa-onnx
- llama.cpp
- Qwen2.5 / DeepSeek GGUF 模型
- TTS

## 构建说明

项目使用 CMake 管理。PC 端和交叉编译场景在顶层 CMake 中做了区分：交叉编译时会跳过 PC 端的 ASR、LLM、TTS 和 server 构建，只构建板端程序。

PC 端构建示例：

```bash
cmake -S . -B cmake-build-wsl-local
cmake --build cmake-build-wsl-local -j
```

板端程序会生成 `out`，PC 端服务程序会生成 `stream_receiver`。具体输出目录取决于构建目录，一般在对应 build 目录的 `bin/` 下。

## 运行方式

### 1. 启动 PC 端服务

先确认 `run_server.sh` 中模型路径和构建目录正确，然后执行：

```bash
./run_server.sh 8080
```

脚本默认使用：

- ASR 模型：`models/sherpa-onnx-streaming-zipformer-small-bilingual-zh-en-2023-02-16`
- LLM 模型：`models/qwen2.5-3b-instruct-q4_k_m.gguf`
- TTS 模型：`models/vits-tts-zh`
- 录音保存目录：`voice_records/`

### 2. 启动开发板端程序

修改 `run.sh` 里的 PC 端 IP：

```sh
SERVER_IP="192.168.0.14"
```

然后在板端运行：

```bash
./run.sh
```

当前脚本默认使用 USB 麦克风：

```sh
MIC_TYPE="usb"
```

如果使用板载 WM8960，可以改成：

```sh
MIC_TYPE="wm8960"
```

## 音频帧协议

上下行都使用同一个基础帧头，主要字段包括：

```text
magic / version / type / seq / timestamp / sample_rate / channels / format / payload_size
```

主要帧类型：

- `MIC_START`：一轮录音开始
- `MIC_PCM`：MIC 音频数据
- `MIC_END`：一轮录音结束
- `TTS_START`：TTS 下行开始
- `TTS_PCM`：TTS 音频数据
- `TTS_END`：TTS 下行结束
- `TTS_CANCEL`：取消当前播放

这样做主要是为了避免直接发送裸 PCM 时无法区分业务边界，也方便处理播放打断、断线重连和音频格式变化。

## 当前状态

目前已经打通了基本的语音交互闭环：

```text
录音 -> TCP 上传 -> ASR -> LLM -> TTS -> TCP 下发 -> 播放
```

当前实现主要面向单开发板连接到单 PC 服务的场景。多客户端、完整 VAD、严格回声消除和端侧模型推理还没有展开，后续可以继续优化。

## 后续计划

- 增加 VAD，减少无效音频上传。
- 优化端到端延迟，统计 ASR、LLM、TTS 各阶段耗时。
- 改进 TTS 播放音量和音频增益控制。
- 增加心跳包和更完整的连接状态管理。
- 将全局状态改成 session 结构，为多客户端接入做准备。
