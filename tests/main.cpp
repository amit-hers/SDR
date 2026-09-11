// Test runner: each suite is a separate translation unit exposing run_X().
// Usage: sdr-tests [framing|fec|modem|crypto|dsp]
//        (no arg → run all)
#include <iostream>
#include <string>
#include <cstring>

void run_framing();
void run_fec();
void run_modem();
void run_crypto();
void run_dsp();
void run_arq();
void run_splitmod();
void run_aggregate();
void run_scramble();
void run_telemetry();
void run_offsets();
void run_packetreader();

int main(int argc, char* argv[]) {
    const char* filter = (argc > 1) ? argv[1] : nullptr;

    auto run = [&](const char* name, void(*fn)()) {
        if (!filter || std::strcmp(filter, name) == 0) fn();
    };

    try {
        run("framing", run_framing);
        run("fec",     run_fec);
        run("modem",   run_modem);
        run("crypto",  run_crypto);
        run("dsp",     run_dsp);
        run("arq",     run_arq);
        run("splitmod", run_splitmod);
        run("aggregate", run_aggregate);
        run("scramble", run_scramble);
        run("telemetry", run_telemetry);
        run("offsets", run_offsets);
        run("packetreader", run_packetreader);
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
