#include "qpsk_common.h"
#include "hls_math.h"

/* ── NCO sin/cos ROM ────────────────────────────────────────────────
 * 512 entries over one full turn: an angular step of 0.703 degrees, so phase
 * quantisation is +-0.352 degrees against QPSK's 45 degree decision margin.
 * Replaces hls::cos/hls::sin, whose CORDIC expansion was the demodulator's
 * critical path -- see costas_loop below.
 *
 * Generated, not hand-written: NCO_COS[k] = cos(2*pi*k/512).
 */
#define NCO_BITS 9
#define NCO_SIZE (1 << NCO_BITS)
/* Table values reach exactly +-1.0, which fixp_t (ap_fixed<16,1>, range
 * [-1,1)) cannot represent. Two integer bits, 16 fractional. */
typedef ap_fixed<18, 2> trig_t;
/* Index arithmetic: (phase + pi) * (NCO_SIZE / 2pi) reaches 512, so it needs
 * 10 integer bits plus sign -- phase_t's four would overflow silently. */
typedef ap_fixed<32, 11> nco_t;

static const trig_t NCO_COS[NCO_SIZE] = {
    +1.00000000f, +0.99992470f, +0.99969882f, +0.99932238f, +0.99879546f, +0.99811811f, +0.99729046f, +0.99631261f,
    +0.99518473f, +0.99390697f, +0.99247953f, +0.99090264f, +0.98917651f, +0.98730142f, +0.98527764f, +0.98310549f,
    +0.98078528f, +0.97831737f, +0.97570213f, +0.97293995f, +0.97003125f, +0.96697647f, +0.96377607f, +0.96043052f,
    +0.95694034f, +0.95330604f, +0.94952818f, +0.94560733f, +0.94154407f, +0.93733901f, +0.93299280f, +0.92850608f,
    +0.92387953f, +0.91911385f, +0.91420976f, +0.90916798f, +0.90398929f, +0.89867447f, +0.89322430f, +0.88763962f,
    +0.88192126f, +0.87607009f, +0.87008699f, +0.86397286f, +0.85772861f, +0.85135519f, +0.84485357f, +0.83822471f,
    +0.83146961f, +0.82458930f, +0.81758481f, +0.81045720f, +0.80320753f, +0.79583690f, +0.78834643f, +0.78073723f,
    +0.77301045f, +0.76516727f, +0.75720885f, +0.74913639f, +0.74095113f, +0.73265427f, +0.72424708f, +0.71573083f,
    +0.70710678f, +0.69837625f, +0.68954054f, +0.68060100f, +0.67155895f, +0.66241578f, +0.65317284f, +0.64383154f,
    +0.63439328f, +0.62485949f, +0.61523159f, +0.60551104f, +0.59569930f, +0.58579786f, +0.57580819f, +0.56573181f,
    +0.55557023f, +0.54532499f, +0.53499762f, +0.52458968f, +0.51410274f, +0.50353838f, +0.49289819f, +0.48218377f,
    +0.47139674f, +0.46053871f, +0.44961133f, +0.43861624f, +0.42755509f, +0.41642956f, +0.40524131f, +0.39399204f,
    +0.38268343f, +0.37131719f, +0.35989504f, +0.34841868f, +0.33688985f, +0.32531029f, +0.31368174f, +0.30200595f,
    +0.29028468f, +0.27851969f, +0.26671276f, +0.25486566f, +0.24298018f, +0.23105811f, +0.21910124f, +0.20711138f,
    +0.19509032f, +0.18303989f, +0.17096189f, +0.15885814f, +0.14673047f, +0.13458071f, +0.12241068f, +0.11022221f,
    +0.09801714f, +0.08579731f, +0.07356456f, +0.06132074f, +0.04906767f, +0.03680722f, +0.02454123f, +0.01227154f,
    +0.00000000f, -0.01227154f, -0.02454123f, -0.03680722f, -0.04906767f, -0.06132074f, -0.07356456f, -0.08579731f,
    -0.09801714f, -0.11022221f, -0.12241068f, -0.13458071f, -0.14673047f, -0.15885814f, -0.17096189f, -0.18303989f,
    -0.19509032f, -0.20711138f, -0.21910124f, -0.23105811f, -0.24298018f, -0.25486566f, -0.26671276f, -0.27851969f,
    -0.29028468f, -0.30200595f, -0.31368174f, -0.32531029f, -0.33688985f, -0.34841868f, -0.35989504f, -0.37131719f,
    -0.38268343f, -0.39399204f, -0.40524131f, -0.41642956f, -0.42755509f, -0.43861624f, -0.44961133f, -0.46053871f,
    -0.47139674f, -0.48218377f, -0.49289819f, -0.50353838f, -0.51410274f, -0.52458968f, -0.53499762f, -0.54532499f,
    -0.55557023f, -0.56573181f, -0.57580819f, -0.58579786f, -0.59569930f, -0.60551104f, -0.61523159f, -0.62485949f,
    -0.63439328f, -0.64383154f, -0.65317284f, -0.66241578f, -0.67155895f, -0.68060100f, -0.68954054f, -0.69837625f,
    -0.70710678f, -0.71573083f, -0.72424708f, -0.73265427f, -0.74095113f, -0.74913639f, -0.75720885f, -0.76516727f,
    -0.77301045f, -0.78073723f, -0.78834643f, -0.79583690f, -0.80320753f, -0.81045720f, -0.81758481f, -0.82458930f,
    -0.83146961f, -0.83822471f, -0.84485357f, -0.85135519f, -0.85772861f, -0.86397286f, -0.87008699f, -0.87607009f,
    -0.88192126f, -0.88763962f, -0.89322430f, -0.89867447f, -0.90398929f, -0.90916798f, -0.91420976f, -0.91911385f,
    -0.92387953f, -0.92850608f, -0.93299280f, -0.93733901f, -0.94154407f, -0.94560733f, -0.94952818f, -0.95330604f,
    -0.95694034f, -0.96043052f, -0.96377607f, -0.96697647f, -0.97003125f, -0.97293995f, -0.97570213f, -0.97831737f,
    -0.98078528f, -0.98310549f, -0.98527764f, -0.98730142f, -0.98917651f, -0.99090264f, -0.99247953f, -0.99390697f,
    -0.99518473f, -0.99631261f, -0.99729046f, -0.99811811f, -0.99879546f, -0.99932238f, -0.99969882f, -0.99992470f,
    -1.00000000f, -0.99992470f, -0.99969882f, -0.99932238f, -0.99879546f, -0.99811811f, -0.99729046f, -0.99631261f,
    -0.99518473f, -0.99390697f, -0.99247953f, -0.99090264f, -0.98917651f, -0.98730142f, -0.98527764f, -0.98310549f,
    -0.98078528f, -0.97831737f, -0.97570213f, -0.97293995f, -0.97003125f, -0.96697647f, -0.96377607f, -0.96043052f,
    -0.95694034f, -0.95330604f, -0.94952818f, -0.94560733f, -0.94154407f, -0.93733901f, -0.93299280f, -0.92850608f,
    -0.92387953f, -0.91911385f, -0.91420976f, -0.90916798f, -0.90398929f, -0.89867447f, -0.89322430f, -0.88763962f,
    -0.88192126f, -0.87607009f, -0.87008699f, -0.86397286f, -0.85772861f, -0.85135519f, -0.84485357f, -0.83822471f,
    -0.83146961f, -0.82458930f, -0.81758481f, -0.81045720f, -0.80320753f, -0.79583690f, -0.78834643f, -0.78073723f,
    -0.77301045f, -0.76516727f, -0.75720885f, -0.74913639f, -0.74095113f, -0.73265427f, -0.72424708f, -0.71573083f,
    -0.70710678f, -0.69837625f, -0.68954054f, -0.68060100f, -0.67155895f, -0.66241578f, -0.65317284f, -0.64383154f,
    -0.63439328f, -0.62485949f, -0.61523159f, -0.60551104f, -0.59569930f, -0.58579786f, -0.57580819f, -0.56573181f,
    -0.55557023f, -0.54532499f, -0.53499762f, -0.52458968f, -0.51410274f, -0.50353838f, -0.49289819f, -0.48218377f,
    -0.47139674f, -0.46053871f, -0.44961133f, -0.43861624f, -0.42755509f, -0.41642956f, -0.40524131f, -0.39399204f,
    -0.38268343f, -0.37131719f, -0.35989504f, -0.34841868f, -0.33688985f, -0.32531029f, -0.31368174f, -0.30200595f,
    -0.29028468f, -0.27851969f, -0.26671276f, -0.25486566f, -0.24298018f, -0.23105811f, -0.21910124f, -0.20711138f,
    -0.19509032f, -0.18303989f, -0.17096189f, -0.15885814f, -0.14673047f, -0.13458071f, -0.12241068f, -0.11022221f,
    -0.09801714f, -0.08579731f, -0.07356456f, -0.06132074f, -0.04906767f, -0.03680722f, -0.02454123f, -0.01227154f,
    -0.00000000f, +0.01227154f, +0.02454123f, +0.03680722f, +0.04906767f, +0.06132074f, +0.07356456f, +0.08579731f,
    +0.09801714f, +0.11022221f, +0.12241068f, +0.13458071f, +0.14673047f, +0.15885814f, +0.17096189f, +0.18303989f,
    +0.19509032f, +0.20711138f, +0.21910124f, +0.23105811f, +0.24298018f, +0.25486566f, +0.26671276f, +0.27851969f,
    +0.29028468f, +0.30200595f, +0.31368174f, +0.32531029f, +0.33688985f, +0.34841868f, +0.35989504f, +0.37131719f,
    +0.38268343f, +0.39399204f, +0.40524131f, +0.41642956f, +0.42755509f, +0.43861624f, +0.44961133f, +0.46053871f,
    +0.47139674f, +0.48218377f, +0.49289819f, +0.50353838f, +0.51410274f, +0.52458968f, +0.53499762f, +0.54532499f,
    +0.55557023f, +0.56573181f, +0.57580819f, +0.58579786f, +0.59569930f, +0.60551104f, +0.61523159f, +0.62485949f,
    +0.63439328f, +0.64383154f, +0.65317284f, +0.66241578f, +0.67155895f, +0.68060100f, +0.68954054f, +0.69837625f,
    +0.70710678f, +0.71573083f, +0.72424708f, +0.73265427f, +0.74095113f, +0.74913639f, +0.75720885f, +0.76516727f,
    +0.77301045f, +0.78073723f, +0.78834643f, +0.79583690f, +0.80320753f, +0.81045720f, +0.81758481f, +0.82458930f,
    +0.83146961f, +0.83822471f, +0.84485357f, +0.85135519f, +0.85772861f, +0.86397286f, +0.87008699f, +0.87607009f,
    +0.88192126f, +0.88763962f, +0.89322430f, +0.89867447f, +0.90398929f, +0.90916798f, +0.91420976f, +0.91911385f,
    +0.92387953f, +0.92850608f, +0.93299280f, +0.93733901f, +0.94154407f, +0.94560733f, +0.94952818f, +0.95330604f,
    +0.95694034f, +0.96043052f, +0.96377607f, +0.96697647f, +0.97003125f, +0.97293995f, +0.97570213f, +0.97831737f,
    +0.98078528f, +0.98310549f, +0.98527764f, +0.98730142f, +0.98917651f, +0.99090264f, +0.99247953f, +0.99390697f,
    +0.99518473f, +0.99631261f, +0.99729046f, +0.99811811f, +0.99879546f, +0.99932238f, +0.99969882f, +0.99992470f,
};

static const trig_t NCO_SIN[NCO_SIZE] = {
    +0.00000000f, +0.01227154f, +0.02454123f, +0.03680722f, +0.04906767f, +0.06132074f, +0.07356456f, +0.08579731f,
    +0.09801714f, +0.11022221f, +0.12241068f, +0.13458071f, +0.14673047f, +0.15885814f, +0.17096189f, +0.18303989f,
    +0.19509032f, +0.20711138f, +0.21910124f, +0.23105811f, +0.24298018f, +0.25486566f, +0.26671276f, +0.27851969f,
    +0.29028468f, +0.30200595f, +0.31368174f, +0.32531029f, +0.33688985f, +0.34841868f, +0.35989504f, +0.37131719f,
    +0.38268343f, +0.39399204f, +0.40524131f, +0.41642956f, +0.42755509f, +0.43861624f, +0.44961133f, +0.46053871f,
    +0.47139674f, +0.48218377f, +0.49289819f, +0.50353838f, +0.51410274f, +0.52458968f, +0.53499762f, +0.54532499f,
    +0.55557023f, +0.56573181f, +0.57580819f, +0.58579786f, +0.59569930f, +0.60551104f, +0.61523159f, +0.62485949f,
    +0.63439328f, +0.64383154f, +0.65317284f, +0.66241578f, +0.67155895f, +0.68060100f, +0.68954054f, +0.69837625f,
    +0.70710678f, +0.71573083f, +0.72424708f, +0.73265427f, +0.74095113f, +0.74913639f, +0.75720885f, +0.76516727f,
    +0.77301045f, +0.78073723f, +0.78834643f, +0.79583690f, +0.80320753f, +0.81045720f, +0.81758481f, +0.82458930f,
    +0.83146961f, +0.83822471f, +0.84485357f, +0.85135519f, +0.85772861f, +0.86397286f, +0.87008699f, +0.87607009f,
    +0.88192126f, +0.88763962f, +0.89322430f, +0.89867447f, +0.90398929f, +0.90916798f, +0.91420976f, +0.91911385f,
    +0.92387953f, +0.92850608f, +0.93299280f, +0.93733901f, +0.94154407f, +0.94560733f, +0.94952818f, +0.95330604f,
    +0.95694034f, +0.96043052f, +0.96377607f, +0.96697647f, +0.97003125f, +0.97293995f, +0.97570213f, +0.97831737f,
    +0.98078528f, +0.98310549f, +0.98527764f, +0.98730142f, +0.98917651f, +0.99090264f, +0.99247953f, +0.99390697f,
    +0.99518473f, +0.99631261f, +0.99729046f, +0.99811811f, +0.99879546f, +0.99932238f, +0.99969882f, +0.99992470f,
    +1.00000000f, +0.99992470f, +0.99969882f, +0.99932238f, +0.99879546f, +0.99811811f, +0.99729046f, +0.99631261f,
    +0.99518473f, +0.99390697f, +0.99247953f, +0.99090264f, +0.98917651f, +0.98730142f, +0.98527764f, +0.98310549f,
    +0.98078528f, +0.97831737f, +0.97570213f, +0.97293995f, +0.97003125f, +0.96697647f, +0.96377607f, +0.96043052f,
    +0.95694034f, +0.95330604f, +0.94952818f, +0.94560733f, +0.94154407f, +0.93733901f, +0.93299280f, +0.92850608f,
    +0.92387953f, +0.91911385f, +0.91420976f, +0.90916798f, +0.90398929f, +0.89867447f, +0.89322430f, +0.88763962f,
    +0.88192126f, +0.87607009f, +0.87008699f, +0.86397286f, +0.85772861f, +0.85135519f, +0.84485357f, +0.83822471f,
    +0.83146961f, +0.82458930f, +0.81758481f, +0.81045720f, +0.80320753f, +0.79583690f, +0.78834643f, +0.78073723f,
    +0.77301045f, +0.76516727f, +0.75720885f, +0.74913639f, +0.74095113f, +0.73265427f, +0.72424708f, +0.71573083f,
    +0.70710678f, +0.69837625f, +0.68954054f, +0.68060100f, +0.67155895f, +0.66241578f, +0.65317284f, +0.64383154f,
    +0.63439328f, +0.62485949f, +0.61523159f, +0.60551104f, +0.59569930f, +0.58579786f, +0.57580819f, +0.56573181f,
    +0.55557023f, +0.54532499f, +0.53499762f, +0.52458968f, +0.51410274f, +0.50353838f, +0.49289819f, +0.48218377f,
    +0.47139674f, +0.46053871f, +0.44961133f, +0.43861624f, +0.42755509f, +0.41642956f, +0.40524131f, +0.39399204f,
    +0.38268343f, +0.37131719f, +0.35989504f, +0.34841868f, +0.33688985f, +0.32531029f, +0.31368174f, +0.30200595f,
    +0.29028468f, +0.27851969f, +0.26671276f, +0.25486566f, +0.24298018f, +0.23105811f, +0.21910124f, +0.20711138f,
    +0.19509032f, +0.18303989f, +0.17096189f, +0.15885814f, +0.14673047f, +0.13458071f, +0.12241068f, +0.11022221f,
    +0.09801714f, +0.08579731f, +0.07356456f, +0.06132074f, +0.04906767f, +0.03680722f, +0.02454123f, +0.01227154f,
    +0.00000000f, -0.01227154f, -0.02454123f, -0.03680722f, -0.04906767f, -0.06132074f, -0.07356456f, -0.08579731f,
    -0.09801714f, -0.11022221f, -0.12241068f, -0.13458071f, -0.14673047f, -0.15885814f, -0.17096189f, -0.18303989f,
    -0.19509032f, -0.20711138f, -0.21910124f, -0.23105811f, -0.24298018f, -0.25486566f, -0.26671276f, -0.27851969f,
    -0.29028468f, -0.30200595f, -0.31368174f, -0.32531029f, -0.33688985f, -0.34841868f, -0.35989504f, -0.37131719f,
    -0.38268343f, -0.39399204f, -0.40524131f, -0.41642956f, -0.42755509f, -0.43861624f, -0.44961133f, -0.46053871f,
    -0.47139674f, -0.48218377f, -0.49289819f, -0.50353838f, -0.51410274f, -0.52458968f, -0.53499762f, -0.54532499f,
    -0.55557023f, -0.56573181f, -0.57580819f, -0.58579786f, -0.59569930f, -0.60551104f, -0.61523159f, -0.62485949f,
    -0.63439328f, -0.64383154f, -0.65317284f, -0.66241578f, -0.67155895f, -0.68060100f, -0.68954054f, -0.69837625f,
    -0.70710678f, -0.71573083f, -0.72424708f, -0.73265427f, -0.74095113f, -0.74913639f, -0.75720885f, -0.76516727f,
    -0.77301045f, -0.78073723f, -0.78834643f, -0.79583690f, -0.80320753f, -0.81045720f, -0.81758481f, -0.82458930f,
    -0.83146961f, -0.83822471f, -0.84485357f, -0.85135519f, -0.85772861f, -0.86397286f, -0.87008699f, -0.87607009f,
    -0.88192126f, -0.88763962f, -0.89322430f, -0.89867447f, -0.90398929f, -0.90916798f, -0.91420976f, -0.91911385f,
    -0.92387953f, -0.92850608f, -0.93299280f, -0.93733901f, -0.94154407f, -0.94560733f, -0.94952818f, -0.95330604f,
    -0.95694034f, -0.96043052f, -0.96377607f, -0.96697647f, -0.97003125f, -0.97293995f, -0.97570213f, -0.97831737f,
    -0.98078528f, -0.98310549f, -0.98527764f, -0.98730142f, -0.98917651f, -0.99090264f, -0.99247953f, -0.99390697f,
    -0.99518473f, -0.99631261f, -0.99729046f, -0.99811811f, -0.99879546f, -0.99932238f, -0.99969882f, -0.99992470f,
    -1.00000000f, -0.99992470f, -0.99969882f, -0.99932238f, -0.99879546f, -0.99811811f, -0.99729046f, -0.99631261f,
    -0.99518473f, -0.99390697f, -0.99247953f, -0.99090264f, -0.98917651f, -0.98730142f, -0.98527764f, -0.98310549f,
    -0.98078528f, -0.97831737f, -0.97570213f, -0.97293995f, -0.97003125f, -0.96697647f, -0.96377607f, -0.96043052f,
    -0.95694034f, -0.95330604f, -0.94952818f, -0.94560733f, -0.94154407f, -0.93733901f, -0.93299280f, -0.92850608f,
    -0.92387953f, -0.91911385f, -0.91420976f, -0.90916798f, -0.90398929f, -0.89867447f, -0.89322430f, -0.88763962f,
    -0.88192126f, -0.87607009f, -0.87008699f, -0.86397286f, -0.85772861f, -0.85135519f, -0.84485357f, -0.83822471f,
    -0.83146961f, -0.82458930f, -0.81758481f, -0.81045720f, -0.80320753f, -0.79583690f, -0.78834643f, -0.78073723f,
    -0.77301045f, -0.76516727f, -0.75720885f, -0.74913639f, -0.74095113f, -0.73265427f, -0.72424708f, -0.71573083f,
    -0.70710678f, -0.69837625f, -0.68954054f, -0.68060100f, -0.67155895f, -0.66241578f, -0.65317284f, -0.64383154f,
    -0.63439328f, -0.62485949f, -0.61523159f, -0.60551104f, -0.59569930f, -0.58579786f, -0.57580819f, -0.56573181f,
    -0.55557023f, -0.54532499f, -0.53499762f, -0.52458968f, -0.51410274f, -0.50353838f, -0.49289819f, -0.48218377f,
    -0.47139674f, -0.46053871f, -0.44961133f, -0.43861624f, -0.42755509f, -0.41642956f, -0.40524131f, -0.39399204f,
    -0.38268343f, -0.37131719f, -0.35989504f, -0.34841868f, -0.33688985f, -0.32531029f, -0.31368174f, -0.30200595f,
    -0.29028468f, -0.27851969f, -0.26671276f, -0.25486566f, -0.24298018f, -0.23105811f, -0.21910124f, -0.20711138f,
    -0.19509032f, -0.18303989f, -0.17096189f, -0.15885814f, -0.14673047f, -0.13458071f, -0.12241068f, -0.11022221f,
    -0.09801714f, -0.08579731f, -0.07356456f, -0.06132074f, -0.04906767f, -0.03680722f, -0.02454123f, -0.01227154f,
};


static void agc(fixp_t& i, fixp_t& q, bool rst)
{
#pragma HLS INLINE
    static acc_t gain = acc_t(1.0f);
    static acc_t env  = acc_t(0.5f);
    const  acc_t bw     = acc_t(1.0f / 1024.0f);
    const  acc_t target = acc_t(0.5f);

    /* Reset values are the static initialisers above, not zero: a zero gain
     * would mute the core and a zero envelope would divide out to the clamp on
     * the first sample. Reset means power-on state, which is what makes
     * "reset then run" and "fresh core" the same thing -- the property the
     * testbench now checks. */
    if (rst) { gain = acc_t(1.0f); env = acc_t(0.5f); i = 0; q = 0; return; }

    /* Measure the INPUT envelope, then set the gain from it.
     *
     * This was `gain = gain * (0.707 / env)`, applied every sample. That is a
     * repeated multiply, not a loop step: whenever env < 0.707 the gain grows
     * without bound. The scaled sample then left fixp_t's [-1,1) range and
     * WRAPPED, which inverts the sign -- and a QPSK decision is nothing but a
     * pair of sign tests, so every wrapped sample became a wrong symbol.
     * Measured: 253 of 256 bytes correct with the AGC bypassed, 2 with it in
     * circuit.
     *
     * Setting the gain directly from a smoothed envelope is both correct and
     * self-limiting, and the clamps make overflow structurally impossible
     * rather than merely unlikely. */
    acc_t mag = hls::sqrt((acc_t)((acc_t)i * (acc_t)i + (acc_t)q * (acc_t)q));
    env = env + (mag - env) * bw;

    if (env > acc_t(1e-3f)) {
        acc_t g = target / env;
        /* Clamp 4. Do NOT raise this above 8: acc_t is ap_fixed<32,4>, whose
         * range is [-8, 8), so acc_t(8.0f) is not representable. The comparison
         * below then misfires, the gain is forced to the 0.05 lower clamp, and
         * the demodulator collapses -- measured mean_gain 0.05 with stress
         * falling from best_run 253 to 18. That looked like "high gain breaks
         * the loops" and is nothing of the kind.
         *
         * The clamp is not what limits acquisition at hardware signal levels.
         * Measured mean gain is only ~2 even on a vector scaled to hardware
         * amplitude, so it never reaches 4, and every representable clamp from
         * 4.0 to 7.9 gives byte-identical results on all six vectors.
         *
         * What does limit it is CONVERGENCE TIME. env starts at 0.5 and adapts
         * at bw = 1/1024 per sample, so on a low-amplitude input it is still
         * travelling for most of a 4096-sample vector -- and acquisition
         * happens during that transient. best_run drops from 253 to 169 on
         * clean.iq scaled down 8x (vector lowamp8), with the errors at the
         * START rather than spread through the run.
         *
         * Widening bw is not the fix either: 1/64 helps low amplitude (169 ->
         * 242) but pushes impaired below its gate, and 1/256 is worse than both
         * its neighbours, so the response is not even monotonic. The fix
         * belongs upstream -- scale the 12-bit converter data so the AGC starts
         * near the right gain instead of hunting for it. See RX_SHIFT in
         * fpga/rtl/adi_iq_to_axis.v. */
        if (g > acc_t(4.0f))  g = acc_t(4.0f);
        if (g < acc_t(0.05f)) g = acc_t(0.05f);
        gain = g;
    }

    /* Saturate explicitly into fixp_t. An implicit cast wraps. */
    acc_t oi = (acc_t)i * gain;
    acc_t oq = (acc_t)q * gain;
    const acc_t LIM = acc_t(0.999f);
    if (oi >  LIM) oi =  LIM;
    if (oi < -LIM) oi = -LIM;
    if (oq >  LIM) oq =  LIM;
    if (oq < -LIM) oq = -LIM;
    i = (fixp_t)oi;
    q = (fixp_t)oq;
}

/* ── QPSK symbol decision → 2 bits ─────────────────────────────────
 * Standard QPSK Gray-coded constellation:
 *   I>0,Q>0 → 00    I<0,Q>0 → 01
 *   I>0,Q<0 → 10    I<0,Q<0 → 11
 */
static ap_uint<2> qpsk_decision(fixp_t i, fixp_t q)
{
#pragma HLS INLINE
    ap_uint<1> bi = (i >= fixp_t(0)) ? ap_uint<1>(0) : ap_uint<1>(1);
    ap_uint<1> bq = (q >= fixp_t(0)) ? ap_uint<1>(0) : ap_uint<1>(1);
    /* Q is the HIGH bit, I the low one.
     *
     * This returned (bi, bq) -- I high, Q low -- which is not the mapping the
     * host modulator uses. liquid's LIQUID_MODEM_QPSK puts bit0 on the I sign
     * and bit1 on the Q sign, so the two disagreed on every symbol whose I and
     * Q bits differ: half of them, giving a byte match indistinguishable from
     * chance. An exhaustive search over the constellation symmetry group
     * (rotation x conjugation x bit order x inversion) picked this one out at
     * 253 of 256 bytes exact, the other three being matched-filter fill. */
    return (ap_uint<2>)((bq, bi));
}

/* ── Costas loop for QPSK carrier recovery ──────────────────────────
 * Phase error detector, decision-directed: e = sgn(I)*Q - sgn(Q)*I
 * Loop filter: 2nd order PLL
 *
 * TIMING. This used hls::cos/hls::sin on the phase accumulator, which put a
 * ~24-stage CORDIC expansion inside a loop-carried dependency. At II=1 a
 * recurrence has to settle within a single cycle, so the whole chain
 *
 *     phase -> CORDIC -> rotate -> error -> phase
 *
 * was one combinational path. Synthesis measured it end to end at 123.808 ns
 * and reported Fmax 8.08 MHz -- against an 8 MS/s sample rate, no margin at
 * all. HLS named it exactly: 'load' on static 'phase' through
 * hls_cordic_apfixed.h to the 58-bit rotate multiply.
 *
 * Three changes, and the first two only help together:
 *
 *   The CORDIC is gone, replaced by the 512-entry sin/cos ROM above.
 *
 *   The loop filter runs on the PREVIOUS symbol's error, and the ROM is read
 *   for the next symbol at the end of the call. That splits the one long
 *   recurrence into three short register-to-register hops -- trig -> error,
 *   error -> phase, phase -> trig -- each of which HLS can pipeline
 *   independently. The cost is one extra symbol of loop delay; at Kp=0.04 that
 *   is far inside the loop's settling time, and the impaired vector (200 Hz
 *   CFO) still decodes exactly.
 *
 *   The +-1 factors in the error detector are written as negations rather than
 *   multiplies by acc_t(1.0f). They were never arithmetic, only sign changes,
 *   and as multiplies they put two 32x32 products on the error hop.
 *
 * Rotating with a trig pair registered one call earlier is also what a real
 * NCO does: the phase accumulator and the waveform lookup are separate
 * pipeline stages, not a single combinational function.
 */
static void costas_loop(fixp_t& i, fixp_t& q, bool rst)
{
#pragma HLS INLINE
    static phase_t phase = 0;
    static phase_t freq  = 0;
    static acc_t   err_z = 0;                 /* previous symbol's phase error */
    static trig_t  cos_p = trig_t(1.0f);      /* cos/sin of `phase`, registered */
    static trig_t  sin_p = trig_t(0.0f);

    /* Every field, including the registered trig pair -- (1,0) is cos/sin of
     * phase 0, so the pair has to be reset consistently with the accumulator
     * or the first rotation after reset uses a stale angle. */
    if (rst) {
        phase = 0; freq = 0; err_z = 0;
        cos_p = trig_t(1.0f); sin_p = trig_t(0.0f);
        i = 0; q = 0;
        return;
    }
    /* Loop gains, both exact powers of two -- deliberately, and it is a timing
     * fix as much as a control one. phase_t has 28 fractional bits, and neither
     * 0.04 nor 0.001 is representable in binary, so HLS realised each constant
     * multiply as a six-term shift-add chain. Those two chains were 15.7 ns of
     * a 36.7 ns recurrence, the single largest item on it. A power of two is a
     * wire.
     *
     * The dynamics are equal or better, not merely acceptable. For this filter
     * (phase += Kp*e + freq; freq += Ki*e) the natural frequency is sqrt(Ki)
     * and the damping is Kp/(2*sqrt(Ki)):
     *
     *            Ki          wn           Kp        zeta
     *   was   0.001      0.031623      0.04       0.632
     *   now   2^-10      0.031250      2^-4       1.000
     *
     * so the loop bandwidth moves by 1.2% and the loop goes from slightly
     * underdamped to critically damped -- no overshoot on acquisition.
     *
     * Both csim vectors pass either way; at 200 Hz CFO and 25 dB SNR they do
     * not discriminate between these gain sets, so this is a design choice
     * resting on the analysis above rather than on a measurement. */
    const phase_t Kp = phase_t(0.0625f);        /* 2^-4  */
    const phase_t Ki = phase_t(0.0009765625f);  /* 2^-10 */

    /* Rotate input by -phase, using the pair looked up at the end of the
     * previous call. cos_p/sin_p start at (1,0), which is phase = 0. */
    acc_t ir =  (acc_t)i * (acc_t)cos_p + (acc_t)q * (acc_t)sin_p;
    acc_t qr = -(acc_t)i * (acc_t)sin_p + (acc_t)q * (acc_t)cos_p;

    /* 2nd order loop filter, driven one symbol behind. */
    freq  = freq  + (phase_t)(Ki * err_z);
    phase = phase + (phase_t)(Kp * err_z) + freq;

    /* Wrap to [-pi, pi]. Without this the accumulator walks off regardless of
     * how many integer bits it has, and the ROM index loses meaning. */
    const phase_t PI  = phase_t(3.14159265f);
    const phase_t TAU = phase_t(6.28318531f);
    if (phase >  PI) phase -= TAU;
    if (phase < -PI) phase += TAU;

    /* Fetch the trig pair for the next symbol.
     *
     * phase is in [-pi, pi]. Adding PI first keeps the value non-negative, so
     * the truncation below is a floor rather than a toward-zero round that
     * would bias negative phases. That shift then has to be taken back out,
     * which is the + NCO_SIZE/2 on the index: half a table is exactly pi, and
     * ap_uint<NCO_BITS> wraps modulo NCO_SIZE because the table is one full
     * turn. Without it phase = 0 indexes cos(pi) = -1 and every rotation comes
     * out inverted -- which is what it did, for 25.4% of bytes. */
    const nco_t scaled = (nco_t)(phase + PI) * nco_t(NCO_SIZE / 6.28318530718);
    const ap_uint<NCO_BITS> idx =
        (ap_uint<NCO_BITS>)((int)scaled + (NCO_SIZE / 2));
    cos_p = NCO_COS[idx];
    sin_p = NCO_SIN[idx];

    /* Error for the NEXT symbol: e = sgn(I)*Q - sgn(Q)*I, as selects. */
    err_z = ((ir >= acc_t(0)) ? qr : (acc_t)(-qr))
          - ((qr >= acc_t(0)) ? ir : (acc_t)(-ir));

    /* Saturate into fixp_t. A rotation can raise the magnitude of either
     * component to sqrt(2) times the input, which leaves [-1,1) and wraps --
     * the same sign-flipping failure the AGC had. */
    const acc_t LIM = acc_t(0.999f);
    if (ir >  LIM) ir =  LIM;
    if (ir < -LIM) ir = -LIM;
    if (qr >  LIM) qr =  LIM;
    if (qr < -LIM) qr = -LIM;
    i = (fixp_t)ir;
    q = (fixp_t)qr;
}

/* ── Symbol timing recovery (early-late gate, simplified) ───────────
 * Returns true at the correct symbol sampling instant.
 */
static bool timing_recovery(fixp_t i, fixp_t q, fixp_t& i_out, fixp_t& q_out,
                            bool rst)
{
#pragma HLS INLINE
    /* Gardner timing recovery, RATE-controlled modulo-1 NCO.
     *
     * The previous version kept `mu` as a [0,1) fractional index and slipped a
     * whole sample whenever it left that interval:
     *
     *     if (mu >= 1) { mu -= 1; cnt = 1; }   // skip a sample
     *     else if (mu < 0) { mu += 1; cnt = 7; }  // repeat one
     *
     * mu starts at 0, which is ON that boundary, so every noise excursion fired
     * a slip. Measured on a real hardware capture: 553 skips and 554 repeats in
     * 4096 symbols -- they cancel, so the sampling phase never moved, and each
     * one teleported the loop onto a different branch of the S-curve before it
     * could build any drift. mu was never once in [0.05,0.95].
     *
     * The loop was therefore not a loop at all: the core behaved as a
     * fixed-phase decimator whose phase was whatever reset left behind. Proof,
     * by changing only mu's reset value:
     *
     *   mu reset   clean  impaired  stress  |  hw@0.00  hw@0.25  hw@0.50  hw@0.75
     *   0.0          253       253     253  |       13       51      182      182
     *   0.5           15        15       7  |      182      182      182      182
     *
     * Mirror images. Neither value is right; the loop simply never adapts, and
     * the csim vectors pass only because they are generated aligned to the
     * phase mu=0 happens to select.
     *
     * Raising the gain cannot fix this. The Gardner error on hardware has
     * mean/rms = 0.245 -- noise-dominated -- and Kp scales drift and noise
     * identically. Sweeping Kp over 64x and Ki over 64x moved hw@0.00 not at
     * all (11..21 bytes across 18 combinations). Pre-filtering the
     * discriminator reached 30 and cost the clean vector. Both were tried and
     * rejected before this rewrite.
     *
     * VALIDATED ON HARDWARE 2026-09-03, which is the evidence that counts.
     * UNIT-A transmits mod_ref.iq through its stock-PL DMA path, UNIT-B runs
     * this core, 434 MHz, RSSI 65.25 dB constant with the AGC regulating, 30
     * independent acquisitions per arm. The soft reset only takes if the output
     * is being drained -- a stalled core stops re-reading its AXI-Lite
     * registers -- so every trial verifies it; see fpga/scripts/rx_acq_test.sh.
     *
     *                       lock rate   min   p25   median   mean   full payload
     *   old slip-based loop    70.0%     19    97      998   646.3     15/30
     *   this NCO loop         100.0%    834  1019     1019  1012.8     29/30
     *
     * The old loop's distribution is starkly BIMODAL -- 19, 36, 68, 75, 77, 86,
     * 93, 93, 110, 236 ... then fifteen trials at 1019 -- with NINE outright
     * failures below 200 symbols. That bimodality is precisely the intermittent
     * lock this work set out to fix. This loop has none: zero failures, and the
     * worst case improves 44x. 1019 of 1024 symbols is the whole payload less
     * matched-filter fill. The csim prediction (mean 100.4 -> 159.8, min
     * 10 -> 127, bimodality gone) holds on real hardware.
     *
     * C/RTL co-simulation also PASSES at best_run 251, identical to csim, so
     * the synthesised core matches the C model -- there is no static-state
     * divergence of the kind qpsk_mod.cpp documents.
     *
     * THE FIX is to control the symbol RATE rather than clamp a phase. A
     * modulo-1 accumulator counts down by w each sample; a symbol strobes when
     * it underflows, and the fractional part at underflow IS the interpolation
     * index. There is no special skip/repeat path, so there is no
     * discontinuity for noise to trip, and the phase can slew without limit --
     * the accumulator wraps as a matter of course. This is the standard
     * interpolator-control structure (Gardner TED + NCO), and it is CHEAPER
     * than what it replaces: one add and one compare per sample against the
     * old branchy wrap plus its 3-bit counter games.
     */
    static acc_t  nco   = 0;          /* modulo-1 timing accumulator        */
    static acc_t  mu    = 0;          /* fractional index, set at each strobe */
    static acc_t  wctl  = 0;          /* loop correction to the nominal rate */
    static acc_t  integ = 0;          /* PI integrator (rate error)         */
    static fixp_t hi[5], hq[5];
    static fixp_t im_i = 0, im_q = 0; /* interpolated mid-symbol sample     */
    static fixp_t ip_i = 0, ip_q = 0; /* previous interpolated symbol       */
    static ap_uint<3> since = 0;      /* samples since the last strobe      */

    /* Nominal rate: one strobe every RRC_SPS_HW samples. */
    const acc_t W0 = acc_t(1.0f / (float)RRC_SPS_HW);
    /* Loop gains, both powers of two -- the same timing-closure argument the
     * Costas loop documents. Chosen by sweeping both against every vector AND
     * against 24 randomised hardware initial conditions at once; the pair below
     * was the best worst-case, and the response is flat around it (Kp 2^-3 and
     * 2^-2 differ by 4 bytes of mean). Kp 2^-1 destabilises: clean falls to
     * 136.
     *
     * These are NOT the old gains rescaled. The old loop steered a POSITION
     * (mu) and these steer a RATE (w), so the units differ; 2^-9 here would
     * leave the loop unable to correct shift50 (measured 11 bytes). */
    const acc_t Kp = acc_t(0.25f);          /* 2^-2 */
    const acc_t Ki = acc_t(0.001953125f);   /* 2^-9 */

    if (rst) {
        for (int k = 0; k < 5; k++) { hi[k] = 0; hq[k] = 0; }
        nco = 0; mu = 0; wctl = 0; integ = 0;
        im_i = 0; im_q = 0; ip_i = 0; ip_q = 0;
        since = 0;
        i_out = 0; q_out = 0;
        return false;
    }

    /* newest sample enters at index 0 */
    for (int k = 4; k > 0; k--) {
#pragma HLS UNROLL
        hi[k] = hi[k-1];
        hq[k] = hq[k-1];
    }
    hi[0] = i;
    hq[0] = q;

    since++;

    /* Advance the timing NCO. Underflow marks the symbol instant, and the
     * residue scaled by SPS is where inside the sample it fell. */
    const acc_t w = W0 + wctl;
    nco = nco - w;
    bool strobe = false;
    if (nco < acc_t(0.0f)) {
        nco = nco + acc_t(1.0f);
        /* nco/w, approximated as nco*SPS: w departs from 1/SPS only by the
         * loop correction, which is small, and a divide would infer a divider
         * on the critical path for no accuracy that matters here. */
        mu = nco * acc_t((float)RRC_SPS_HW);
        if (mu > acc_t(0.999f)) mu = acc_t(0.999f);
        if (mu < acc_t(0.0f))   mu = acc_t(0.0f);
        strobe = true;
    }

    /* LINEAR interpolation, as before: the cubic form put a
     * mu -> mu^2 -> mu^3 chain on the critical path and dropped estimated Fmax
     * below the 30 MHz modem clock. */
    const acc_t x1 = (acc_t)hi[2], x2 = (acc_t)hi[1];
    const acc_t y1 = (acc_t)hq[2], y2 = (acc_t)hq[1];
    const acc_t ii = x1 + (x2 - x1) * mu;
    const acc_t qq = y1 + (y2 - y1) * mu;

    /* Mid-symbol sample, half a symbol before the strobe. */
    if (since == 2) { im_i = (fixp_t)ii; im_q = (fixp_t)qq; }

    if (!strobe) return false;
    since = 0;

    const fixp_t si = (fixp_t)ii;
    const fixp_t sq = (fixp_t)qq;

    /* Gardner: the error vanishes when the mid sample sits on the zero
     * crossing, i.e. when the symbol sample sits on the eye centre. */
    acc_t err = (acc_t)(si - ip_i) * (acc_t)im_i
              + (acc_t)(sq - ip_q) * (acc_t)im_q;

    /* PI on the RATE. integ carries any standing rate error; Kp supplies the
     * phase correction. Both stay powers of two -- see costas_loop for why
     * that matters to timing closure. */
    integ = integ + Ki * err;
    wctl  = Kp * err + integ;

    /* Bound the correction so a transient cannot invert or stall the strobe:
     * w must stay positive and well inside one sample per strobe. */
    const acc_t WLIM = acc_t(0.15f);
    if (wctl >  WLIM) wctl =  WLIM;
    if (wctl < -WLIM) wctl = -WLIM;

    ip_i = si; ip_q = sq;
    i_out = si;
    q_out = sq;
    return true;
}

/* ═══════════════════════════════════════════════════════════════════
 * Top-level HLS function
 * Synthesized to an AXI4-Stream IP core.
 *
 * Interface:
 *   s_axis_iq   → AXI4-Stream input  (AD9363 IQ samples, up to 61.44 MSPS (HamGeek Pluto+))
 *   m_axis_bits → AXI4-Stream output (decoded byte stream)
 *
 * Throughput: 1 IQ sample per clock cycle @ up to 200 MHz
 *             → 200 MSPS > 61.44 MSPS requirement (10× margin)
 * Latency:    ~36 clock cycles pipeline depth (RRC filter + sync)
 * Resources:  ~800 LUTs, ~400 FFs, 2 DSP48 slices (Zynq-7010 estimate)
 * ════════════════════════════════════════════════════════════════════ */
/* ── AXI4-Lite demod control ─────────────────────────────────────────
 *   0x10  demod_enabled  RW  1 = demodulate, 0 = suppress output
 *   0x18  lock_count     RO  symbol lock events since reset
 */
void qpsk_demod_top(hls::stream<IQSample>& s_axis_iq,
                    hls::stream<BitByte>&  m_axis_bits,
                    volatile ap_uint<1>&   demod_enabled,
                    volatile ap_uint<32>&  lock_count,
                    volatile ap_uint<1>&   soft_reset,
                    volatile ap_uint<1>&   diff_mode)
{
#pragma HLS INTERFACE axis       port=s_axis_iq
#pragma HLS INTERFACE axis       port=m_axis_bits
#pragma HLS INTERFACE s_axilite  port=demod_enabled  offset=0x10 bundle=ctrl
#pragma HLS INTERFACE s_axilite  port=lock_count     offset=0x18 bundle=ctrl
#pragma HLS INTERFACE s_axilite  port=soft_reset     offset=0x20 bundle=ctrl
#pragma HLS INTERFACE s_axilite  port=diff_mode      offset=0x28 bundle=ctrl
#pragma HLS INTERFACE s_axilite  port=return         bundle=ctrl
/* II=2, not 1.
 *
 * The cubic interpolator added for timing recovery put its multiply chain on
 * the critical path and dropped the estimated Fmax from 44.41 MHz to 28.41 MHz
 * -- below the 30 MHz modem clock, so the core would not have closed timing.
 * The throughput was never needed at II=1: the RX sample rate is 7.68 MS/s and
 * II=2 still gives 15 MS/s of capacity at 30 MHz, about 2x margin. Relaxing II
 * lets the scheduler spread the interpolator over two cycles instead of
 * demanding it in one. */
/* II=2, and it must stay 2.
 *
 * II=1 schedules and meets timing (Fmax 54.49 MHz, WNS +0.552 in the
 * integrated design) but MEASURES WORSE ON HARDWARE: BER 8.8e-3 against
 * 1.9e-6 at II=2, reproduced with two different test scripts at the same
 * 7.68 MS/s. csim cannot see the difference because II is a scheduling
 * directive, so the C model is identical either way. The cause is not
 * understood; the measurement is repeatable, so the throughput is bought by
 * raising the modem clock instead, where the effect is understood.
 *
 * Capacity at II=2 is modem_clk/2 samples per second: 15 MS/s at 30 MHz,
 * 20 MS/s at 40 MHz. The 8 Mbit/s target needs 16 MS/s. */
#pragma HLS PIPELINE II=2

    static ap_uint<32> locks = 0;
    /* Declared here, not at the packing code below, so the reset path can
     * reach them. The width rationale for both still lives with the packing. */
    static ap_uint<8>  bit_acc = 0;
    static ap_uint<3>  sym_cnt = 0;
#if SDR_BYPASS_TIMING
    static ap_uint<3>  fixed_phase = 0;
#endif

    // AXI-Lite control registers are `volatile ap_uint<N>&`, and clang-16
    // (Vitis HLS 2026.1) no longer converts those implicitly to bool:
    //   ERROR: [HLS 207-4589] no viable conversion from 'volatile ap_uint<1>' to 'bool'
    // Sampling each register once into a local is both the fix and the
    // better hardware: a volatile reference re-reads the register on every
    // access, so a control value could otherwise change midway through the
    // computation it is steering.
    const ap_uint<1> demod_raw = demod_enabled;   // copy out of volatile first
    const ap_uint<1> rst_raw   = soft_reset;
    const ap_uint<1> diff_raw  = diff_mode;
    const bool       demod_en  = (demod_raw != 0);
    const bool       rst_en    = (rst_raw   != 0);
    const bool       diff_en   = (diff_raw  != 0);

    /* Differential decoding, the receive half of qpsk_mod's diff_mode. Must
     * match the transmitter: both ends absolute, or both differential.
     *
     * data[n] = sym[n] - sym[n-1] (mod 4). A constant rotation r adds r to
     * every received symbol and cancels in the difference, which is what makes
     * the four-fold QPSK phase ambiguity disappear without a preamble
     * correlator. See qpsk_mod.cpp for the full rationale and the cost.
     *
     * This sits on the SYMBOL path -- once every four samples -- not on the
     * per-sample critical path, so it costs nothing that matters. */
    static ap_uint<2> prev_phase = 0;   // differential phase reference

    /* ── Soft reset (0x20) ───────────────────────────────────────────
     * Level sensitive: while it is high the core holds every stage at its
     * power-on state and consumes nothing but stale input.
     *
     * This exists because the core is a chain of static accumulators -- carrier
     * phase, loop integrator, symbol-phase counter, matched-filter history, AGC
     * gain and envelope -- and none of it had a way back to a known state. Two
     * consequences, one measured and one waiting to happen:
     *
     *   In C simulation the vectors were not independent. Running clean, then
     *   impaired, then stress left the third starting from the second's
     *   converged loop state: stress scored 248 in sequence against 253 on its
     *   own. Test order was load-bearing, which means a green run proved less
     *   than it appeared to.
     *
     *   On hardware the same statics survive a receiver restart, a channel or
     *   modulation change, and the gap between bursts. A carrier integrator
     *   still wound up for the previous channel's offset is a real acquisition
     *   failure, not a testbench artefact.
     *
     * The clear is dispatched into each stage rather than done here, so the
     * code that clears a field sits beside the code that declares it.
     *
     * Input is drained rather than left alone: holding reset while a producer
     * is still streaming would otherwise back-pressure it into a stall, and on
     * a channel change the samples in flight are exactly the ones to discard.
     */
    if (rst_en) {
        fixp_t r0 = 0, r1 = 0, r2 = 0, r3 = 0;
        agc(r0, r1, true);
        (void)rrc_filter_decim(r0, r1, r2, r3, true);
        (void)timing_recovery(r0, r1, r2, r3, true);
        costas_loop(r2, r3, true);
        locks    = 0;
        bit_acc  = 0;
        sym_cnt  = 0;
        prev_phase = 0;
#if SDR_BYPASS_TIMING
        fixed_phase = 0;
#endif
        lock_count = 0;
        if (!s_axis_iq.empty()) s_axis_iq.read();
        return;
    }

    if (s_axis_iq.empty()) return;
    if (!demod_en)        { s_axis_iq.read(); return; }  // drain + suppress

    IQSample in = s_axis_iq.read();

    /* Reinterpret int16 as Q1.15.
     *
     * This was `fixp_t(in.i) / fixp_t(32768)`, which divides by zero on every
     * single sample: fixp_t is ap_fixed<16,1> with range [-1,1), so the
     * literal 32768 is not representable and wraps to exactly 0. C simulation
     * dies with SIGFPE on the first sample -- the core had never been run.
     *
     * An int16 and a Q1.15 fixed-point value have identical bit patterns, so
     * the conversion is a reinterpretation, not an arithmetic operation. This
     * is both correct and free in hardware, where the divide would otherwise
     * have inferred a divider. */
    fixp_t i, q;
    i.range(15, 0) = in.data.range(15,  0);
    q.range(15, 0) = in.data.range(31, 16);

    /* AGC */
#if !SDR_BYPASS_AGC
    agc(i, q, false);
#endif

    /* RRC matched filter + decimation (SPS=4) */
    fixp_t di, dq;
    if (!rrc_filter_decim(i, q, di, dq, false)) return;

    /* Timing recovery */
    fixp_t ti, tq;
#if SDR_BYPASS_TIMING
    /* Fixed-phase decimation: take one sample in SPS with no loop. Matches
     * what the float reference decoder does, so a mismatch here isolates to
     * the timing loop rather than to the filter or the mapping. */
    fixed_phase++;
    if (fixed_phase < 4) return;
    fixed_phase = 0;
    ti = di; tq = dq;
#else
    if (!timing_recovery(di, dq, ti, tq, false)) return;
#endif

    /* Carrier recovery */
#if !SDR_BYPASS_COSTAS
    costas_loop(ti, tq, false);
#endif

    /* QPSK symbol decision */
    ap_uint<2> sym = qpsk_decision(ti, tq);
    if (diff_en) {
        /* Gray -> binary, then difference in PHASE. For two bits the Gray
         * decode is the same operation as the encode, g ^ (g>>1). Differencing
         * the raw symbol INDEX does not work: the index walks the circle as
         * 0, 1, 3, 2, so a rotation is not a constant offset in it. */
        const ap_uint<2> ph = (ap_uint<2>)(sym ^ (sym >> 1));
        sym = (ap_uint<2>)(ph - prev_phase);
        prev_phase = ph;
    }

    /* Pack four 2-bit symbols into one output byte.
     *
     * Both widths here were wrong, and together they silently removed the
     * entire output path:
     *
     *   sym_cnt was ap_uint<2>, which counts 0,1,2,3,0,... and can never
     *   equal 4. The comparison below was therefore always false, the write
     *   to m_axis_bits was unreachable, and HLS eliminated it -- leaving
     *   'Port m_axis_bits_TDATA has no fanin or fanout' and a top function
     *   reported as having no outputs. The core synthesised and exported IP
     *   that could never emit a bit.
     *
     *   bit_acc was ap_uint<4>, which holds two symbols, not the four the
     *   comment describes. Even had the counter worked, half of every byte
     *   would have been shifted out and lost.
     *
     * sym_cnt needs three bits to hold the value 4 at all.
     */
    bit_acc = (bit_acc << 2) | sym;
    sym_cnt++;

    if (sym_cnt == 4) {
        sym_cnt = 0;
        locks++;
        lock_count = locks;
        BitByte out;
        out.data = (byte_t)bit_acc;
        /* No `valid` field any more: TVALID is the stream handshake, and a
         * payload bit that duplicated it was only ever going to disagree with
         * it. TLAST is forwarded from the input beat that completed the byte. */
        axis_mark(out, in.last != 0);
        m_axis_bits.write(out);
    }
}

