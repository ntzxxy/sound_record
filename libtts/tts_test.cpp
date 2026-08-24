#include "tts.h"

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

static std::vector<int16_t> g_test_audio;

static void test_callback(const int16_t *pcm, int n, float progress) {
    g_test_audio.insert(g_test_audio.end(), pcm, pcm + n);
    fprintf(stderr, "[test] progress %.0f%%, %d samples\n", progress * 100, n);
}

int main(int argc, char *argv[]) {
    const char *model_dir = (argc > 1) ? argv[1] : "../models/supertonic-tts";
    const char *text      = (argc > 2) ? argv[2] : "你好，我是语音助手。";
    const char *out_wav   = (argc > 3) ? argv[3] : "tts_output.wav";

    if (tts_init(model_dir) != 0) return 1;
    fprintf(stderr, "[test] sample_rate=%d\n", tts_sample_rate());

    if (tts_speak(text, test_callback) != 0 || g_test_audio.empty()) {
        fprintf(stderr, "[test] synthesis failed or produced no audio\n");
        tts_destroy();
        return 2;
    }

    int sr = tts_sample_rate();
    int data_bytes = g_test_audio.size() * sizeof(int16_t);
    std::ofstream f(out_wav, std::ios::binary);
    auto w32 = [&](int v) { f.write((char *)&v, 4); };
    auto w16 = [&](short v) { f.write((char *)&v, 2); };
    f.write("RIFF", 4); w32(data_bytes + 36);
    f.write("WAVEfmt ", 8); w32(16); w16(1); w16(1);
    w32(sr); w32(sr * 2); w16(2); w16(16);
    f.write("data", 4); w32(data_bytes);
    f.write((char *)g_test_audio.data(), data_bytes);
    f.close();

    fprintf(stderr, "[test] saved %s (%d samples, %d Hz)\n",
            out_wav, (int)g_test_audio.size(), sr);
    tts_destroy();
    return 0;
}
