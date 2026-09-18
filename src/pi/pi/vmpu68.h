/* vmpu68 Raspberry Pi <-> iCE40 interface definitions.
 *
 * GPIO map (BCM numbering), board 1.2 CN3 wiring:
 *   AD0-AD11   = GPIO 2..13
 *   AD12/AD13  = GPIO 0/1    (moved off 14/15 to free the UART, board 1.2)
 *   AD14/AD15  = GPIO 16/17
 *   UART TX/RX = GPIO 14/15  (free: serial console / bare-metal debug)
 *   REG_A0/A1  = GPIO 18/19
 *   WR#        = GPIO 20
 *   RD#        = GPIO 21
 *   IRQ        = GPIO 22  (input, level high = attention)
 *   CRESET_B   = GPIO 23  (low = hold FPGA in reset; flash access window)
 *   SPI_SS#    = GPIO 24   \
 *   SPI_SCK    = GPIO 25    | bit-banged W25Q32 access while CRESET_B low
 *   SPI_MOSI   = GPIO 26    |
 *   SPI_MISO   = GPIO 27   /
 *
 * NOTE: GPIO2/3 (AD0/AD1) carry the Pi's fixed 1.8k I2C pull-ups.  The AD
 * bus is always actively driven during strobes so this only biases the idle
 * state; do not interpret idle AD levels.
 *
 * PROTOCOL TIMING: the FPGA phase-1 core samples the strobes at 16MHz.
 * Strobes must stay low >= 250ns and the whole write cycle (data valid ->
 * WR# low -> WR# high) must not be faster than ~375ns.  vmpu68_pace()
 * enforces this; do not remove it "because it works on the bench".
 *
 * Register map (REG_A1:A0):
 *   0 DATA    W: write-data           R: read-data of completed cycle
 *   1 ADDR_LO W: addr[15:0]           R: snoop stream (HI, LO, DATA seq)
 *   2 CTRL    W: {fc[14:12], word[9], rw[8], addr[23:16]} -> starts cycle
 *             R: {0x56, reserved}
 *   3 STATUS  R: bit0 busy, bit1 berr|timeout, bit2 snoop_avail,
 *                bits5:3 ipl, bit6 reset_in, bit7 halt_in
 *             W: bit0 drv_reset, bit1 drv_halt, bits5:3 led,
 *                bit6 irq_en, bit7 snoop_phase_reset
 */
#ifndef VMPU68_H
#define VMPU68_H

#include <stdint.h>

/* Board GPIO layouts.  The pin numbers below are the core 1.x layout and
 * are kept for the Linux tools (flash68, buscheck); the driver itself
 * selects a layout at run time (vmpu68_set_board / vmpu68_probe_board):
 *   1.x: AD0-11 -> 2..13, AD12/13 -> 0/1, AD14/15 -> 16/17, REG_A 18/19,
 *        WR# 20, RD# 21, IRQ 22, CRESET 23, SS 24, SCK 25, MOSI 26, MISO 27,
 *        console UART0 (GPIO14/15)
 *   2.x: AD0-15 -> 8..23 (SMI SD0-15), REG_A0/A1 5/4, WR# 7, RD# 6, IRQ 24,
 *        CRESET 3, SS 2, SCK 25, MOSI 26, MISO 27, console UART2 (GPIO0/1) */
#define VMPU68_AD_MASK    0x00033FFFu
#define VMPU68_REGA_SHIFT 18
#define VMPU68_GPIO_WR    20
#define VMPU68_GPIO_RD    21
#define VMPU68_GPIO_IRQ   22
#define VMPU68_GPIO_CRESET 23
#define VMPU68_GPIO_SS    24
#define VMPU68_GPIO_SCK   25
#define VMPU68_GPIO_MOSI  26
#define VMPU68_GPIO_MISO  27

typedef struct {
    int id;                       /* 1 = core 1.x, 2 = core 2.x */
    const char *name;
    uint8_t ad[16];               /* ADn -> BCM GPIO */
    uint8_t rega0, rega1, wr, rd, irq, creset, ss, sck, mosi, miso;
    uint8_t uart;                 /* console UART device (circle): 0 = GPIO14/15, 2 = GPIO0/1 */
} vmpu68_board_t;
const vmpu68_board_t *vmpu68_board(void);
int  vmpu68_board_id(void);
void vmpu68_set_board(int id);    /* 1 or 2; call before vmpu68_open() */
int  vmpu68_probe_board(void);    /* try the 2.x then the 1.x layout for the FPGA signature;
                                     returns the id that answered (left selected) or 0 (pins released) */

#define VREG_DATA    0
#define VREG_ADDR_LO 1
#define VREG_CTRL    2
#define VREG_STATUS  3

#define VCTRL_RW_READ  (1u << 8)
#define VCTRL_WORD     (1u << 9)
#define VCTRL_PF       (1u << 10)  /* read via the prefetch FIFO + stream the next words */
#define VCTRL_WAIT     (1u << 11)  /* the host waits for this command: PI_IRQ high until done */
#define VCTRL_FC(x)    ((uint16_t)((x) & 7) << 12)
#define VCTRL_AI       (1u << 15)  /* writes: a DATA write starts the cycle, address auto-advances */
#define VCTRL_WR_DEC   VCTRL_PF    /* writes with VCTRL_AI: advance downward (pf has no meaning for writes) */

#define VST_BUSY        (1u << 0)
#define VST_FAULT       (1u << 1)   /* sticky; clear via VSTW_FLAG_CLR */
#define VST_SNOOP       (1u << 2)
#define VST_IPL(st)     (((st) >> 3) & 7)
#define VST_RESET_IN    (1u << 6)
#define VST_HALT_IN     (1u << 7)
#define VST_VPA         (1u << 8)   /* last cycle ended via VPA (autovector) */
#define VST_SNOOP_OVF   (1u << 9)   /* sticky; clear via VSTW_SNOOP_RST */
#define VST_CMD_FULL    (1u << 10)  /* posted-write FIFO full: do not post */
#define VST_CMD_OVF     (1u << 11)  /* sticky: a posted command was DROPPED */
#define VST_PF_CNT(st)  (((st) >> 12) & 3)  /* words in the prefetch FIFO (saturated at 3) */
#define VST_PF_ACTIVE   (1u << 14)  /* the prefetch stream is still fetching */
#define VST_BUSY_IRQ    (1u << 15)  /* echo of VSTW_BUSY_IRQ (0 = bitstream lacks it) */

#define VSTW_DRV_RESET  (1u << 0)
#define VSTW_DRV_HALT   (1u << 1)
#define VSTW_FLAG_CLR   (1u << 2)
#define VSTW_LED(x)     ((uint16_t)((x) & 7) << 3)
#define VSTW_IRQ_EN     (1u << 6)
#define VSTW_SNOOP_RST  (1u << 7)
#define VSTW_AUTOSTART  (1u << 8)   /* REG1 write starts the cycle; REG2 only sets fields */
#define VSTW_BUSY_IRQ   (1u << 9)   /* PI_IRQ |= a VCTRL_WAIT command is pending */
#define VSTW_HELLO      (1u << 10)  /* host marker, read back as VREG_CTRL bit0: 0 = FPGA reconfigured */
#define VSTW_RST_CLR    (1u << 11)  /* pulse: clear VDIAG_RST_SEEN */
#define VSTW_BUS_SLOW   (1u << 12)  /* 5-tick bus cycles (0.1.5-0.1.16 timing); 0 = 4-tick like a 68000 (default since 0.1.17, docs 30) */
#define VSTW_SNOOP2     (1u << 13)  /* snoop stream v2 (see vmpu68_snoop_pop); echoed at VDIAG_SNOOP2 */
#define VSTW_WR_SETUP(n) ((uint16_t)(((n) & 3u) << 14))  /* bits 15:14: extra bus clocks of write-data setup before AS (16 MHz: 2); older bitstreams ignore it */
#define VSTW_WR_SETUP_MASK (3u << 14)
#define VDIAG_HELLO     (1u << 0)   /* VREG_CTRL read: echo of VSTW_HELLO */
#define VDIAG_RST_SEEN  (1u << 1)   /* VREG_CTRL read: sticky, RESET_IN+HALT_IN seen (>= 250 ns) while not
                                       driving RESET ourselves - the reset button's pulse is only ~25 us.
                                       Older bitstreams have the raw WR# level here: see hw_rst_seen() */
#define VDIAG_WR_AI     (1u << 2)   /* VREG_CTRL read: the bitstream implements VCTRL_AI (older: RD# sync level) */
#define VDIAG_SNOOP2    (1u << 3)   /* VREG_CTRL read: echo of VSTW_SNOOP2 (older: WR# level, reads 1) */
#define VDIAG_PLL_LOCK  (1u << 7)   /* VREG_CTRL read: bus clock PLL locked (X68000 on) */
#define VDIAG_HB_FAST   (1u << 6)   /* VREG_CTRL read: toggles every 2^21 PLL clocks while it runs */
#define VDIAG_HB_CLK16  (1u << 5)   /* VREG_CTRL read: toggles every 2^19 bus clocks (33 ms @16 MHz) */

int  vmpu68_open(void);                 /* mmap /dev/gpiomem, init pins  */
void vmpu68_close(void);

void     vmpu68_reg_write(unsigned reg, uint16_t val);
uint16_t vmpu68_reg_read(unsigned reg);
int      vmpu68_irq_pin(void);          /* PI_IRQ level: irq_en & (ipl!=0 | snoop) */

/* 68000 bus cycles (blocking; returns STATUS after completion) */
uint16_t vmpu68_bus_read (uint32_t addr, int word, uint16_t *out);
uint16_t vmpu68_bus_read_fc(uint32_t addr, int word, unsigned fc, uint16_t *out);
uint16_t vmpu68_bus_write(uint32_t addr, int word, uint16_t data);
void     vmpu68_bus_write_posted(uint32_t addr, int word, uint16_t data);  /* no completion wait */
uint16_t vmpu68_bus_drain(void);        /* wait until the command queue is empty */
/* word read through the FPGA's sequential prefetch stream: when addr is the
 * word after the previous vmpu68_bus_read_pf() and nothing else touched the
 * bus since, the data is popped from the FIFO (STATUS/DATA reads only,
 * ~0.5us instead of ~1.7us); otherwise a new stream starts at addr.  Only
 * for side-effect-free memory (RAM/VRAM/ROM), never for the I/O area. */
uint16_t vmpu68_bus_read_pf(uint32_t addr, uint16_t *out);
void     vmpu68_pf_enable(int on);      /* 0 = plain reads (diagnostics) */
int      vmpu68_pf_enabled(void);
void     vmpu68_pf_stats(uint32_t out[6], int clear);  /* hits, misses, polls, aborts */
void     vmpu68_pace_bench(unsigned n); /* n pacing units (timing diagnostics) */
/* minimum time between GPIO strobe edges (WR#/RD# pulse width, setup) */
#define VMPU68_PACE_NS_DEFAULT 60
void     vmpu68_set_pace_ns(unsigned ns);
unsigned vmpu68_get_pace_ns(void);
/* delay before the first STATUS poll after a bus command (0 = three paces).
 * A bus cycle takes >= 5 ticks (500ns) in the FPGA and each poll that finds
 * busy costs a ~350ns GPIO round trip: with 500ns ~90% of the reads need one
 * poll (measured: word read 2.1 -> 1.95us).  Poll count histogram for tuning. */
#define VMPU68_WAIT_NS_DEFAULT 500
#define VMPU68_POLL_HIST 8
void     vmpu68_set_wait_ns(unsigned ns);
unsigned vmpu68_get_wait_ns(void);
void     vmpu68_poll_hist(uint32_t *out, int clear);
void     vmpu68_wait_prof(uint64_t out[12], int clear);
void     vmpu68_lat_probe(uint32_t addr, uint32_t out[4]); /* bench: line up/down/STATUS latency of one read */ /* ticks: start, pre, poll, data; [4]=n */
/* free-running high resolution counter (ARM generic timer, 54MHz on Pi 4) */
uint64_t vmpu68_ticks(void);
uint64_t vmpu68_tick_hz(void);
void     vmpu68_ctrl_invalidate(void);  /* next cycle rewrites CTRL (after REG3/FPGA reset) */
void     vmpu68_set_autostart_io(int on); /* match the FPGA cycle mode (1=2-write, 0=3-write) */
void     vmpu68_set_wr_ai_io(int on);   /* VCTRL_AI write mode (needs autostart + VDIAG_WR_AI) */
int      vmpu68_wr_ai_io(void);
/* busy-on-PI_IRQ completion: with VSTW_BUSY_IRQ set in the FPGA, a waited
 * command holds PI_IRQ high until it has completed, so wait_done() spins on
 * one GPIO read (~50ns) instead of STATUS reads (~360ns each).  The line is
 * also high while an interrupt/snoop record is pending or a cycle is long:
 * after VMPU68_PIN_POLLS it falls back to STATUS (which also reports the
 * fault flag - a faulting cycle therefore leaves the line high via the
 * sticky flag until the host clears it).  0 = STATUS polling only. */
#define VMPU68_PIN_POLLS 24         /* high -> low: the bus cycle itself */
#define VMPU68_PIN_UP_POLLS 12      /* low -> high: write-buffer drain + FPGA sync */
void     vmpu68_set_busy_irq_io(int on);
int      vmpu68_busy_irq_io(void);
void     vmpu68_set_irq_en_io(int en);  /* mirror of VSTW_IRQ_EN: ipl/snoop then hold the line too */

/* snoop record: one DMA write observed on the bus.  A record without
 * strobes is a bound of the address range whose writes the FPGA had to drop
 * (FIFO overflow): VSNOOP_N() = 1 low bound, 2 high bound, and the host
 * re-reads [lo, hi] from the real RAM.  Real records have VSNOOP_N() = 3. */
typedef struct {
    uint32_t addr;      /* byte address, bit0 = 0 */
    uint16_t data;
    uint8_t  uds, lds;
    uint8_t  aux;       /* {rw_last|cont[5], fc[4:2], nsamp-1[1:0]} */
} vmpu68_snoop_t;
#define VSNOOP_N(aux)     ((aux) & 3)
#define VSNOOP_RANGE_LO   1
#define VSNOOP_RANGE_HI   2
int vmpu68_snoop_pop(vmpu68_snoop_t *rec);  /* 1 = got one, 0 = empty */
/* stream v2 (VSTW_SNOOP2, needs the bitstream): the HI word reads 0 while
 * the FIFO is empty (no STATUS read per record) and a record whose address
 * is the previous one's + 2 comes without its LO word (2 reads instead of
 * 3).  vmpu68_snoop_resync() after a stream reset. */
void vmpu68_set_snoop2_io(int on);
int  vmpu68_snoop2_io(void);
void vmpu68_snoop_resync(void);
/* called from the busy-wait loops whenever STATUS shows busy together with
 * a pending snoop record - during a DMA burst holding the bus the host has
 * nothing else to do, and the FIFO would otherwise fill behind its back.
 * The hook may only pop records (no bus commands). */
void vmpu68_set_snoop_hook(void (*fn)(void));
void     vmpu68_set_snoop_hook_core(int core);   /* only this core runs the hook from busy waits (-1 = any) */

/* flash access (CRESET_B held low during the whole session) */
void vmpu68_flash_begin(void);
void vmpu68_flash_end(void);            /* releases CRESET_B -> FPGA boots */
void vmpu68_flash_xfer(const uint8_t *tx, uint8_t *rx, unsigned n, int cont);

#endif
