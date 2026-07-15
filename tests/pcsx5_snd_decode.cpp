#include "ui/snd_player.h"
#include <cstdio>
#include <fstream>
#include <vector>
#include <string>

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::printf("Usage: pcsx5_snd_decode <input_file> <output_wav>\n");
        return 1;
    }
    std::string input = argv[1];
    std::string output = argv[2];

    std::vector<std::uint8_t> wav;
    if (input.size() > 4 && input.compare(input.size() - 4, 4, ".at9") == 0) {
        wav = Ui::SndPreviewPlayer::DecodeAt9ToWav(input);
    } else if (input.size() > 4 && input.compare(input.size() - 4, 4, ".ogg") == 0) {
        wav = Ui::SndPreviewPlayer::DecodeOggToWav(input);
    } else {
        std::printf("Unsupported format: %s\n", input.c_str());
        return 4;
    }

    if (wav.empty()) {
        std::printf("Failed to decode: %s\n", input.c_str());
        return 2;
    }

    std::ofstream f(output, std::ios::binary);
    if (!f) {
        std::printf("Failed to open output %s\n", output.c_str());
        return 3;
    }
    f.write(reinterpret_cast<const char*>(wav.data()), wav.size());
    std::printf("Decoded successfully: %zu bytes\n", wav.size());
    return 0;
}
