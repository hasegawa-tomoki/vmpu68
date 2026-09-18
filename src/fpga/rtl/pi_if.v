// Raspberry Pi register interface.
//
// The Pi bus is fully asynchronous: 16-bit multiplexed AD bus, two register
// select lines, WR#/RD# strobes.  There is NO ready/ack back to the Pi, so
// every readable register must be servable combinationally from registered
// state, and the Pi must respect minimum strobe widths (>= 3 clk16 = ~190ns).
//
// Register map (pi_reg_a):
//   0 DATA    W: write-data latch            R: read-data of last bus cycle
//   1 ADDR_LO W: addr[15:0] (bit0 = A0)      R: snoop stream (HI,LO,DATA seq)
//              HI = {uds, lds, rw|cont, fc[2:0], n[1:0], A23:16}, LO = {A15:1, 0}
//              snoop2 (REG3 bit13) stream: HI reads 0 while no record is
//              available (no STATUS read needed per record); HI bit13 = cont:
//              the address is the previous real record's + 2 and the LO word
//              is skipped (HI, DATA); a record without strobes is a range
//              bound (n=1 low, n=2 high) of writes lost to a FIFO overflow.
//   2 CTRL    W: {ai[15], fc[14:12], wt[11], pf[10], word[9], rw[8], addr[23:16]}  -> starts cycle
//              pf=1 (reads): data via the prefetch FIFO + stream the following words
//              ai=1 (writes, rw=0): a REG0 (DATA) write starts the cycle at the
//                    current address and then advances the address by the access
//                    size, downward when pf=1 (pf has no meaning for writes).  A
//                    REG1 write only sets the address.  Sequential writes (fills,
//                    copies, stack frames) are then one strobe each instead of two.
//              wt=1: the Pi waits for this command -> PI_IRQ is held high from the
//                    start of the command until the engine is idle again (busy_irq
//                    mode), so completion costs one GPIO read instead of a STATUS
//                    read.  Posted writes leave wt=0 and never touch the line.
//              R: {8'h56, pll_lock, hb_fast, hb_clk16, armed, snoop2, ai_ok, rst_seen, hello}
//                 (0x56 = 'V' signature; low byte = diagnostics.  hello = REG3
//                 bit10 as last written: 0 after (re)configuration, so a host that set it
//                 at init can tell the FPGA has been reloaded behind its back - the board
//                 is powered from the X68000, the Pi is not.  rst_seen: sticky, RESET_IN
//                 and HALT_IN were both asserted for >= 16 clocks while the host was not
//                 driving RESET itself - the machine's reset button gives a pulse of only
//                 ~25 us (measured), enough for a real 68000 but far too short for a
//                 polling host; cleared by REG3 bit11.  ai_ok: constant 1, the
//                 bitstream implements the CTRL ai bit.  snoop2: REG3 bit13 as
//                 last written - older bitstreams show the idle WR# level, 1,
//                 there, so the host probes by writing 0 and reading 0 back)
//   3 STATUS  R: {busy_irq[15], pf_active[14], pf_cnt[13:12] (sat. 3), cmd_ovf, cmd_full,
//                 snoop_ovf, vpa, halt_in, reset_in, ipl[2:0], snoop_avail, berr|timeout, busy}
//   0 DATA    R: while pf_cnt != 0 the FIFO head; the read strobe pops it
//             W: {wr_setup[15:14], snoop2[13], bus_slow[12], rst_clr[11], hello[10], busy_irq[9], autostart[8], snoop_rst[7], irq_en[6], led[5:3], drv_halt[1], drv_reset[0]}
//             autostart=1: REG1 (ADDR_LO) write starts the cycle with the fields
//             last written to REG2 (2 register writes per bus write, 1 per read)
//             busy_irq=1: PI_IRQ = irq | (wt command pending); echoed in STATUS[15]
//             so the host can tell the bitstream supports it
// PI_IRQ: irq_en & (ipl != 0 | snoop_avail | rst_seen)  |  busy_irq & (wait_pend | fault)
//         (rst_seen on the line: the emulator stops within a few instructions of
//         the reset button instead of running on a reset machine for up to a
//         host polling period)
`default_nettype none

module pi_if (
    input  wire        clk,
    input  wire        clk16,         // raw bus clock (diagnostics only)
    input  wire        pll_lock,      // diagnostics only

    inout  wire [15:0] pi_ad,
    input  wire [1:0]  pi_reg_a,
    input  wire        pi_wr_n,
    input  wire        pi_rd_n,
    output wire        pi_irq,

    output reg         cyc_start,
    output reg  [23:1] cyc_addr,
    output reg         cyc_a0,
    output reg  [15:0] cyc_wdata,
    output reg         cyc_rw,
    output reg         cyc_word,
    output reg  [2:0]  cyc_fc,
    output reg         cyc_pf,

    input  wire [15:0] rdata,
    input  wire        busy,
    input  wire        pf_active,
    input  wire [3:0]  pf_count,
    input  wire [15:0] pf_head,
    output reg         pf_pop,
    input  wire        berr_flag,
    input  wire        timeout_flag,
    input  wire        vpa_flag,
    input  wire        snoop_ovf,
    input  wire        cmd_full,
    input  wire        cmd_ovf,
    input  wire [2:0]  ipl_level,
    input  wire        reset_in,
    input  wire        halt_in,

    input  wire        snoop_avail,
    input  wire [47:0] snoop_rec,
    output reg         snoop_pop,

    output reg         snoop_rst,     // pulse: reset snoop stream + ovf
    output reg         flags_clr,     // pulse: clear sticky fault/cmd_ovf
    output reg  [2:0]  led,
    output reg         drv_reset,
    output reg         drv_halt,
    output reg         bus_slow,      // REG3 bit12: 5-tick bus cycles (see bus_engine)
    output reg  [1:0]  wr_setup       // REG3 bit15:14: extra ticks of write-data setup before AS, 0-3 (see bus_engine)
);

    // ---- strobe synchronizers ----
    reg [2:0] wr_s, rd_s;
    always @(posedge clk) begin
        wr_s <= {wr_s[1:0], pi_wr_n};
        rd_s <= {rd_s[1:0], pi_rd_n};
    end
    // power-up arming: the Pi's GPIOs default to pull-downs, so both
    // strobes sit LOW until Linux/bare-metal configures them.  Ignore all
    // strobe edges until both lines have been seen HIGH for 16 consecutive
    // clocks, so a configured FPGA never executes a ghost command while
    // the Pi is still booting (independent of the host's init order).
    reg        armed;
    reg [3:0]  arm_cnt;
    always @(posedge clk) begin
        if (!armed) begin
            if (wr_s[1] && rd_s[1]) begin
                arm_cnt <= arm_cnt + 4'd1;
                if (arm_cnt == 4'hF)
                    armed <= 1'b1;
            end else
                arm_cnt <= 4'd0;
        end
    end

    wire wr_rise = armed && (wr_s[2:1] == 2'b01);   // end of write pulse: AD/A stable
    wire rd_rise = armed && (rd_s[2:1] == 2'b01);   // end of read pulse: mux must stay
                                           // stable for the whole strobe

    // AD and reg_a are sampled from the first raw-low samples of a strobe
    // (the Pi sets them up before asserting it) and then FROZEN until the
    // synchronizer has seen the strobe rise and the *_rise event has been
    // consumed.  Sampling for the whole raw-low time (old scheme) let the
    // NEXT access, started by the Pi within ~50ns of the previous strobe's
    // rise, overwrite reg_a before the late rise event looked at it: a
    // STATUS read followed by a REG1 read was then counted as two REG1
    // reads and the snoop hi/lo/data phase slipped for every record after
    // (found on hardware as garbage DMA records / shadow corruption).
    // Separate copies per strobe so a write followed by a read cannot
    // interfere either.
    // The read DATA is frozen the same way (rd_hold): the mux sources are
    // registered, but pf_count and the FIFO head change whenever a fetch
    // completes or a pop lands, i.e. asynchronously to the Pi's sample
    // point ~150ns into the strobe.  With the live mux the Pi caught the
    // saturated count in the middle of 1->2 ({0,1}->{1,0}) as 3, popped
    // three words and got rdata from the empty FIFO: one stale word
    // inserted, the rest of the stream shifted (seen as doubled/shifted
    // rows in the screen viewer, ~once per 32K-word block read).  A DATA
    // read that started on an empty FIFO also must not pop the word that
    // arrives mid-strobe (rd_pf).  The live mux is only visible for the
    // first ~3 clocks (~50ns) of the strobe; see rd_out for what is frozen.
    // A hold read (busy_irq mode: RD# taken low on DATA right after a
    // waited command is issued, the word sampled when PI_IRQ drops) must
    // NOT freeze a FIFO word: the FIFO still holds the words of the
    // PREVIOUS stream until the engine picks the new command up (posted
    // writes may be queued ahead of it), so pf_count != 0 at the strobe
    // start means "stale", not "ours".  Freezing that head handed it out
    // as the read's data - one word of an earlier sequential read (256-colour
    // GVRAM pixels, text rows the IOCS scrolled) in place of the byte the
    // CPU asked for: the console's function-key row garbage and the red
    // lines in Dracula's title (docs 30/32) were exactly this, not the
    // CRTC.  While a waited command is pending the DATA output is the live
    // rdata, and the strobe's end pops nothing.
    reg [15:0] ad_in;
    reg [1:0]  ra_wr, ra_rd;
    reg [15:0] rd_mux;  // live read mux (below)
    reg [15:0] rd_hold;
    reg        rd_pf;   // the frozen DATA came from the prefetch FIFO: pop it at the end
    reg        wait_pend;   // a wt command has started and the engine has not gone idle since
    reg        hold_live;   // this DATA strobe is a hold read (a waited command was pending
                            // at some point during it): output the live rdata, pop nothing.
                            // Set during the strobe rather than sampled at its start: the
                            // host's RD# can reach us before the command strobe that
                            // precedes it has been synchronised and pushed (~80 ns).
    always @(posedge clk) begin
        if (!pi_wr_n && wr_s[1]) begin ad_in <= pi_ad; ra_wr <= pi_reg_a; end
        if (!pi_rd_n && rd_s[1]) begin
            ra_rd <= pi_reg_a; rd_avail <= snoop_avail;
            rd_hold <= rd_mux; rd_pf <= (pf_count != 4'd0);
        end
        if (rd_s[1])         hold_live <= 1'b0;   // no strobe in progress (as synchronised)
        else if (wait_pend)  hold_live <= 1'b1;
    end
    wire rd_pf_eff = rd_pf && !hold_live;   // a FIFO word only counts when this is not a hold read

    // ---- clock liveness heartbeats (~15Hz toggles, diagnostics) ----
    reg [19:0] hb16;
    reg [21:0] hbf;
    always @(posedge clk16) hb16 <= hb16 + 20'd1;
    always @(posedge clk)   hbf  <= hbf  + 22'd1;

    // ---- registers ----
    reg irq_en;
    reg autostart;      // REG3 bit8: REG1 write starts the cycle (REG2 only sets fields)
    reg busy_irq;       // REG3 bit9: PI_IRQ also reports a pending wt command
    reg hello;          // REG3 bit10: host-set marker, cleared only by reconfiguration
    reg rst_seen;       // sticky external reset (see the register map)
    reg rst_clr;        // pulse from REG3 bit11
    reg [3:0] rst_cnt;
    reg cyc_wait;       // CTRL bit11 (wt): the Pi waits for this command
    reg snoop2;         // REG3 bit13: snoop stream v2 (valid-gated HI, continuation records)
    reg rd_avail;       // snoop_avail as sampled at the start of the current read strobe
    reg cyc_ai;         // CTRL bit15 (ai): REG0 write starts a write cycle, address auto-advances
    wire ai_wr = cyc_ai && !cyc_rw;
    reg busy_d;
    reg [1:0] snoop_phase;

    initial begin
        cyc_start = 0; snoop_pop = 0; snoop_rst = 0; flags_clr = 0; pf_pop = 0; cyc_pf = 0;
        cyc_wait = 0; busy_irq = 0; wait_pend = 0; busy_d = 0; hello = 0; cyc_ai = 0; bus_slow = 0;
        rst_seen = 0; rst_clr = 0; rst_cnt = 0; snoop2 = 0; rd_avail = 0; rd_hold = 0; rd_pf = 0; wr_setup = 0; hold_live = 0;
        snoop_phase = 0; irq_en = 0; led = 0; drv_reset = 0; drv_halt = 0; autostart = 0;
        wr_s = 3'b111; rd_s = 3'b111;
        armed = 0; arm_cnt = 0; hb16 = 0; hbf = 0;
    end   // 0=HI 1=LO 2=DATA

    // record layout: {cont, rw_last, uds, lds, addr[23:1], fc[2:0], n[1:0], data[15:0]}
    // snoop2: bit13 carries cont instead of rw, and the word is 0 while no
    // record was available at the start of the strobe (a record arriving
    // mid-strobe is left for the next read; rd_avail keeps the mux stable)
    wire [15:0] snoop_hi   = snoop2 ? (rd_avail ? {snoop_rec[45], snoop_rec[44], snoop_rec[47], snoop_rec[20:16], snoop_rec[43:36]} : 16'h0000)
                                    : {snoop_rec[45], snoop_rec[44], snoop_rec[46], snoop_rec[20:16], snoop_rec[43:36]}; // {uds,lds,rw,fc,n,A23:16}
    wire [15:0] snoop_lo   = {snoop_rec[35:21], 1'b0};                               // A15:1, 0
    wire [15:0] snoop_data = snoop_rec[15:0];

    always @(posedge clk) begin
        cyc_start <= 1'b0;
        snoop_pop <= 1'b0;
        snoop_rst <= 1'b0;
        flags_clr <= 1'b0;
        pf_pop    <= 1'b0;
        rst_clr   <= 1'b0;

        // ai write: post-advance the address one clock after the start, i.e.
        // after the command FIFO has sampled it.  Placed before the strobe
        // handling so that a REG1/REG2 write in the same clock wins.
        if (cyc_start && ai_wr)
            {cyc_addr, cyc_a0} <= {cyc_addr, cyc_a0} +
                                  (cyc_pf ? (cyc_word ? 24'hFFFFFE : 24'hFFFFFF)
                                          : (cyc_word ? 24'h000002 : 24'h000001));

        if (wr_rise) begin
            case (ra_wr)
                2'd0: begin
                    cyc_wdata <= ad_in;
                    if (ai_wr) cyc_start <= 1'b1;       // address/fields as they stand
                end
                2'd1: begin
                    cyc_addr[15:1] <= ad_in[15:1];
                    cyc_a0         <= ad_in[0];
                    if (autostart && !ai_wr) cyc_start <= 1'b1;   // fields from the last REG2
                end
                2'd2: begin
                    cyc_addr[23:16] <= ad_in[7:0];
                    cyc_rw          <= ad_in[8];
                    cyc_word        <= ad_in[9];
                    cyc_pf          <= ad_in[10];
                    cyc_wait        <= ad_in[11];
                    cyc_fc          <= ad_in[14:12];
                    cyc_ai          <= ad_in[15];
                    if (!autostart) cyc_start <= 1'b1;
                end
                2'd3: begin
                    drv_reset <= ad_in[0];
                    drv_halt  <= ad_in[1];
                    flags_clr <= ad_in[2];
                    led       <= ad_in[5:3];
                    irq_en    <= ad_in[6];
                    autostart <= ad_in[8];
                    busy_irq  <= ad_in[9];
                    hello     <= ad_in[10];
                    rst_clr   <= ad_in[11];
                    bus_slow  <= ad_in[12];
                    snoop2    <= ad_in[13];
                    wr_setup  <= ad_in[15:14];
                    if (ad_in[7]) begin
                        snoop_phase <= 2'd0;
                        snoop_rst   <= 1'b1;
                    end
                end
            endcase
        end

        // external reset latch: 16 consecutive clocks of RESET_IN & HALT_IN
        // (~250 ns; a real 68000 needs 10 bus clocks) while we do not drive
        // RESET ourselves.  The clear pulse loses against a still-asserted
        // reset, so the host clears it once the level has gone.
        if (reset_in && halt_in && !drv_reset) begin
            if (rst_cnt == 4'hF) rst_seen <= 1'b1;
            else                 rst_cnt  <= rst_cnt + 4'd1;
        end else
            rst_cnt <= 4'd0;
        if (rst_clr && !(reset_in && halt_in && !drv_reset))
            rst_seen <= 1'b0;

        // wt command tracking for PI_IRQ.  busy drops for one clock right
        // after cyc_start (the command FIFO's rdy is registered), hence the
        // two-sample clear.  The waited command is always the last one
        // queued, so "engine idle" means it has completed.
        busy_d <= busy;
        if (cyc_start && cyc_wait)
            wait_pend <= 1'b1;
        else if (!busy && !busy_d)
            wait_pend <= 1'b0;

        // prefetch FIFO: pop at the END of each DATA read strobe, only when
        // the word handed out came from the FIFO (rd_pf; a word arriving
        // mid-strobe stays for the next read)
        if (rd_rise && ra_rd == 2'd0 && rd_pf_eff)
            pf_pop <= 1'b1;

        // snoop stream: advance phase at the END of each REG1 read strobe.
        // A stray REG1 read while the FIFO is empty must not leave the
        // phase rotated for the next record: re-align whenever it is empty.
        // snoop2: a HI read that returned 0 (no record at strobe start) does
        // not advance, and a continuation record skips the LO phase.
        if (!snoop_avail)
            snoop_phase <= 2'd0;
        else if (rd_rise && ra_rd == 2'd1 && (rd_avail || !snoop2)) begin
            if (snoop_phase == 2'd2) begin
                snoop_phase <= 2'd0;
                snoop_pop   <= 1'b1;
            end else if (snoop_phase == 2'd0 && snoop2 && snoop_rec[47])
                snoop_phase <= 2'd2;
            else
                snoop_phase <= snoop_phase + 2'd1;
        end
    end

    // ---- read mux (async output enable, registered sources; frozen into
    // rd_hold for the rest of the strobe once the synchroniser sees RD#) ----
    always @* begin
        case (pi_reg_a)
            2'd0: rd_mux = (pf_count != 4'd0) ? pf_head : rdata;
            2'd1: rd_mux = (snoop_phase == 2'd0) ? snoop_hi :
                           (snoop_phase == 2'd1) ? snoop_lo : snoop_data;
            // low byte = diagnostics (host code only checks 0x56, bit0, bit1)
            2'd2: rd_mux = {8'h56, pll_lock, hbf[21], hb16[19], armed,
                            snoop2, 1'b1 /* ai_ok */, rst_seen, hello};
            // pf_count saturated to 3: {cnt>=2, cnt==1 || cnt>=3}
            2'd3: rd_mux = {busy_irq, pf_active, pf_count[3] | pf_count[2] | pf_count[1],
                            pf_count[3] | pf_count[2] | pf_count[0],
                            cmd_ovf, cmd_full, snoop_ovf, vpa_flag,
                            halt_in, reset_in, ipl_level,
                            snoop_avail, (berr_flag | timeout_flag), busy};
        endcase
    end

    // Frozen for the rest of the strobe: STATUS, and DATA when it came from
    // the FIFO.  A plain DATA read stays live: the hold read (busy_irq mode)
    // keeps RD# low while the cycle completes and samples rdata as it lands.
    wire [15:0] rd_out = rd_s[1]        ? rd_mux :
                         (ra_rd == 2'd0) ? (rd_pf_eff ? rd_hold : rdata) :
                         (ra_rd == 2'd3) ? rd_hold : rd_mux;
    assign pi_ad  = (!pi_rd_n) ? rd_out : 16'bz;
    // busy_irq: the sticky fault also holds the line, so a host that only
    // watches the line for completion is forced to STATUS (where the fault
    // is reported) until it clears the flag
    assign pi_irq = (irq_en & ((ipl_level != 3'd0) | snoop_avail | rst_seen)) |
                    (busy_irq & (wait_pend | berr_flag | timeout_flag));

endmodule
