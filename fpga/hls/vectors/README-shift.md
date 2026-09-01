# Sampling-phase vectors (shift25 / shift50)

`clean.iq` delayed by a fraction of a sample via an FFT phase ramp, keeping
`clean.bits` as the expected payload. They exist to reproduce a defect that
only appears on hardware-sourced signals.

`timing_recovery()` in qpsk_demod.cpp computes an early-late error, integrates
it into `tau` -- and never reads `tau`. The symbol instant is therefore frozen
wherever reset left it, and the core cannot re-centre on the eye. Measured:

| vector  | sampling phase   | match  |
|---------|------------------|--------|
| clean   | aligned          | 100.0% |
| shift25 | quarter sample   |  99.6% |
| shift50 | half sample      |  42.3% |

shift25 reproduces the hardware residual almost exactly: over coax the FPGA
demodulator scores 99.4%, failing 6 symbols per 1024 at IDENTICAL positions in
all 255 blocks. Deterministic errors at fixed positions are the signature of a
fixed sampling phase, not of noise.

To use them (the testbench only knows clean/impaired/stress):

    cp shift25.iq clean.iq   # keep a backup of the real clean.iq first
    ./demod_csim ../vectors clean

Native csim needs no Vitis licence:

    g++ -O2 -std=c++17 -w -I$VITIS/include \
        -o demod_csim qpsk_demod.cpp qpsk_demod_tb.cpp

A naive fix -- using `tau` to skip/repeat a sample -- was tried and REVERTED.
It rescues the offset cases but regresses the aligned one, because the loop has
no deadband and keeps hunting after it has centred:

| Kt   | thresh | clean  | shift25 | shift50 |
|------|--------|--------|---------|---------|
| 0.01 | 0.5    | 100.0% |  99.6%  |  42.3%  |  (loop never engages: today)
| 0.05 | 0.1    |  83.3% |  88.5%  |  89.7%  |
| 0.2  | 0.5    |  94.4% |  96.0%  |  96.8%  |

Trading a bit-exact aligned case for offset robustness is not an improvement.
A real fix needs a Gardner TED with a damped PI loop and a fractional
interpolator, so it settles instead of oscillating.
