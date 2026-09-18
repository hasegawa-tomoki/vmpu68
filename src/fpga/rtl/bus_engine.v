// SPDX-License-Identifier: MIT
// 68000 bus master engine — phase 2.
//
// Runs in the PLL (clk16 x 61/16) domain; bus-facing state advances on
// tick16, one pulse per bus clock, phased like a 68000's S-states.
//
// Phase-2 additions:
//  - posted-write command FIFO (64 deep, BRAM): the Pi can fire-and-forget
//    write cycles; reads still poll busy.  cmd_full/cmd_ovf exported.
//  - snoop FIFO in BRAM, SNOOP_AW deep (default 512), with sticky overflow;
//    the address range of dropped records is reported down the stream for
//    the Pi to re-read, and sequential records carry a continuation flag.
//  - vpa_flag: the last completed cycle terminated via VPA/E (autovector
//    hint for IACK); cleared when the next command starts.
//  - berr/timeout flags are sticky, cleared by flags_clr (REG3 bit2).
//  - sequential read prefetch: a read command with pf=1 delivers its data
//    through an 8-word FIFO (pf_head/pf_pop) instead of rdata and, while
//    the bus is otherwise idle, keeps reading the following words into the
//    FIFO on its own ("speculative" cycles).  The Pi then fetches a run of
//    consecutive words with STATUS/DATA reads only.  The stream stops at a
//    64KB boundary (never runs from VRAM into the I/O area), on any bus
//    fault (silently: the fault reappears if the Pi really reads there),
//    on a bus request (fetching stops, buffered words stay readable) and
//    is flushed by the next command from the Pi.
`default_nettype none

module bus_engine #(parameter SNOOP_AW = 9) (
    input  wire        clk,
    input  wire        tick16,
    input  wire        tick16h,     // same, half a bus clock later (falling edge phase)
    input  wire        slow,        // REG3 bit12: legacy 5-tick cycles (END/address on tick16)
    input  wire [1:0]  wr_setup,    // REG3 bit15:14: extra bus clocks (ticks) of write-data setup before AS,
                                    // WEN -> WPRE x n -> ADDR.  At a 16 MHz bus clock main RAM lost about 1 in
                                    // 2000 words (2.6% of $FFFF words) written right after the bus had floated:
                                    // the DRAM sampled the data before the many rising lines had settled.
                                    // One extra tick cut that 100x, two make it zero (docs 27.13).

    // command entry (one pulse per REG2 write)
    input  wire        start,
    input  wire [23:1] addr,
    input  wire        a0,
    input  wire [15:0] wdata,
    input  wire        rw,          // 1 = read
    input  wire        word,
    input  wire [2:0]  fc,
    input  wire        pf,          // read: deliver via the prefetch FIFO and stream on
    output reg  [15:0] rdata,
    output wire        busy,
    output wire        pf_active,   // stream still fetching
    output wire [3:0]  pf_count,    // words in the prefetch FIFO (0..8)
    output wire [15:0] pf_head,
    input  wire        pf_pop,
    output reg         berr_flag,
    output reg         timeout_flag,
    output reg         vpa_flag,
    output wire        cmd_full,
    output wire        cmd_ovf,
    input  wire        flags_clr,

    // 68000 bus — outputs (valid when own=1) and input taps
    output reg         own,
    output reg  [23:1] la_o,
    output reg         as_n_o,
    output reg         uds_n_o,
    output reg         lds_n_o,
    output reg         rw_o,
    output reg  [2:0]  fc_o,
    input  wire [23:1] la_i,
    input  wire [2:0]  fc_i,          // snoop diagnostics only
    input  wire        as_n_i,
    input  wire        uds_n_i,
    input  wire        lds_n_i,
    input  wire        rw_i,

    output reg  [15:0] ld_o,
    output reg         ld_drive,
    input  wire [15:0] ld_i,

    input  wire        dtack_n,
    input  wire        berr_n,
    input  wire        vpa_n,
    input  wire        br_n,
    input  wire        bgack_n,
    output reg         bg_n,

    output reg         vma_req,
    input  wire        vma_done,

    output wire        buf_d_dir,
    output wire        buf_d_oe_n,
    output wire        buf_a_dir,
    output wire        buf_a_oe_n,

    // snoop FIFO read side
    output wire        snoop_avail,
    output wire [47:0] snoop_rec,
    input  wire        snoop_pop,
    output wire        snoop_ovf,
    input  wire        snoop_rst
);

    // Buffer turnaround rule: DIR may only change while the buffer's OE is
    // inactive, so the 8T245 and the FPGA never drive the same line at once
    // (a ns-scale contention that simulation cannot show, but real hardware
    // dissipates).  All four controls are sequenced registers.
    reg a_dir, a_oe_n, d_dir, d_oe_n;
    assign buf_a_dir  = a_dir;
    assign buf_a_oe_n = a_oe_n;
    assign buf_d_dir  = d_dir;
    assign buf_d_oe_n = d_oe_n;

    // ---- input synchronizers (fast domain: ~30ns latency) ----
    wire dtack_s, berr_s, vpa_s, br_s, bgack_s;
    sync2 s0 (.clk(clk), .d(~dtack_n), .q(dtack_s));
    sync2 s1 (.clk(clk), .d(~berr_n),  .q(berr_s));
    sync2 s2 (.clk(clk), .d(~vpa_n),   .q(vpa_s));
    sync2 s3 (.clk(clk), .d(~br_n),    .q(br_s));
    sync2 s4 (.clk(clk), .d(~bgack_n), .q(bgack_s));

    // ---- posted command FIFO ----
    // Each entry records whether BR was already asserted when the Pi posted
    // it (abr).  Commands posted before a bus request are "already executed"
    // from the CPU's point of view and land before the grant; commands the
    // Pi ran ahead and posted while the request was pending wait behind it,
    // as they would have on a 68000 that grants between any two cycles.
    // Without this a run of posted writes held BR off for the whole run
    // (20-60us) and a cycle-steal FDC DMA overran (one byte per 16us).
    localparam CW = 1 + 1 + 23 + 1 + 16 + 1 + 1 + 3;   // 47
    wire [CW-1:0] cmd_head;
    wire cmd_rdy;
    reg  cmd_pop;
    fwft_fifo #(.AW(6), .DW(CW), .AFULL_MARGIN(1)) u_cmdq (
        .clk(clk), .push(start), .din({br_s, pf, addr, a0, wdata, rw, word, fc}),
        .pop(cmd_pop), .head(cmd_head), .rdy(cmd_rdy),
        .full(cmd_full), .afull(), .ovf(cmd_ovf), .ovf_clr(flags_clr)
    );
    wire        c_abr   = cmd_head[46];
    wire        c_pf    = cmd_head[45];
    wire [23:1] c_addr  = cmd_head[44:22];
    wire        c_a0    = cmd_head[21];
    wire [15:0] c_wdata = cmd_head[20:5];
    wire        c_rw    = cmd_head[4];
    wire        c_word  = cmd_head[3];
    wire [2:0]  c_fc    = cmd_head[2:0];

    localparam ST_ADDR1  = 4'd14,
               ST_IDLE   = 4'd0,
               ST_ADDR   = 4'd1,
               ST_WSETUP = 4'd2,
               ST_WEN    = 4'd3,
               ST_STROBE = 4'd4,
               ST_WAIT   = 4'd5,
               ST_VPA    = 4'd6,
               ST_END    = 4'd7,
               ST_END2   = 4'd8,
               ST_GISO   = 4'd9,
               ST_GRANT  = 4'd10,
               ST_GRUN   = 4'd11,
               ST_UNGISO = 4'd12,
               ST_END3   = 4'd13,
               ST_WPRE   = 4'd15;   // wr_setup: one tick with data driven, before AS

    reg [3:0]  st = ST_IDLE;
    reg [11:0] wdog;
    reg [1:0]  pre_cnt;               // WPRE ticks left (wr_setup)
    reg        p_a0; reg [15:0] p_wdata;
    reg        p_rw, p_word;
    reg        p_pf;                 // current cycle: the pf read command itself
    reg        p_spec = 1'b0;        // current cycle: a speculative stream fetch
    reg        p_bad;                // current cycle ended without data (berr/timeout/VPA)

    // ---- prefetch stream + FIFO (8 x 16, flops) ----
    reg        pf_run = 1'b0;
    reg [23:1] pf_a;                 // next word to fetch
    reg [2:0]  pf_fc;
    reg [15:0] pf_q [0:7];
    reg [3:0]  pf_wp = 4'd0, pf_rp = 4'd0;
    wire [3:0] pf_cnt = pf_wp - pf_rp;
    assign pf_active = pf_run;
    assign pf_count  = pf_cnt;
    assign pf_head   = pf_q[pf_rp[2:0]];

    // Bus timing (tick = FSM update, ~R+4..30ns after clk16 rising edge R;
    // see vmpu68_top): a 68000 asserts AS at S2, samples DTACK at the end of
    // S4, latches read data at the end of S6 and negates the strobes in S7.
    //   read : ADDR(AS,DS) -> STROBE(no-op, S3/S4) -> WAIT(DTACK) -> END
    //   write: WSETUP -> WEN(data) -> ADDR(AS) -> STROBE(DS) -> WAIT -> END -> END2 -> END3
    // END latches ld_q1 (the bus sampled one clk before the update) and
    // negates the strobes; write data stays driven until END3 (2 clk >=
    // 50ns hold).
    //
    // A 68000 negates AS/DS in S7, a falling-edge state, and its cycle is
    // 4 bus clocks AS-to-AS.  END therefore advances on tick16h (the
    // falling-edge phase): the data is latched at the S6 end (the 68000's
    // own sample point), the next IDLE tick follows half a clock later
    // and the cycle is 4 clocks.  The address keeps changing in IDLE, a
    // full clock before AS: moving it to the falling edge too (S1, as a
    // 68000 does) cost ROM/VRAM cycles a clock on the XVI, whose DTACK
    // wants the address early; the hold after AS negation is then half a
    // clock, which is what a 68000 guarantees worst-case.  slow=1 keeps
    // END on tick16 (5 clocks), the timing of the first releases.  A
    // write's data is enabled onto the bus in ADDR1 (tick16h) rather than
    // right after IDLE (WEN), so the previous cycle's device has the
    // same clock to let go of the data bus as before.
    //
    // The D-buffer turnaround states (WSETUP, WEN, END2, END3) advance on
    // every clk (~26ns, plenty for the 8T245's OE/DIR switching) instead of
    // on tick16: they are not bus events, and pacing them at bus-clock rate
    // made a write cycle 8 ticks AS-to-AS instead of the 5 of a read.  The
    // fastest tick spacing is 3 clk (61/16), so IDLE -> WSETUP -> WEN lands
    // in ST_ADDR before the tick after the pop, and END -> END2 -> END3 in
    // ST_IDLE before the tick after END.
    reg [15:0] ld_q1;
    always @(posedge clk) ld_q1 <= ld_i;

    wire exec_busy = (st == ST_ADDR) || (st == ST_ADDR1) || (st == ST_WSETUP) || (st == ST_WEN) || (st == ST_WPRE) ||
                     (st == ST_STROBE) || (st == ST_WAIT) || (st == ST_VPA) ||
                     (st == ST_END) || (st == ST_END2) || (st == ST_END3);
    wire fast_st   = (st == ST_WSETUP) || (st == ST_WEN) || (st == ST_END2) || (st == ST_END3);
    // A write always ends on the falling-edge phase (4-clock tail, strobes
    // negated a 68000's S7 after DTACK): with END on tick16 the strobes
    // stay asserted half a bus clock longer after the CRTC's DTACK, and the
    // text VRAM controller then performs a phantom cycle - stray rows of
    // display data land in the text screen (function key row garbage at
    // 16 MHz, docs 30).  Reads keep the slow timing: their data latch
    // margin after DTACK is what the 5-clock cycle was kept for.
    wire half_st   = ((st == ST_END) && (!slow || !p_rw)) || (!slow && (st == ST_ADDR1));
    wire adv       = half_st ? tick16h : (tick16 || fast_st);
    // speculative fetches are not "busy" for the Pi: it waits on pf_count
    assign busy = cmd_rdy || (exec_busy && !p_spec) || start;

    initial begin
        own = 1'b1; bg_n = 1'b1;
        as_n_o = 1'b1; uds_n_o = 1'b1; lds_n_o = 1'b1; rw_o = 1'b1;
        ld_drive = 1'b0; vma_req = 1'b0; cmd_pop = 1'b0; snoop_push = 1'b0;
        berr_flag = 1'b0; timeout_flag = 1'b0; vpa_flag = 1'b0;
        a_dir = 1'b1; a_oe_n = 1'b0; d_dir = 1'b0; d_oe_n = 1'b0;
    end

    // ---- snoop FIFO ----
    reg  [47:0] snoop_din;
    reg         snoop_push;
    wire snoop_afull, snoop_full, snq_rdy, snq_pop;
    wire [47:0] snq_rec;
    // grant DMA only with room for a whole burst (X68000 DMAC bursts a
    // sector = 256+ words while it holds BGACK): 1024 entries, 768 free
    fwft_fifo #(.AW(SNOOP_AW), .DW(48), .AFULL_MARGIN(768)) u_snq (
        .clk(clk), .push(snoop_push), .din(snoop_din),
        .pop(snq_pop), .head(snq_rec), .rdy(snq_rdy),
        .full(snoop_full), .afull(snoop_afull), .ovf(snoop_ovf), .ovf_clr(snoop_rst)
    );

    // Overflow repair.  A memory-to-memory DMA burst writes a word every
    // ~0.8us for as long as it likes; the Pi drains a record in ~1us at
    // best and not at all while its own bus cycle waits behind the burst,
    // so the FIFO can fill and records are lost - a stale word in the
    // shadow, found much later, if ever.  Rather than pretend, the engine
    // keeps the address range [ovf_lo, ovf_hi] of everything it dropped
    // and, once the FIFO has been emptied, hands the range down the same
    // stream as two "range" records (no data strobes, n = 1 for the low
    // bound, n = 2 for the high one; a real record always has a strobe and
    // n = 3).  The Pi then re-reads that range from the real RAM.  The
    // range is latched into pres_* for the two reads so a drop in between
    // cannot tear it; drops during the presentation start a new range.
    wire        snoop_drop = snoop_push & snoop_full;   // what the FIFO refuses
    wire [23:1] drop_a     = snoop_din[43:21];
    reg         rng_valid = 1'b0;
    reg  [23:1] rng_lo, rng_hi;
    reg         pres_valid = 1'b0, pres_ph = 1'b0;
    reg  [23:1] pres_lo, pres_hi;
    always @(posedge clk) begin
        if (!pres_valid && rng_valid && !snq_rdy) begin
            pres_valid <= 1'b1; pres_ph <= 1'b0;
            pres_lo <= rng_lo; pres_hi <= rng_hi;
            rng_valid <= 1'b0;
        end else if (pres_valid && snoop_pop) begin
            if (!pres_ph) pres_ph <= 1'b1;
            else          pres_valid <= 1'b0;
        end
        if (snoop_drop) begin
            rng_valid <= 1'b1;
            if (!rng_valid || drop_a < rng_lo) rng_lo <= drop_a;
            if (!rng_valid || drop_a > rng_hi) rng_hi <= drop_a;
        end
        if (snoop_rst) begin
            rng_valid <= 1'b0; pres_valid <= 1'b0;
        end
    end
    assign snq_pop = snoop_pop & ~pres_valid;
    assign snoop_avail = pres_valid | snq_rdy;
    assign snoop_rec   = pres_valid ? {4'b0000, (pres_ph ? pres_hi : pres_lo), 3'b000, pres_ph, ~pres_ph, 16'h0000}
                                    : snq_rec;
    // ({cont, rw, uds, lds, A23:1, fc, n[1:0], data}: n = 01 lo / 10 hi)

    // Continuation flag (record bit 47): the record's address is the
    // previous pushed record's + 2 and it is a word write.  The Pi then
    // skips the LO read (2 register reads per record instead of 3 - a
    // sequential burst is drained faster than the DMAC writes it).  A
    // dropped record breaks the chain so the next one carries a full
    // address again; so does a stream reset.
    reg         last_ok = 1'b0;
    reg  [23:1] last_a;
    always @(posedge clk) begin
        if (snoop_push) begin
            last_a  <= snoop_din[43:21];
            last_ok <= 1'b1;
        end
        if (snoop_drop || snoop_rst) last_ok <= 1'b0;
    end
    wire snoop_cont = last_ok && snoop_hold[45] && snoop_hold[44] &&
                      (snoop_hold[43:21] == last_a + 23'd1);

    // Snoop capture runs at clk rate, NOT on tick16: a DMAC in single-
    // address (device->memory) burst mode issues back-to-back write cycles
    // whose AS-high gap (~1 bus clock minus the DMAC's AS delays) can fall
    // entirely between two ticks; a tick-based "AS went high" test then
    // merges two cycles into one record and the shadow loses a word.
    //
    // Every decision is taken on 2-stage synchronised control lines: with
    // several flops sampling the raw AS/strobes/R/W, a transition landing
    // in the sampling window is seen by some flops and not others, which
    // showed up as a phase-periodic (61/16 clock beat) loss of ~4% of the
    // records plus read cycles logged as writes.  Address/data ride a
    // pipeline aligned with the synchronised controls; the record takes
    // the sample one clock BEFORE the last in-cycle one (26ns clear of the
    // strobe negation, where the master may drop the data with 0ns hold)
    // and R/W from the FIRST in-cycle sample (a 68000-class master drives
    // R/W a bus clock before the write strobes and may return it high
    // together with them).
    reg [2:0] as_s = 3'b111, uds_s = 3'b111, lds_s = 3'b111, rw_s = 3'b111;
    reg [47:0] smp_p1, smp_p2, smp_p3;       // raw samples, p3 aligned with *_s[2]
    always @(posedge clk) begin
        as_s  <= {as_s[1:0],  as_n_i};
        uds_s <= {uds_s[1:0], uds_n_i};
        lds_s <= {lds_s[1:0], lds_n_i};
        rw_s  <= {rw_s[1:0],  rw_i};
        smp_p1 <= {2'b00, ~uds_n_i, ~lds_n_i, la_i, fc_i, 2'b00, ld_i};
        smp_p2 <= smp_p1;
        smp_p3 <= smp_p2;
    end
    wire cyc_s    = ~as_s[1] & ~(uds_s[1] & lds_s[1]);   // strobed cycle, synchronised
    wire cyc_prev = ~as_s[2] & ~(uds_s[2] & lds_s[2]);
    wire cyc_end  = cyc_prev & ~cyc_s;
    reg [47:0] snoop_hold;
    reg        snoop_wr;
    reg [1:0]  cyc_n;                                   // in-cycle samples - 1, saturating
    always @(posedge clk) begin
        if (cyc_s && !cyc_prev) snoop_wr <= ~rw_s[1];   // first in-cycle sample
        if (cyc_s) snoop_hold <= smp_p3;                // one clock behind *_s[1]
        if (cyc_s) cyc_n <= cyc_prev ? cyc_n + {1'b0, cyc_n != 2'd3} : 2'd0;
    end
    // (GRANT+BGACK covers the DMAC's first cycle before the tick-paced FSM
    // has reached GRUN)
    wire snooping  = (st == ST_GRUN) || (st == ST_GRANT && bgack_s);
    // A cycle seen while another master owns the bus is "armed" and pushed
    // at its end even if the FSM has already left GRUN by then: the DMAC
    // negates BGACK within a clock of its last AS negation, and the two
    // synchronizers plus the tick-paced FSM could otherwise drop that last
    // record (seen as one stale word in the shadow per ~10^5 DMA writes).
    reg snoop_arm = 1'b0;
    always @(posedge clk)
        if (snooping && cyc_s) snoop_arm <= 1'b1;
        else if (cyc_end)      snoop_arm <= 1'b0;

    always @(posedge clk) begin
        cmd_pop <= 1'b0;
        snoop_push <= 1'b0;

        if (flags_clr) begin
            berr_flag <= 1'b0;
            timeout_flag <= 1'b0;
        end

        // a real 68000-style write cycle spans >= 4 bus clocks (>= 11 samples);
        // shorter pulses are the bus floating around DMAC grant/release
        if (snoop_arm && cyc_end && snoop_wr && cyc_n == 2'd3) begin
            // continuation flag [47]; diagnostics in the spare bits: R/W at
            // the last in-cycle sample [46], FC [20:18], sample count-1 [17:16]
            snoop_din  <= {snoop_cont, rw_s[2], snoop_hold[45:18], cyc_n, snoop_hold[15:0]};
            snoop_push <= 1'b1;
        end

        // prefetch FIFO pop (end of a DATA read strobe); a flush below wins
        if (pf_pop && pf_cnt != 4'd0)
            pf_rp <= pf_rp + 4'd1;

        if (adv) case (st)
        ST_IDLE: begin
            as_n_o <= 1'b1; uds_n_o <= 1'b1; lds_n_o <= 1'b1; rw_o <= 1'b1;
            ld_drive <= 1'b0;
            own  <= 1'b1;
            bg_n <= 1'b1;
            vma_req <= 1'b0;
            a_oe_n <= 1'b0;
            d_oe_n <= 1'b0;
            // commands posted before BR land first; then the grant (throttled
            // while the snoop FIFO drains); then the commands posted while
            // the request was pending (see the FIFO).  Speculative fetches
            // come last.
            if (br_s && !snoop_afull && (!cmd_rdy || c_abr)) begin
                pf_run <= 1'b0;          // another master: stop fetching (keep the words)
                bg_n   <= 1'b0;
                a_oe_n <= 1'b1;          // isolate before turnaround
                own    <= 1'b0;
                wdog   <= 12'd0;
                st <= ST_GISO;
            end else if (cmd_rdy) begin
                la_o    <= c_addr;
                p_a0    <= c_a0;
                p_wdata <= c_wdata;
                p_rw    <= c_rw;
                p_word  <= c_word;
                p_pf    <= c_pf && c_rw;
                p_spec  <= 1'b0;
                p_bad   <= 1'b0;
                rw_o    <= c_rw;
                fc_o    <= c_fc;
                cmd_pop <= 1'b1;
                vpa_flag <= 1'b0;
                wdog <= 12'd0;
                pf_run <= 1'b0;          // any command flushes the prefetch stream
                pf_wp  <= 4'd0;
                pf_rp  <= 4'd0;
                if (c_rw)
                    st <= ST_ADDR;
                else begin
                    d_oe_n <= 1'b1;      // write: turn the D buffer around BEFORE AS
                    st <= ST_WSETUP;     // so data is valid when AS asserts (68000: S3)
                end
            end else if (br_s) begin
                pf_run <= 1'b0;          // request pending, snoop FIFO full: wait
            end else if (pf_run && pf_cnt != 4'd8) begin
                la_o    <= pf_a;         // speculative word read, same FC as the command
                p_a0    <= 1'b0;
                p_rw    <= 1'b1;
                p_word  <= 1'b1;
                p_pf    <= 1'b0;
                p_spec  <= 1'b1;
                p_bad   <= 1'b0;
                rw_o    <= 1'b1;
                fc_o    <= pf_fc;
                wdog <= 12'd0;
                st <= ST_ADDR;
            end
        end

        ST_WSETUP: begin                 // buffer is off: flip and drive
            ld_o     <= p_word ? p_wdata : {p_wdata[7:0], p_wdata[7:0]};
            d_dir    <= 1'b1;
            ld_drive <= 1'b1;
            st <= slow ? ST_WEN : ST_ADDR1;
        end

        ST_WEN: begin
            d_oe_n <= 1'b0;              // data now flows to the bus
            pre_cnt <= wr_setup - 2'd1;
            st <= (wr_setup != 2'd0) ? ST_WPRE : ST_ADDR;
        end

        ST_WPRE: begin                   // (tick) data settles n bus clocks before AS
            if (pre_cnt != 2'd0) pre_cnt <= pre_cnt - 2'd1;
            else st <= ST_ADDR;
        end

        ST_ADDR1: begin                  // 68000 S1 (falling edge): write data onto the bus
            d_oe_n <= 1'b0;              // (WEN's job; the buffer was flipped in WSETUP)
            st <= ST_ADDR;
        end

        ST_ADDR: begin                   // 68000 S2: assert AS (address + R/W set earlier)
            as_n_o <= 1'b0;
            if (p_rw) begin              // read: DS together with AS
                uds_n_o <= !(p_word || !p_a0);
                lds_n_o <= !(p_word ||  p_a0);
            end
            st <= ST_STROBE;             // write: DS one clock later (68000 S4);
        end                              // read: DTACK is not sampled before S4

        ST_STROBE: begin
            uds_n_o <= !(p_word || !p_a0);
            lds_n_o <= !(p_word ||  p_a0);
            st <= ST_WAIT;
        end

        ST_WAIT: begin
            wdog <= wdog + 12'd1;
            if (dtack_s)
                st <= ST_END;            // S5/S6: data valid by the end of S6
            else if (berr_s) begin
                if (!p_spec) berr_flag <= 1'b1;   // a speculative fault is not the Pi's
                p_bad <= 1'b1;
                st <= ST_END;
            end else if (vpa_s) begin
                if (p_pf || p_spec) begin // no E-cycle for a streamed read: just end it
                    p_bad <= 1'b1;       // (the Pi never streams from the I/O area)
                    st <= ST_END;
                end else begin
                    vma_req <= 1'b1;
                    st <= ST_VPA;
                end
            end else if (&wdog) begin
                if (!p_spec) timeout_flag <= 1'b1;
                p_bad <= 1'b1;
                st <= ST_END;
            end
        end

        ST_VPA: begin
            if (!vma_done && p_rw)
                rdata <= p_word ? ld_i : (p_a0 ? {8'h00, ld_i[7:0]} : {8'h00, ld_i[15:8]});
            if (vma_done) begin
                vma_req <= 1'b0;
                vpa_flag <= 1'b1;
                st <= ST_END;
            end
        end

        ST_END: begin                    // S7: latch data (sampled at S6 end), strobes up
            if (p_pf || p_spec) begin    // streamed read: data goes to the FIFO
                if (!p_bad) begin
                    pf_q[pf_wp[2:0]] <= ld_q1;
                    pf_wp <= pf_wp + 4'd1;
                end
                // keep streaming until a fault or the end of the 64KB block
                pf_run <= !p_bad && !(&la_o[15:1]) && (p_pf || pf_run);
                pf_a   <= la_o + 23'd1;
                if (p_pf) pf_fc <= fc_o;
            end else if (p_rw && !vpa_flag)   // VPA cycles latched their data in ST_VPA
                rdata <= p_word ? ld_q1 : (p_a0 ? {8'h00, ld_q1[7:0]} : {8'h00, ld_q1[15:8]});
            as_n_o <= 1'b1; uds_n_o <= 1'b1; lds_n_o <= 1'b1;
            st <= p_rw ? ST_IDLE : ST_END2;   // write data stays driven one more clock
        end

        ST_END2: st <= ST_END3;          // data hold after the strobes negated

        ST_END3: begin                   // release the data bus, buffer back to input
            d_oe_n   <= 1'b1;
            ld_drive <= 1'b0;
            d_dir    <= 1'b0;
            st <= ST_IDLE;
        end

        // ---- bus grant + snoop ----
        ST_GISO: begin                       // pins are Z, A buffer is off
            a_dir <= 1'b0;                   // turn around toward snoop
            st <= ST_GRANT;
        end

        ST_GRANT: begin                      // BG# asserted, wait BGACK#
            a_oe_n <= 1'b0;                  // buffer on, snooping
            // BGACK assertion and BR release arrive near-simultaneously from
            // a real DMAC; their separate synchronizers can skew by a clock.
            // Only treat BR-release as a withdrawal after a grace period,
            // and always give BGACK priority.
            if (bgack_s) begin
                bg_n <= 1'b1;
                st <= ST_GRUN;
            end else if (!br_s) begin
                wdog <= wdog + 12'd1;
                if (wdog >= 12'd4) begin
                    bg_n <= 1'b1;
                    a_oe_n <= 1'b1;
                    st <= ST_UNGISO;
                end
            end else
                wdog <= 12'd0;
        end

        ST_GRUN: begin                       // alternate master owns the bus (snooping)
            // keep the A buffer (address + strobes) on until the last cycle's
            // AS is seen negated, so its record is captured from real pins
            if (!bgack_s && as_s[1]) begin
                a_oe_n <= 1'b1;
                st <= ST_UNGISO;
            end
        end

        ST_UNGISO: begin                     // buffer off: flip back and drive
            a_dir <= 1'b1;
            own   <= 1'b1;
            st <= ST_IDLE;
        end

        default: st <= ST_IDLE;
        endcase
    end

endmodule
