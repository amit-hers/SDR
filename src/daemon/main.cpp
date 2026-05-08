#include "Config.hpp"
#include "Controller.hpp"
#include <iostream>
#include <cstring>

static void usage(const char* prog) {
    std::cerr << "Usage: " << prog << " --config <path/to/config.json>\n"
              << "       " << prog << " --help\n";
}

int main(int argc, char* argv[]) {
    std::string config_path = "config.json";

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--config") == 0 && i + 1 < argc)
            config_path = argv[++i];
        else if (std::strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            usage(argv[0]);
            return 1;
        }
    }

    try {
        auto cfg = sdr::Config::fromFile(config_path);
        sdr::Controller ctrl(std::move(cfg));
        ctrl.run();
    } catch (const std::exception& e) {
        std::cerr << "[sdr] Fatal: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
