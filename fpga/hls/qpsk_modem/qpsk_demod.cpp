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
    /* Gardner timing recovery with a fractional interpolator.
     *
     * The previous version computed a Gardner error, integrated it into `tau`,
     * and never read `tau` -- an open loop. The symbol instant stayed frozen
     * wherever reset left it, so the core could not re-centre on the eye.
     * csim measures the cost by delaying clean.iq a fraction of a sample:
     * aligned 100.0%, quarter-sample 99.6%, half-sample 42.3%. The quarter
     * case matches the 99.4% seen over coax, where 6 symbols per 1024 failed
     * at identical positions in all 255 blocks -- a fixed phase, not noise.
     *
     * Skipping or repeating whole samples was tried first and reverted: it
     * rescued the offset cases but regressed the aligned one (100% -> 83-94%)
     * because a correction quantised to whole samples cannot settle, so the
     * loop hunts. Steering a fractional delay lets it converge and STAY there,
     * which is what keeps the aligned case bit-exact.
     *
     * Cubic (Catmull-Rom) interpolation over a 4-sample window. The history is
     * one sample deeper than the arithmetic needs so that mu can reach both
     * ends of its interval without the window running off the buffer.
     */
    static fixp_t hi[5], hq[5];
    static acc_t  mu    = 0;          /* fractional delay within a sample   */
    static acc_t  integ = 0;          /* PI integrator (frequency estimate) */
    static fixp_t im_i = 0, im_q = 0; /* interpolated mid-symbol sample     */
    static fixp_t ip_i = 0, ip_q = 0; /* previous interpolated symbol       */
    static ap_uint<3> cnt = 0;

    /* Loop gains. Ki is far below Kp so the integrator only trims a slow rate
     * error; damping comes from that separation. Both are powers of two: the
     * Costas loop above documents why that matters for timing closure. */
    /* Chosen by sweeping both gains against every vector at once. The pair
     * matters more than either value: a fast Kp rescues a badly offset signal
     * but hunts on an aligned one (Kp 2^-5 took clean from 100% to 91.7%),
     * while Ki must stay far below it or the integrator winds up and drags the
     * instant off the eye centre. At 2^-9 / 2^-16 the loop settles:
     *
     *   vector    open loop            closed loop
     *   clean     run 253 100.0%       run 253 100.0%   (bit-exact, unchanged)
     *   impaired  run 253 100.0%       run 253 100.0%   (bit-exact, unchanged)
     *   stress    run 253 100.0%       run 253 100.0%
     *   shift25   run 127  99.6%       run 251  99.6%
     *   shift50   run  10  42.3%       run 251  99.6%
     *
     * best_run is the figure that counts: it is the longest unbroken stretch of
     * correct bytes, so it separates "acquires and holds" from "never locks".
     */
    const acc_t Kp = acc_t(0.001953125f);      /* 2^-9  */
    const acc_t Ki = acc_t(0.0000152587890625f); /* 2^-16 */

    if (rst) {
        for (int k = 0; k < 5; k++) { hi[k] = 0; hq[k] = 0; }
        mu = 0; integ = 0;
        im_i = 0; im_q = 0; ip_i = 0; ip_q = 0;
        cnt = 0;
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

    cnt++;

    /* Catmull-Rom at fractional position mu between hi[2] and hi[1]. */
    const acc_t x0 = (acc_t)hi[3], x1 = (acc_t)hi[2];
    const acc_t x2 = (acc_t)hi[1], x3 = (acc_t)hi[0];
    const acc_t y0 = (acc_t)hq[3], y1 = (acc_t)hq[2];
    const acc_t y2 = (acc_t)hq[1], y3 = (acc_t)hq[0];
    const acc_t m2 = mu * mu;
    const acc_t m3 = m2 * mu;
    const acc_t ii = acc_t(0.5f) * ((acc_t(2.0f) * x1) +
                     (-x0 + x2) * mu +
                     (acc_t(2.0f)*x0 - acc_t(5.0f)*x1 + acc_t(4.0f)*x2 - x3) * m2 +
                     (-x0 + acc_t(3.0f)*x1 - acc_t(3.0f)*x2 + x3) * m3);
    const acc_t qq = acc_t(0.5f) * ((acc_t(2.0f) * y1) +
                     (-y0 + y2) * mu +
                     (acc_t(2.0f)*y0 - acc_t(5.0f)*y1 + acc_t(4.0f)*y2 - y3) * m2 +
                     (-y0 + acc_t(3.0f)*y1 - acc_t(3.0f)*y2 + y3) * m3);

    if (cnt == 2) { im_i = (fixp_t)ii; im_q = (fixp_t)qq; }
    if (cnt < 4) return false;
    cnt = 0;

    const fixp_t si = (fixp_t)ii;
    const fixp_t sq = (fixp_t)qq;

    /* Gardner: the error vanishes when the mid sample sits on the zero
     * crossing, i.e. when the symbol sample sits on the eye centre. */
    acc_t err = (acc_t)(si - ip_i) * (acc_t)im_i
              + (acc_t)(sq - ip_q) * (acc_t)im_q;

    integ = integ + Ki * err;
    mu    = mu + Kp * err + integ;

    /* Keep mu inside one sample by moving the interpolation window instead. */
    if (mu >= acc_t(1.0f))      { mu = mu - acc_t(1.0f); cnt = 1; }
    else if (mu < acc_t(0.0f))  { mu = mu + acc_t(1.0f); cnt = 7; }

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
                    volatile ap_uint<1>&   soft_reset)
{
#pragma HLS INTERFACE axis       port=s_axis_iq
#pragma HLS INTERFACE axis       port=m_axis_bits
#pragma HLS INTERFACE s_axilite  port=demod_enabled  offset=0x10 bundle=ctrl
#pragma HLS INTERFACE s_axilite  port=lock_count     offset=0x18 bundle=ctrl
#pragma HLS INTERFACE s_axilite  port=soft_reset     offset=0x20 bundle=ctrl
#pragma HLS INTERFACE s_axilite  port=return         bundle=ctrl
#pragma HLS PIPELINE II=1   /* initiation interval = 1 clock */

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
    const bool       demod_en  = (demod_raw != 0);
    const bool       rst_en    = (rst_raw   != 0);

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
        locks   = 0;
        bit_acc = 0;
        sym_cnt = 0;
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

