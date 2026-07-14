#include "audio_player.h"

#include <alsa/asoundlib.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <signal.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

static snd_pcm_t *g_pcm_handle = nullptr;
static unsigned int g_rate = 44100;
static unsigned int g_channels = 1;
static std::atomic<bool> g_playing{false};
static std::atomic<bool> g_stop_requested{false};

// WSL/无 ALSA 时的 fallback：写临时 WAV 播放
static bool g_fallback = false;
static pid_t g_fallback_pid = 0;

// ---- WAV 写入 ----
static void write_wav(const char *path, const int16_t *pcm, int n, int sr) {
    std::ofstream f(path, std::ios::binary);
    int db = n * sizeof(int16_t);
    auto w32 = [&](int v) { f.write((char *)&v, 4); };
    auto w16 = [&](short v) { f.write((char *)&v, 2); };
    f.write("RIFF", 4); w32(db + 36);
    f.write("WAVEfmt ", 8); w32(16); w16(1); w16(1);
    w32(sr); w32(sr * 2); w16(2); w16(16);
    f.write("data", 4); w32(db);
    f.write((char *)pcm, db);
}

// ---- ALSA ----
int audio_player_init(const char *device, unsigned int rate, unsigned int channels) {
    g_rate = rate;
    g_channels = channels;
    const char *dev = device ? device : "default";
    int ret = snd_pcm_open(&g_pcm_handle, dev, SND_PCM_STREAM_PLAYBACK, 0);
    if (ret < 0) {
        fprintf(stderr, "[AudioPlayer] ALSA 不可用(%s), 启用 WAV fallback 模式\n",
                snd_strerror(ret));
        g_pcm_handle = nullptr;
        g_fallback = true;
        return 0;  // 不报错，用 fallback
    }
    ret = snd_pcm_set_params(g_pcm_handle, SND_PCM_FORMAT_S16_LE,
                             SND_PCM_ACCESS_RW_INTERLEAVED,
                             channels, rate, 1, 500000);
    if (ret < 0) {
        snd_pcm_close(g_pcm_handle);
        g_pcm_handle = nullptr;
        g_fallback = true;
        fprintf(stderr, "[AudioPlayer] ALSA 参数失败, 启用 WAV fallback\n");
        return 0;
    }
    fprintf(stderr, "[AudioPlayer] ALSA 就绪, device=%s, rate=%d\n", dev, rate);
    return 0;
}

int audio_player_play_chunk(const int16_t *pcm, int num_samples, int drain) {
    if (!pcm || num_samples <= 0) return -1;

    // ---- fallback 模式：写临时 WAV → system 播放 ----
    if (g_fallback) {
        g_playing = true;

        // 检测 WSL
        bool is_wsl = (system("test -f /proc/sys/fs/binfmt_misc/WSLInterop") == 0);

        const char *tmp = is_wsl ? "/mnt/c/Windows/Temp/tts_playback.wav"
                                 : "/tmp/tts_playback.wav";
        write_wav(tmp, pcm, num_samples, g_rate);

        if (is_wsl) {
            // WSL: PowerShell 播放 C:\Windows\Temp\tts_playback.wav
            g_fallback_pid = fork();
            if (g_fallback_pid == 0) {
                execlp("powershell.exe", "powershell.exe", "-c",
                       "(New-Object Media.SoundPlayer 'C:\\Windows\\Temp\\tts_playback.wav').PlaySync()",
                       nullptr);
                _exit(0);
            }
        } else {
            // Linux fallback: aplay
            g_fallback_pid = fork();
            if (g_fallback_pid == 0) {
                execlp("aplay", "aplay", "-q", tmp, nullptr);
                _exit(0);
            }
        }

        if (g_fallback_pid > 0) {
            int status;
            waitpid(g_fallback_pid, &status, 0);  // 阻塞等待播放完成
        }
        g_fallback_pid = 0;
        g_playing = false;
        return 0;
    }

    // ---- ALSA 模式 ----
    if (!g_pcm_handle) return -1;
    g_playing = true;
    g_stop_requested = false;

    const int chunk = 1024;
    int written = 0;
    while (written < num_samples) {
        if (g_stop_requested) {
            snd_pcm_drop(g_pcm_handle);
            snd_pcm_prepare(g_pcm_handle);
            g_playing = false;
            return 0;
        }
        int n = (written + chunk <= num_samples) ? chunk : (num_samples - written);
        int ret = snd_pcm_writei(g_pcm_handle, pcm + written, n);
        if (ret < 0) {
            ret = snd_pcm_recover(g_pcm_handle, ret, 0);
            if (ret < 0) { g_playing = false; return -1; }
            continue;
        }
        written += ret;
    }
    if (drain) snd_pcm_drain(g_pcm_handle);
    g_playing = false;
    return 0;
}

int audio_player_play(const int16_t *pcm, int num_samples) {
    return audio_player_play_chunk(pcm, num_samples, 1);
}

void audio_player_stop(void) {
    if (g_fallback && g_fallback_pid > 0) {
        kill(g_fallback_pid, SIGKILL);
        g_fallback_pid = 0;
    }
    g_stop_requested = true;
}

int audio_player_is_playing(void) { return g_playing ? 1 : 0; }

void audio_player_destroy(void) {
    if (g_pcm_handle) { snd_pcm_close(g_pcm_handle); g_pcm_handle = nullptr; }
}
