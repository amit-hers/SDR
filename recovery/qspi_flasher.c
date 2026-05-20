/* Zynq-7000 QSPI flasher - auto-start mode, 32KB chunks, OCM bank2 only */
typedef unsigned int  u32;
typedef unsigned char u8;

/* QSPI controller registers */
#define QSPI_BASE     0xE000D000U
#define QSPI_CR       (*(volatile u32*)(QSPI_BASE + 0x00))
#define QSPI_ISR      (*(volatile u32*)(QSPI_BASE + 0x04))
#define QSPI_IDR      (*(volatile u32*)(QSPI_BASE + 0x0C))
#define QSPI_ER       (*(volatile u32*)(QSPI_BASE + 0x14))
#define QSPI_TXD0     (*(volatile u32*)(QSPI_BASE + 0x1C))
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
 * data is written to TXD0 — no extra MANSTRT trigger needed.
 */
#define CR_IDLE     ((1u<<31)|(1u<<14)|(1u<<10)|(7u<<3)|(1u<<0))
#define CR_CS_ON    ((1u<<31)|(1u<<14)|(0u<<10)|(7u<<3)|(1u<<0))

/* All slots in OCM bank2 (0x20000-0x2FFFF, 64KB) */
#define CHUNK_DATA  0x00021000U   /* 32KB data buffer */
#define STATUS      0x00029000U   /* handshake */
#define DEST_ADDR   0x00029004U   /* QSPI target address */
#define DO_ERASE    0x00029008U   /* 1 = erase 64KB sector first */
#define JEDEC0      0x0002900CU   /* JEDEC ID byte 0 (manufacturer) */
#define JEDEC1      0x00029010U   /* JEDEC ID byte 1 (memory type)  */
#define JEDEC2      0x00029014U   /* JEDEC ID byte 2 (capacity)     */

static void dly(void) {
    volatile int i;
    for (i = 0; i < 2000; i++) __asm__("nop");
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
 * Auto-start mode: writing TXD0 immediately kicks off the transfer. */
static u8 xfer(u8 tx) {
    while (QSPI_ISR & ISR_TXF);        /* wait if TX FIFO full */
    QSPI_TXD0 = (u32)tx << 24;         /* byte in [31:24]; starts transfer */
    while (!(QSPI_ISR & ISR_RXNE));    /* wait for received byte */
    return (u8)(QSPI_RXD >> 24);
}

static void cs_on(void)  { QSPI_CR = CR_CS_ON; dly(); }
static void cs_off(void) { QSPI_CR = CR_IDLE;  dly(); }

/* MISO is stuck at 0 on this board, so RDSR polling is unreliable.
 * Use fixed unconditional delays instead: dly()≈15µs at 667MHz.
 * wait_erase: 300000×15µs ≈ 4.5s  (64KB erase worst-case ~2s)
 * wait_prog:  2000×15µs   ≈ 30ms  (page-prog worst-case ~5ms) */
static void wait_erase(void) {
    int t;
    for (t = 0; t < 300000; t++) dly();
}

static void wait_prog(void) {
    int t;
    for (t = 0; t < 2000; t++) dly();
}

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
    int i;
    wren();
    cs_on();
    xfer(0x02);
    xfer((addr >> 16) & 0xFF);
    xfer((addr >>  8) & 0xFF);
    xfer( addr        & 0xFF);
    for (i = 0; i < 256; i++) xfer(buf[i]);
    cs_off();
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
        *st = 0xBEEF0012; /* sector_erase sent, starting wait_erase */
        wait_erase();   /* fixed delay: ~4.5s — MISO unreliable, can't poll RDSR */
        *st = 0xBEEF0013; /* wait_erase done, starting page_prog loop */
    }

    for (p = 0; p < 128; p++) {                  /* 128 pages × 256B = 32KB */
        if (p == 0)  *st = 0xBEEF0020;           /* first page */
        if (p == 64) *st = 0xBEEF0060;           /* halfway */
        page_prog(addr + (u32)(p * 256), src + p * 256);
        wait_prog();    /* fixed delay: ~30ms per page */
    }

    *st = 0xBEEF0002;
    while (1) __asm__("nop");
}
