/* Zynq-7000 QSPI flasher - auto-start mode, 32KB chunks, OCM bank2 only */
typedef unsigned int  u32;
typedef unsigned char u8;

/* QSPI controller registers */
#define QSPI_BASE     0xE000D000U
#define QSPI_CR       (*(volatile u32*)(QSPI_BASE + 0x00))
#define QSPI_ISR      (*(volatile u32*)(QSPI_BASE + 0x04))
#define QSPI_IDR      (*(volatile u32*)(QSPI_BASE + 0x0C))
#define QSPI_ER       (*(volatile u32*)(QSPI_BASE + 0x14))
#define QSPI_TXD1     (*(volatile u32*)(QSPI_BASE + 0x80))  /* 1-byte TX */
#define QSPI_RXD      (*(volatile u32*)(QSPI_BASE + 0x20))
#define QSPI_LQSPI_CR (*(volatile u32*)(QSPI_BASE + 0xA0))

/* ISR bits */
#define ISR_TXF     (1u<<3)   /* TX FIFO full */
#define ISR_RXNE    (1u<<4)   /* RX FIFO not empty */

/*
 * CR bits:
 *   31: IFM   = 1  (I/O mode, not memory-mapped)
 *   14: SSFORCE = 1  (manual CS control enabled)
 *   10: SSCTRL = 1  (CS deasserted = high when SSFORCE=1)
 *  5:3: BAUD  = 7  (SPI clock = ref_clk / 256, very safe)
 *    0: MSTR  = 1  (master mode)
 *
 * NOTE: bit 15 (MAN_START_EN) is deliberately 0 here.
 * With MAN_START_EN=0, a transfer starts automatically the moment
 * data is written to TXD1 -- no extra MANSTRT trigger needed.
 */
#define CR_IDLE     ((1u<<31)|(1u<<14)|(1u<<10)|(7u<<3)|(1u<<0))
#define CR_CS_ON    ((1u<<31)|(1u<<14)|(0u<<10)|(7u<<3)|(1u<<0))

/* All slots in OCM bank2 (0x20000-0x2FFFF, 64KB) */
#define CHUNK_DATA  0x00021000U   /* 32KB data buffer */
#define STATUS      0x00029000U   /* handshake */
#define DEST_ADDR   0x00029004U   /* QSPI target address */
#define DO_ERASE    0x00029008U   /* 1 = erase 64KB sector first */
#define JEDEC0      0x0002900CU
#define JEDEC1      0x00029010U
#define JEDEC2      0x00029014U

static void dly(void) {
    volatile int i;
    for (i = 0; i < 2000; i++) __asm__("nop");
}

/*
 * SWDT kick register — the Zynq System Watchdog fires every ~250ms at the
 * BootROM's default CCR=0x00003FFC setting (CLKSEL=12, CRV=1023).
 * Writing 0x1999 to SWDT_RESTART (0xF8005008) resets the counter.
 * We kick it at least every 100ms from ARM code to prevent it from firing.
 * dly() ≈ 60µs, so 1666 dly calls ≈ 100ms → kick every 1600 iterations.
 */
#define SWDT_RESTART (*(volatile u32 *)0xF8005008U)
#define SWDT_KICK()  (SWDT_RESTART = 0x1999U)
#define KICK_INTERVAL 1600   /* 1600 × 60µs ≈ 96ms, well under 250ms timer */

/* Fixed delay for sector erase: W25Q256 max 400ms.
 * dly() ≈ 60µs at 33MHz; 8000 × 60µs = 480ms > 400ms.
 * Kick SWDT every KICK_INTERVAL iterations so the 250ms timer never fires. */
static void wait_erase(void) {
    int j;
    for (j = 0; j < 8000; j++) {
        dly();
        if ((j % KICK_INTERVAL) == 0) SWDT_KICK();
    }
}

/* Fixed delay for page program: W25Q256 max 3ms.
 * dly() ≈ 60µs; 60 × 60µs = 3.6ms > 3ms.
 * Also kick SWDT so the timer never expires between pages. */
static void wait_prog(void) {
    int j;
    SWDT_KICK();
    for (j = 0; j < 60; j++) dly();
}

static void qspi_init(void) {
    QSPI_ER       = 0;
    QSPI_LQSPI_CR = 0;        /* disable BootROM linear mode (LQ_MODE bit 31) */
    QSPI_IDR      = 0x7F;
    QSPI_CR       = CR_IDLE;
    QSPI_ER       = 1;
    /* drain RX FIFO */
    while (QSPI_ISR & ISR_RXNE) (void)QSPI_RXD;
}

/* Send one byte, return received byte.
 * Auto-start mode: writing TXD1 immediately kicks off the transfer. */
static u8 xfer(u8 tx) {
    while (QSPI_ISR & ISR_TXF);        /* wait if TX FIFO full */
    QSPI_TXD1 = (u32)tx;               /* byte in [7:0]; starts transfer */
    while (!(QSPI_ISR & ISR_RXNE));    /* wait for received byte */
    return (u8)(QSPI_RXD >> 24);
}

static void cs_on(void)  { QSPI_CR = CR_CS_ON; dly(); }
static void cs_off(void) { QSPI_CR = CR_IDLE;  dly(); }

static void wren(void) {
    cs_on(); xfer(0x06); cs_off(); dly();
}

static void sector_erase(u32 addr) {
    wren();
    cs_on();
    xfer(0xD8);
    xfer((addr >> 16) & 0xFF);
    xfer((addr >>  8) & 0xFF);
    xfer( addr        & 0xFF);
    cs_off();
}

static void page_prog(u32 addr, const u8 *buf) {
    volatile u32 *st = (volatile u32 *)STATUS;
    int i;
    *st = 0xBEEF0031;   /* entered page_prog */
    wren();
    *st = 0xBEEF0032;   /* wren done */
    cs_on();
    *st = 0xBEEF0033;   /* CS asserted */
    xfer(0x02);
    *st = 0xBEEF0034;   /* PP cmd sent */
    xfer((addr >> 16) & 0xFF);
    xfer((addr >>  8) & 0xFF);
    xfer( addr        & 0xFF);
    *st = 0xBEEF0035;   /* address sent */
    for (i = 0; i < 256; i++) {
        if (i == 0)   *st = 0xBEEF0036;
        if (i == 128) *st = 0xBEEF0037;
        xfer(buf[i]);
    }
    *st = 0xBEEF0038;   /* all data sent */
    cs_off();
    *st = 0xBEEF0039;   /* CS deasserted -- PP issued */
}

/* ── main(): init QSPI, read JEDEC ID, then spin for TCL to confirm ──── */
void __attribute__((section(".text"), noreturn)) main(void) {
    volatile u32 *st = (volatile u32 *)STATUS;
    *st = 0xDEAD0001;

    qspi_init();
    *st = 0xDEAD0002;

    /* Read JEDEC ID (cmd 0x9F → 3 bytes: mfr, type, capacity) */
    cs_on();
    xfer(0x9F);
    *(volatile u32 *)JEDEC0 = xfer(0xFF);
    *(volatile u32 *)JEDEC1 = xfer(0xFF);
    *(volatile u32 *)JEDEC2 = xfer(0xFF);
    cs_off();

    *st = 0xDEAD0003;   /* TCL reads JEDEC and decides whether to proceed */
    while (1) __asm__("nop");
}

/* ── program_chunk(): called by TCL for each 32KB block ─────────────── */
void __attribute__((section(".text"), noreturn)) program_chunk(void) {
    volatile u32 *st   = (volatile u32 *)STATUS;
    u32 addr           = *(volatile u32 *)DEST_ADDR;
    u32 do_erase       = *(volatile u32 *)DO_ERASE;
    const u8 *src      = (const u8 *)CHUNK_DATA;
    int p;

    *st = 0xBEEF0001;

    qspi_init();
    *st = 0xBEEF0011;    /* past qspi_init */

    if (do_erase) {
        sector_erase(addr & ~0xFFFFU);
        *st = 0xBEEF0012; /* sector_erase sent, waiting fixed delay */
        wait_erase();
        *st = 0xBEEF0013; /* erase wait done, starting page_prog loop */
    }

    for (p = 0; p < 128; p++) {                  /* 128 pages × 256B = 32KB */
        if (p == 0)  *st = 0xBEEF0020;           /* first page */
        if (p == 64) *st = 0xBEEF0060;           /* halfway */
        page_prog(addr + (u32)(p * 256), src + (u32)(p * 256));
        wait_prog();    /* fixed 3.6ms delay -- avoids re-entering QSPI after 256-byte burst */
    }

    *st = 0xBEEF0002;
    while (1) __asm__("nop");
}
