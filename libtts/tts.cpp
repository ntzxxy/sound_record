#include "tts.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

#include "sherpa-onnx/csrc/offline-tts.h"

static std::unique_ptr<sherpa_onnx::OfflineTts> g_tts;
static int32_t g_sample_rate = 0;
static thread_local tts_callback_t g_user_callback = nullptr;
static thread_local bool g_had_audio = false;
static bool g_use_qwen = false;
static bool g_use_kokoro = false;
static std::string g_qwen_model_dir;
static std::string g_qwen_cli;

static int32_t bridge_callback(const float *samples, int32_t n, float progress) {
    if (!g_user_callback || !samples || n <= 0) return 1;
    g_had_audio = true;
    std::vector<int16_t> pcm(n);
    for (int32_t i = 0; i < n; i++) {
        float s = samples[i];
        if (s > 1.0f)  s = 1.0f;
        if (s < -1.0f) s = -1.0f;
        pcm[i] = static_cast<int16_t>(s * 32767.0f);
    }
    g_user_callback(pcm.data(), n, progress);
    return 1;
}

static bool file_exists(const std::string& path) {
    FILE *f = fopen(path.c_str(), "rb");
    if (f) { fclose(f); return true; }
    return false;
}

static bool is_qwen_model_dir(const std::string& base) {
    return file_exists(base + "/qwen3-tts-0.6b-f16.gguf") &&
           file_exists(base + "/qwen3-tts-tokenizer-f16.gguf") &&
           file_exists(base + "/christina.spk") &&
           file_exists(base + "/christina-zh.spk");
}

static bool is_kokoro_model_dir(const std::string& base) {
    return file_exists(base + "/model.int8.onnx") &&
           file_exists(base + "/voices.bin") &&
           file_exists(base + "/tokens.txt") &&
           file_exists(base + "/lexicon-us-en.txt") &&
           file_exists(base + "/lexicon-zh.txt") &&
           file_exists(base + "/dict") &&
           file_exists(base + "/date-zh.fst") &&
           file_exists(base + "/number-zh.fst") &&
           file_exists(base + "/phone-zh.fst");
}

static bool contains_han(const std::string& text) {
    for (size_t i = 0; i + 2 < text.size(); ++i) {
        const unsigned char c0 = static_cast<unsigned char>(text[i]);
        const unsigned char c1 = static_cast<unsigned char>(text[i + 1]);
        const unsigned char c2 = static_cast<unsigned char>(text[i + 2]);
        if ((c0 >= 0xE4 && c0 <= 0xE9) && (c1 >= 0x80 && c1 <= 0xBF) &&
            (c2 >= 0x80 && c2 <= 0xBF)) {
            return true;
        }
    }
    return false;
}

static uint16_t read_le16(const uint8_t *p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

static uint32_t read_le32(const uint8_t *p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

static bool read_pcm16_wav(const std::string& path, std::vector<int16_t>& pcm, int32_t& sample_rate) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), {});
    if (data.size() < 44 || std::memcmp(data.data(), "RIFF", 4) != 0 ||
        std::memcmp(data.data() + 8, "WAVE", 4) != 0) return false;

    uint16_t channels = 0;
    uint16_t format = 0;
    uint16_t bits = 0;
    uint32_t rate = 0;
    size_t pos = 12;
    while (pos + 8 <= data.size()) {
        const uint8_t *chunk = data.data() + pos;
        const uint32_t size = read_le32(chunk + 4);
        pos += 8;
        if (size > data.size() - pos) return false;
        if (std::memcmp(chunk, "fmt ", 4) == 0 && size >= 16) {
            format = read_le16(data.data() + pos);
            channels = read_le16(data.data() + pos + 2);
            rate = read_le32(data.data() + pos + 4);
            bits = read_le16(data.data() + pos + 14);
        } else if (std::memcmp(chunk, "data", 4) == 0) {
            if (format != 1 || channels != 1 || bits != 16 || rate == 0 || size % 2 != 0) return false;
            pcm.resize(size / 2);
            std::memcpy(pcm.data(), data.data() + pos, size);
            sample_rate = static_cast<int32_t>(rate);
            return !pcm.empty();
        }
        pos += size + (size & 1U);
    }
    return false;
}

static int qwen_speak(const char *text, tts_callback_t callback) {
    const bool chinese = contains_han(text);
    const std::string speaker = g_qwen_model_dir + (chinese ? "/christina-zh.spk" : "/christina.spk");
    char output_template[] = "/tmp/qwen3_tts_XXXXXX";
    const int output_fd = mkstemp(output_template);
    if (output_fd < 0) return -1;
    close(output_fd);

    const char *threads_env = std::getenv("TTS_NUM_THREADS");
    const char *threads = (threads_env && threads_env[0]) ? threads_env : "4";
    const char *language = chinese ? "zh" : "en";
    const char *args[] = {
        g_qwen_cli.c_str(), "-m", g_qwen_model_dir.c_str(), "-s", speaker.c_str(),
        "-l", language, "-t", text, "-o", output_template, "-j", threads,
        "--temperature", "0.7", "--top-k", "20", "--top-p", "0.85",
        "--repetition-penalty", "1.1", nullptr
    };
    const pid_t pid = fork();
    if (pid == 0) {
        execv(args[0], const_cast<char * const *>(args));
        _exit(127);
    }
    int status = 0;
    const bool succeeded = pid > 0 && waitpid(pid, &status, 0) == pid &&
                           WIFEXITED(status) && WEXITSTATUS(status) == 0;
    std::vector<int16_t> pcm;
    int32_t sample_rate = 0;
    const bool wav_ok = succeeded && read_pcm16_wav(output_template, pcm, sample_rate);
    unlink(output_template);
    if (!wav_ok) {
        fprintf(stderr, "[TTS] Qwen3-TTS synthesis failed\n");
        return -1;
    }
    g_sample_rate = sample_rate;
    callback(pcm.data(), static_cast<int>(pcm.size()), 1.0f);
    return 0;
}

int tts_init(const char *model_dir) {
    std::string base = model_dir;
    if (is_qwen_model_dir(base)) {
        const char *cli_env = std::getenv("TTS_QWEN_CLI");
        g_qwen_cli = (cli_env && cli_env[0]) ? cli_env : base + "/qwen3-tts-cli";
        if (!file_exists(g_qwen_cli)) {
            fprintf(stderr, "[TTS] Qwen3-TTS CLI not found: %s (set TTS_QWEN_CLI)\n", g_qwen_cli.c_str());
            return -1;
        }
        g_use_qwen = true;
        g_qwen_model_dir = base;
        g_sample_rate = 24000;
        fprintf(stderr, "[TTS] detected Christina Qwen3-TTS model, cli=%s\n", g_qwen_cli.c_str());
        return 0;
    }
    g_use_qwen = false;
    g_use_kokoro = false;
    sherpa_onnx::OfflineTtsConfig config;
    int num_threads = 1;
    const char *threads_env = std::getenv("TTS_NUM_THREADS");
    if (threads_env && threads_env[0]) {
        int parsed = std::atoi(threads_env);
        if (parsed > 0) num_threads = parsed;
    }
    if (num_threads > 12) num_threads = 12;
    config.model.num_threads = num_threads;
    config.model.provider    = "cpu";
    config.max_num_sentences = 1;  // 按句/chunk 回调，便于边合成边播放

    // P0 uses the Kokoro INT8 Chinese-English model.  The vendored
    // sherpa-onnx revision also supports the legacy VITS configuration below,
    // but it does not expose the newer Supertonic API.
    if (is_kokoro_model_dir(base)) {
        config.model.kokoro.model = base + "/model.int8.onnx";
        config.model.kokoro.voices = base + "/voices.bin";
        config.model.kokoro.tokens = base + "/tokens.txt";
        config.model.kokoro.lexicon = base + "/lexicon-us-en.txt," + base + "/lexicon-zh.txt";
        config.model.kokoro.data_dir = base + "/espeak-ng-data";
        config.model.kokoro.dict_dir = base + "/dict";
        config.rule_fsts = base + "/date-zh.fst," + base + "/number-zh.fst," + base + "/phone-zh.fst";
        g_use_kokoro = true;
        fprintf(stderr, "[TTS] detected Kokoro INT8 Chinese-English model\n");
    } else {
        // 优先用 INT8 模型（52MB，3x 快），否则用 FP32（163MB）
        std::string model  = base + "/model.int8.onnx";
        if (!file_exists(model)) model = base + "/model.onnx";
        std::string tokens = base + "/tokens.txt";
        if (!file_exists(model) || !file_exists(tokens)) {
            fprintf(stderr, "[TTS] 未找到 Kokoro 或 VITS 模型文件\n");
            return -1;
        }
        config.model.vits.model  = model;
        config.model.vits.tokens = tokens;

        // 可选：中文 lexicon 和 dict
        if (file_exists(base + "/lexicon.txt"))
            config.model.vits.lexicon = base + "/lexicon.txt";
        if (file_exists(base + "/dict"))
            config.model.vits.dict_dir = base + "/dict";

        // 可选：FST 规则文件（数字/日期/电话号码等正则）
        std::string fsts;
        for (const char *f : {"date.fst", "number.fst", "phone.fst", "new_heteronym.fst"}) {
            if (file_exists(base + "/" + f))
                fsts += (fsts.empty() ? "" : ",") + base + "/" + f;
        }
        if (!fsts.empty()) config.rule_fsts = fsts;

        fprintf(stderr, "[TTS] 检测到 VITS/Matcha 模型\n");
    }

    try {
        g_tts = std::make_unique<sherpa_onnx::OfflineTts>(config);
        g_sample_rate = g_tts->SampleRate();
        fprintf(stderr, "[TTS] 模型加载成功, sample_rate=%d, threads=%d\n",
                g_sample_rate, num_threads);
        return 0;
    } catch (const std::exception &e) {
        fprintf(stderr, "[TTS] 模型加载失败: %s\n", e.what());
        return -1;
    }
}

int tts_speak(const char *text, tts_callback_t callback) {
    if (!text || !text[0] || !callback) return -1;
    if (g_use_qwen) return qwen_speak(text, callback);
    if (!g_tts) return -1;
    g_user_callback = callback;
    g_had_audio = false;
    try {
        int64_t speaker_id = 0;
        float speed = 1.0f;
        if (g_use_kokoro) {
            const char *speaker_env = std::getenv("KOKORO_SPEAKER_ID");
            const char *speed_env = std::getenv("KOKORO_SPEED");
            speaker_id = (speaker_env && speaker_env[0]) ? std::atoi(speaker_env) : 50;
            speed = (speed_env && speed_env[0]) ? static_cast<float>(std::atof(speed_env)) : 1.10f;
        }
        // The pinned sherpa-onnx version used on Jetson provides this
        // signature.  It is sufficient for Kokoro speaker and speed control.
        g_tts->Generate(std::string(text), speaker_id, speed, bridge_callback);
    } catch (const std::exception &e) {
        fprintf(stderr, "[TTS] 合成失败: %s\n", e.what());
        g_user_callback = nullptr;
        return -1;
    }
    g_user_callback = nullptr;
    return g_had_audio ? 0 : -1;
}

int tts_sample_rate(void) { return g_sample_rate; }
void tts_destroy(void) {
    g_tts.reset();
    g_use_qwen = false;
    g_use_kokoro = false;
    g_qwen_model_dir.clear();
    g_qwen_cli.clear();
    g_sample_rate = 0;
}
