# Jetson Orin NX 16GB：从 Git Clone 到运行 P0 本地座舱语音链路

本文用于在 **Jetson Orin NX 16GB / 256GB** 上原生部署本项目当前的 P0
版本。P0 的运行方式是单机本地链路：

```text
ALSA 麦克风 -> 进程内有界队列 -> ASR -> 意图/LLM -> TTS -> ALSA 扬声器
```

`server/`、`libnet/` 和旧 AIV1/TCP 音频协议仍在仓库中保留，但 P0 的
`cockpit_runtime` 不会建立音频 TCP 连接。请使用 **Jetson 板端原生编译**；
不要把 x86 PC 编译出的 CUDA/TensorRT/ONNX Runtime 二进制直接复制到板端。

> 本文命令均在 Jetson 的 Ubuntu 终端执行。`<...>` 是需要替换的占位符。

## 当前代码模型清单（以源码与启动脚本为准）

下表是本项目当前分支的实际默认模型，而非通用推荐或历史备选。

| 模块 | 当前默认模型与文件 | 代码依据 | 官方下载入口 | 是否为 P0 必需 |
| --- | --- | --- | --- | --- |
| 流式 ASR | `sherpa-onnx-streaming-zipformer-small-bilingual-zh-en-2023-02-16` | `libasr/asr.cpp` 固定加载其 encoder、decoder、joiner、tokens | `https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models/sherpa-onnx-streaming-zipformer-small-bilingual-zh-en-2023-02-16.tar.bz2` | 是 |
| 对话 LLM / 意图分类 | **Gemma 4 E4B IT Q4_0**：`gemma-4-E4B-it-Q4_0.gguf` | `run_server.sh:11` 的 `LLM_MODEL` 默认值；`libllm/llm.cpp` 有 Gemma 4 专用 chat template | `https://huggingface.co/ggml-org/gemma-4-E4B-it-GGUF` | 是 |
| 默认 TTS | **Kokoro INT8 multilingual v1.1**：`kokoro-int8-multi-lang-v1_1/` | `run_server.sh:12` 的 `TTS_MODEL` 默认值；`libtts/tts.cpp` 自动识别 `model.int8.onnx` | `https://github.com/k2-fsa/sherpa-onnx/releases/download/tts-models/kokoro-int8-multi-lang-v1_1.tar.bz2` | 是 |
| 目标检测（视觉模块） | **YOLO11n ONNX**：`yolo11n.onnx` | `libvision/yolo_detector.h` 与 `tools/yolo_preview.cpp` 的默认路径 | `https://github.com/ultralytics/assets/releases/download/v8.3.0/yolo11n.pt`（下载后导出 ONNX） | 否，P0 关闭视觉 |
| 视觉语言 VLM | Gemma 4 E4B IT Q4_0 + `mmproj-gemma-4-E4B-it-Q8_0.gguf` | `libvision/scene_analyzer.h` | `https://huggingface.co/ggml-org/gemma-4-E4B-it-GGUF` | 否，需 `BUILD_VLM=ON` |

**本次确认结论：** 仅语音 P0 必须下载 ASR、`gemma-4-E4B-it-Q4_0.gguf` 与 Kokoro
三项；YOLO、`mmproj`、Qwen/DeepSeek/Christina/Supertonic 等均不应作为首次跑通的
前置下载项。

仓库中还能看到 DeepSeek、Qwen、Qwen3-TTS/Christina、Supertonic、VITS 和 Gemma
QAT 等文件或兼容代码；它们均不是当前默认启动路径，不能替代本节列出的模型来验证
当前 P0。

## 0. 前置条件

1. 已使用与 Orin NX 匹配的 JetPack 镜像刷机并完成首次启动。
2. Jetson 可以访问互联网（首次编译和模型下载都需要网络）。
3. 已接好 USB 麦克风、扬声器；GPIO 按键可以稍后接入。
4. 建议预留至少 30GB 空间给源码、构建缓存、依赖与模型。

先确认 JetPack/CUDA 可用：

```bash
cat /etc/nv_tegra_release
nvcc -V
sudo tegrastats
```

如果没有 `nvcc` 或 `tegrastats`，请先完成 JetPack 安装。CUDA、TensorRT 和
相机驱动必须与当前 JetPack 保持匹配。

## 1. 安装系统依赖

```bash
sudo apt update
sudo apt install -y \
  git build-essential cmake ninja-build pkg-config \
  libasound2-dev alsa-utils \
  portaudio19-dev \
  wget curl bzip2 unzip ca-certificates \
  python3-pip \
  v4l-utils
```

为当前用户授予声卡、摄像头和输入设备权限：

```bash
sudo usermod -aG audio,video,render,input "$USER"
```

执行后需要退出当前桌面会话并重新登录，或重启系统：

```bash
sudo reboot
```

## 2. Clone 项目及子模块

```bash
cd ~/xuyi

git clone --branch edge --single-branch --recurse-submodules \
  https://github.com/ntzxxy/sound_record.git
cd sound_record
```

检查 P0 运行时源文件已经存在：

```bash
test -f apps/cockpit_runtime/main.cpp
test -f libpipeline/local_voice_pipeline.cpp
```

如果这里失败，请切换到包含 P0 改造的分支或提交后再继续。

## 3. 建立模型目录

模型不提交到 Git 仓库。推荐将模型放在项目外，以免误提交且便于多个构建目录复用：

```bash
export MODEL_ROOT="$HOME/models/smart-cockpit"
mkdir -p "$MODEL_ROOT/asr" "$MODEL_ROOT/llm" "$MODEL_ROOT/tts"
```

可将下面一行加入 `~/.bashrc`，以后登录自动生效：

```bash
echo 'export MODEL_ROOT="$HOME/models/smart-cockpit"' >> ~/.bashrc
source ~/.bashrc
```

目录完成后的目标结构：

```text
$MODEL_ROOT/
├── asr/
│   └── sherpa-onnx-streaming-zipformer-small-bilingual-zh-en-2023-02-16/
├── llm/
│   └── gemma-4-E4B-it-Q4_0.gguf
└── tts/
    └── kokoro-int8-multi-lang-v1_1/
```

## 4. 下载运行所需模型

### 4.1 ASR：中英双语流式 Zipformer

该模型与 `libasr/asr.cpp` 当前使用的 encoder、decoder、joiner 文件名一致。

```bash
cd "$MODEL_ROOT/asr"
wget https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models/sherpa-onnx-streaming-zipformer-small-bilingual-zh-en-2023-02-16.tar.bz2
tar xf sherpa-onnx-streaming-zipformer-small-bilingual-zh-en-2023-02-16.tar.bz2
rm sherpa-onnx-streaming-zipformer-small-bilingual-zh-en-2023-02-16.tar.bz2
```

校验文件：

```bash
ASR_DIR="$MODEL_ROOT/asr/sherpa-onnx-streaming-zipformer-small-bilingual-zh-en-2023-02-16"
test -f "$ASR_DIR/encoder-epoch-99-avg-1.onnx"
test -f "$ASR_DIR/decoder-epoch-99-avg-1.onnx"
test -f "$ASR_DIR/joiner-epoch-99-avg-1.onnx"
test -f "$ASR_DIR/tokens.txt"
```

### 4.2 TTS：Kokoro INT8 中英模型

P0 先使用 CPU TTS，避免与 LLM 争用 GPU。Kokoro INT8 的内存和速度适合作为
第一版端侧播报方案。

```bash
cd "$MODEL_ROOT/tts"
wget https://github.com/k2-fsa/sherpa-onnx/releases/download/tts-models/kokoro-int8-multi-lang-v1_1.tar.bz2
tar xf kokoro-int8-multi-lang-v1_1.tar.bz2
rm kokoro-int8-multi-lang-v1_1.tar.bz2
```

校验文件：

```bash
TTS_DIR="$MODEL_ROOT/tts/kokoro-int8-multi-lang-v1_1"
test -f "$TTS_DIR/model.int8.onnx"
test -f "$TTS_DIR/voices.bin"
test -f "$TTS_DIR/tokens.txt"
test -d "$TTS_DIR/espeak-ng-data"
```

### 4.3 默认 LLM：Gemma 4 E4B IT，Q4_0 GGUF

这是当前 `run_server.sh` 和项目文档的默认对话/意图模型；`libllm` 会根据 GGUF
元数据识别 `gemma4` 架构，并应用项目中实现的 Gemma 4 chat template。下载地址：

```text
https://huggingface.co/ggml-org/gemma-4-E4B-it-GGUF
```

下载当前代码默认的精确文件：

```bash
python3 -m pip install --user -U "huggingface_hub[cli]"

~/.local/bin/hf download \
  ggml-org/gemma-4-E4B-it-GGUF \
  gemma-4-E4B-it-Q4_0.gguf \
  --local-dir "$MODEL_ROOT/llm"
```

校验文件：

```bash
LLM_MODEL="$MODEL_ROOT/llm/gemma-4-E4B-it-Q4_0.gguf"
test -s "$LLM_MODEL"
ls -lh "$LLM_MODEL"
```

> 该文件约 4.3GB。Orin NX 16GB 可以先以“仅语音 P0”模式运行它；在尚未完成
> 性能 benchmark 前，不要同时启用 VLM 与大分辨率视觉推理。

### 4.4 可选 VLM 投影模型：Gemma 4 mmproj

此文件仅在 `BUILD_VLM=ON` 并运行 `yolo_preview --enable-vlm` 等视觉问答路径时
需要；纯 P0 语音运行时不加载它。下载地址与主模型相同：

```bash
mkdir -p "$MODEL_ROOT/vlm"

~/.local/bin/hf download \
  ggml-org/gemma-4-E4B-it-GGUF \
  mmproj-gemma-4-E4B-it-Q8_0.gguf \
  --local-dir "$MODEL_ROOT/vlm"
```

视觉模块默认的相对路径是：

```text
models/gemma-4-E4B-it/mmproj-gemma-4-E4B-it-Q8_0.gguf
```

若模型放在 `$MODEL_ROOT`，启动视觉工具时必须以 `--vlm-model` 和 `--mmproj`
传入这两个绝对路径，不能依赖默认相对路径。

### 4.5 可选目标检测模型：YOLO11n

视觉检测默认加载 `models/vision/yolo11n.onnx`。原始权重官方下载地址：

```text
https://github.com/ultralytics/assets/releases/download/v8.3.0/yolo11n.pt
```

项目运行时需要 ONNX 文件，建议在 Jetson 或已配置的开发机上从该权重导出：

```bash
mkdir -p "$MODEL_ROOT/vision"
wget -O "$MODEL_ROOT/vision/yolo11n.pt" \
  https://github.com/ultralytics/assets/releases/download/v8.3.0/yolo11n.pt

# 需先在当前 Python 环境安装与 JetPack/CUDA 匹配的 PyTorch，以及：
python3 -m pip install -r tools/vision/requirements.txt
( cd "$MODEL_ROOT/vision" && python3 - <<'PY'
from ultralytics import YOLO

model = YOLO("yolo11n.pt")
model.export(format="onnx", imgsz=640, opset=13, simplify=True)
PY
)
```

运行视觉工具时使用：

```bash
./build-jetson/bin/yolo_preview --model "$MODEL_ROOT/vision/yolo11n.onnx"
```

### 4.6 代码支持但非默认的 TTS 模型

`libtts/tts.cpp` 还支持下列模型，仅用于后续质量/性能对比，不是运行当前 P0 的前置条件：

| 模型 | 下载地址 | 备注 |
| --- | --- | --- |
| Christina Qwen3-TTS 1.5 Q4 | https://huggingface.co/Loke-60000/christina-TTS-1.5-q4 | 需要同时构建 `third_party/qwen3-tts.cpp`；项目以 `qwen3-tts-0.6b-f16.gguf`、tokenizer 与 `christina-zh.spk` 识别它。 |
| Supertonic 3 INT8 | https://github.com/k2-fsa/sherpa-onnx/releases/download/tts-models/sherpa-onnx-supertonic-3-tts-int8-2026-05-11.tar.bz2 | `tts.cpp` 可由 `tts.json` 自动识别。 |
| VITS 中文模型 | 仅保留兼容分支；当前仓库未提供可复现的固定下载来源 | 不作为部署默认项。 |

### 4.7 项目中可选的 Gemma 4 QAT 模型

`models/gemma-4-E4B_q4_0-it.gguf` 是项目中存在的可选 QAT 文件，但当前默认启动
脚本并不选择它。官方 QAT GGUF 的入口为：

```text
https://huggingface.co/google/gemma-4-E4B-it-qat-q4_0-gguf
```

该模型可能要求先在 Hugging Face 页面接受 Gemma 使用条款并执行 `hf auth login`。
下载后可将其绝对路径作为 `cockpit_runtime` 的第二个参数，或通过 `LLM_MODEL` 覆盖
旧 `run_server.sh` 的默认值。

## 5. 原生编译 P0 运行时

回到项目根目录：

```bash
cd ~/workspace/sound_record
```

Orin NX 是 CUDA compute capability 8.7；使用 Release、CUDA 和较保守的四线程
构建。P0 阶段关闭旧服务端和视觉模块，缩短首次验证时间。

```bash
cmake -S . -B build-jetson -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DGGML_CUDA=ON \
  -DCMAKE_CUDA_ARCHITECTURES=87 \
  -DBUILD_BOARD_CLIENT=ON \
  -DBUILD_COCKPIT_RUNTIME=ON \
  -DBUILD_SERVER=OFF \
  -DBUILD_VISION=OFF \
  -DBUILD_DESKTOP_CLIENT=OFF

cmake --build build-jetson --target cockpit_runtime -j4
```

成功后确认二进制：

```bash
test -x build-jetson/bin/cockpit_runtime
```

构建日志中应出现 CUDA backend；若没有，先确认 `nvcc -V` 正常，再删除
`build-jetson/` 后重新执行本节的 CMake 配置命令。

## 6. 在没有模型/硬件时先运行 P0 队列测试

下面测试不打开 ALSA，不加载 ASR/LLM/TTS 模型。它构造 100ms 的 16kHz 单声道假
PCM，验证：`录音开始 -> PCM -> ASR 接口 -> 会话提交`，以及录音开始时 TTS 打断。

```bash
cd ~/workspace/sound_record

cmake -S . -B build-p0-test -G Ninja \
  -DBUILD_ASR=OFF \
  -DBUILD_LLM=OFF \
  -DBUILD_SERVER=OFF \
  -DBUILD_BOARD_CLIENT=OFF \
  -DBUILD_VISION=OFF \
  -DBUILD_DESKTOP_CLIENT=OFF \
  -DBUILD_COCKPIT_RUNTIME=OFF

cmake --build build-p0-test --target test_local_voice_pipeline -j4
./build-p0-test/bin/test_local_voice_pipeline
```

预期输出：

```text
[TestLocalVoicePipeline] fake PCM -> ASR -> conversation passed
```

## 7. 检查声卡、麦克风、按键

列出系统检测到的声卡和录音设备：

```bash
arecord -l
aplay -l
```

当前 P0 的 USB 默认值是 `plughw:1,0`、16kHz、单声道。先独立录制并回放验证：

```bash
arecord -D plughw:1,0 -f S16_LE -r 16000 -c 1 -d 3 /tmp/mic-test.wav
aplay /tmp/mic-test.wav
```

如果 `arecord -l` 显示的卡号、设备号不是 `1,0`，修改
`libaudio/audio.cpp` 中的 `AUDIO_CFG_USB.device`，例如：

```cpp
.device = "plughw:2,0",
```

然后重新执行第 5 节的构建命令。

按键设备同样需要确认：

```bash
ls -l /dev/input/by-path
sudo apt install -y evtest
sudo evtest
```

当前 P0 入口暂时使用旧板子的固定路径：

```text
/dev/input/by-path/platform-gpio_keys@0-event
```

如果 Jetson 的 GPIO 按键不使用该路径，`cockpit_runtime` 会在启动时提示初始化失败。
第一次带通时可先接入一个可产生按下/松开事件的 USB 按键，或在下一次改造中将路径改为
`COCKPIT_KEY_DEVICE` 环境变量，避免硬编码。

## 8. 启动 P0 本地运行时

```bash
cd ~/workspace/sound_record

export ASR_DIR="$MODEL_ROOT/asr/sherpa-onnx-streaming-zipformer-small-bilingual-zh-en-2023-02-16"
export LLM_MODEL="$MODEL_ROOT/llm/gemma-4-E4B-it-Q4_0.gguf"
export TTS_DIR="$MODEL_ROOT/tts/kokoro-int8-multi-lang-v1_1"

# sherpa-onnx 下载的 ONNX Runtime 动态库路径。
export LD_LIBRARY_PATH="$PWD/build-jetson/_deps/onnxruntime-src/lib:${LD_LIBRARY_PATH}"

# P0：TTS 使用 CPU，LLM 使用 llama.cpp CUDA。
export TTS_NUM_THREADS=4
export KOKORO_SPEAKER_ID=3
export KOKORO_SPEED=1.15

./build-jetson/bin/cockpit_runtime "$ASR_DIR" "$LLM_MODEL" "$TTS_DIR" usb
```

启动成功后，按下按键开始录音，松开按键结束录音。运行时应：

1. 打断正在播报的 TTS；
2. 接收本地 ALSA PCM；
3. 在本机完成 ASR、意图分类和 LLM 回复；
4. 用本地 ALSA 播放 TTS。

可在另一个终端观察资源使用：

```bash
sudo tegrastats
```

建议通过 `sudo nvpmodel -q --verbose` 查看板端可用功耗档位；确认散热条件充足后再选择
最高性能档，并执行：

```bash
sudo jetson_clocks
```

## 9. 常见问题

### `cockpit_runtime` 没有生成

确认配置时未关闭 `BUILD_BOARD_CLIENT`。P0 目前复用 `audio_lib` 和保留的 `net_lib`，
因此两者必须由 Linux 板端构建出来。

### 运行时找不到 `libonnxruntime.so`

重新设置第 8 节中的 `LD_LIBRARY_PATH`，并确认路径存在：

```bash
ls build-jetson/_deps/onnxruntime-src/lib/libonnxruntime.so
```

### `ALSA` 打不开麦克风

先用第 7 节的 `arecord` 命令单独验证；再核对 `AUDIO_CFG_USB` 中的卡号、采样率与声道。
P0 的 ASR 只支持 16kHz 单声道。现有 WM8960 的 44.1kHz 双声道配置需要先加入重采样，
不应直接用于 P0。

### LLM 内存不足或系统卡顿

先确认使用的是默认 `gemma-4-E4B-it-Q4_0.gguf`，并保持视觉/VLM 关闭；再降低
`libllm/llm.cpp` 中的上下文长度或暂时切换到仓库已有的小模型进行排障。不要把
“临时小模型”写回默认部署命令，以免与当前 Gemma 4 主链路不一致。

### 没有声音或音色异常

确认 `$TTS_DIR` 内同时存在 `model.int8.onnx`、`voices.bin`、`tokens.txt` 与
`espeak-ng-data/`。再用 `aplay -l` 检查默认播放设备。

## 10. P0 跑通后的下一步

1. 将按键和声卡配置改为环境变量/配置文件。
2. 为 ASR 加入 44.1kHz 双声道到 16kHz 单声道的重采样。
3. 开启 `BUILD_VISION`，实现 USB V4L2 与 CSI Argus/GStreamer 相机后端。
4. 将 YOLO ONNX 转成在本机生成的 TensorRT FP16 engine。
5. 新增 Web HMI、动作白名单和串口/MCU 模拟执行器。
6. 最后再封装 Docker；容器必须复用宿主 JetPack 驱动并挂载 `/dev/snd`、`/dev/video*`、
   GPIO/串口设备。
