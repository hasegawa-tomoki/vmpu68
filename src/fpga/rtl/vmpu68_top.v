// SPDX-License-Identifier: MIT
//
// vmpu68 — iCE40HX4K bus interface between Raspberry Pi (68k emulator)
// and X68000 XVI 68000 socket.  Phase 2.
//
// Clocking: the 16MHz bus clock feeds an SB_PLL40_CORE producing a 64MHz
// internal clock.  Everything runs in that single domain; 68000-bus state
// machines advance on tick16 (clk16 rising edge detected in the fast
// domain), so bus timing is identical to a 16MHz design while the Pi
// interface samples 4x faster.  The bus outputs themselves are re-timed by
// the bus clock in the IO registers (docs 57), so their position on the bus
// does not depend on the PLL phase.  Buffer output enables are gated by PLL
// lock so the 5V bus stays isolated until the clock is stable.
`default_nettype none

module vmpu68_top #(parameter SNOOP_AW = 10) (
    input  wire        clk16,

    // ---- 68000 bus, FPGA side of buffers ----
    inout  wire [23:1] la,
    inout  wire [15:0] ld,
    inout  wire        l_as_n,
    inout  wire        l_uds_n,
    inout  wire        l_lds_n,
    inout  wire        l_rw,
    inout  wire [2:0]  lfc,
    inout  wire        l_vma_n,
    input  wire        l_dtack_n,
    input  wire        l_bgack_n,
    input  wire        l_br_n,
    input  wire        l_halt_n,
    input  wire        l_reset_n,
    input  wire        l_vpa_n,
    input  wire        l_berr_n,
    input  wire [2:0]  l_ipl_n,
    output wire        l_bg_n,
    output wire        l_e,
    output wire        rst_drv,
    output wire        hlt_drv,

    // ---- buffer direction / enable ----
    output wire        buf_d_dir,
    output wire        buf_d_oe_n,
    output wire        buf_a_dir,
    output wire        buf_a_oe_n,
    output wire        buf_o_oe_n,

    // ---- Raspberry Pi interface ----
    inout  wire [15:0] pi_ad,
    input  wire [1:0]  pi_reg_a,
    input  wire        pi_wr_n,
    input  wire        pi_rd_n,
    output wire        pi_irq,

    // ---- misc ----
    output wire        led_r,
    output wire        led_g,
    output wire        led_b,
    // core 2.1 pins (unconnected on 1.x/2.0/2.0.1): the DTACK driver Q4 (EXDTACK) must be held
    // OFF and the PSRAM chips deselected.  Left undriven, Q4's base floats and the bus DTACK
    // comes early at random - main RAM writes lost 0.3-0.7% of the words on a 2.1 board with
    // the EXDTACK wire in place, varying with time/temperature (docs 56.8, 56.10)
    output wire        dtack_drv,
    output wire [1:0]  psram_ce_n
);

    // ------------------------------------------------------------------
    // PLL: clk16 x 61/16 -> ~61MHz @16.0MHz / ~63.6MHz @16.67MHz (XVI).
    // The design is a single clock domain that only samples clk16, so the
    // ratio need not be an integer.  DIVF=60 keeps the VCO inside 533-1066MHz
    // for any bus clock from 10MHz (X68000 10MHz mode: 610MHz) to 16.67MHz
    // (XVI: 1017MHz); DIVF=63 would exceed the limit at 16.67MHz.
    // NOTE: nextpnr places this PLL at the top site (X16/Y33); its supply is
    // VCCPLL1 = TQ144 pin 126, which core 1.0 left unconnected (wrong
    // library symbol) -> reworked with a jumper to pin 111 (VCC).
    // ------------------------------------------------------------------
    // The bus clock pad is GBIN3 (TQ144 pin 93): besides feeding the PLL it
    // drives a global net that clocks the bus output registers (docs 57).
    wire clk16_g, clk16_i;
    SB_GB_IO #(.PIN_TYPE(6'b000001)) u_clk16 (
        .PACKAGE_PIN(clk16), .GLOBAL_BUFFER_OUTPUT(clk16_g), .D_IN_0(clk16_i),
        .LATCH_INPUT_VALUE(1'b0), .CLOCK_ENABLE(1'b1), .INPUT_CLK(1'b0), .OUTPUT_CLK(1'b0),
        .OUTPUT_ENABLE(1'b0), .D_OUT_0(1'b0), .D_OUT_1(1'b0)
    );
    wire clk, pll_lock;
    SB_PLL40_CORE #(
        .FEEDBACK_PATH("SIMPLE"),
        .DIVR(4'd0), .DIVF(7'd60), .DIVQ(3'd4), .FILTER_RANGE(3'd1)
    ) u_pll (
        .REFERENCECLK(clk16_i),
        .PLLOUTGLOBAL(clk),
        .LOCK(pll_lock),
        .RESETB(1'b1),
        .BYPASS(1'b0)
    );

    // clk16 edge detect in the fast domain.
    // The bus FSM must move its outputs shortly after the clk16 rising
    // edge like a 68000 does (AS within ~55ns of the S2 edge): the X68000
    // memory controller is synchronous to this clock.  A rising-edge
    // detector built from posedge samples only (3 stages) places the
    // outputs 95-120ns after the edge at 10MHz - i.e. straddling the NEXT
    // edge, where the controller sees them one clock late at random.
    // Detecting the FALLING edge instead, with the first sample taken on
    // the negative clk edge, puts the FSM update 39-65ns after the falling
    // edge = 4-30ns after the following rising edge.
    reg       c16n;
    always @(negedge clk) c16n <= clk16_g;   // the same net that clocks the output registers
    reg [1:0] c16p;
    always @(posedge clk) c16p <= {c16p[0], c16n};
    wire tick16 = (c16p == 2'b10);
    // the rising edge, detected the same way: 39-65ns after it = the
    // falling-edge phase (4-30ns after the falling edge at 10MHz)
    wire tick16h = (c16p == 2'b01);

    // ------------------------------------------------------------------
    // Pi register interface
    // ------------------------------------------------------------------
    wire        cyc_start;
    wire [23:1] cyc_addr;
    wire [15:0] cyc_wdata;
    wire        cyc_rw, cyc_word, cyc_a0, cyc_pf;
    wire [2:0]  cyc_fc;
    wire        pf_active, pf_pop;
    wire [3:0]  pf_count;
    wire [15:0] pf_head;
    wire [15:0] rdata;
    wire        busy, berr_flag, timeout_flag, vpa_flag;
    wire        cmd_full, cmd_ovf, snoop_ovf;
    wire        flags_clr, snoop_rst;
    wire [2:0]  ipl_level;
    wire        reset_in, halt_in;
    wire        snoop_avail, snoop_pop;
    wire [47:0] snoop_rec;
    wire [2:0]  led;
    wire        drv_reset, drv_halt;
    wire        bus_slow;
    wire [1:0]  wr_setup;

    pi_if u_pi (
        .clk(clk), .clk16(clk16_i), .pll_lock(pll_lock), .pi_ad(pi_ad), .pi_reg_a(pi_reg_a),
        .pi_wr_n(pi_wr_n), .pi_rd_n(pi_rd_n), .pi_irq(pi_irq),
        .cyc_start(cyc_start), .cyc_addr(cyc_addr), .cyc_a0(cyc_a0),
        .cyc_wdata(cyc_wdata), .cyc_rw(cyc_rw), .cyc_word(cyc_word), .cyc_fc(cyc_fc),
        .cyc_pf(cyc_pf),
        .rdata(rdata), .busy(busy),
        .pf_active(pf_active), .pf_count(pf_count), .pf_head(pf_head), .pf_pop(pf_pop),
        .berr_flag(berr_flag), .timeout_flag(timeout_flag), .vpa_flag(vpa_flag),
        .snoop_ovf(snoop_ovf), .cmd_full(cmd_full), .cmd_ovf(cmd_ovf),
        .ipl_level(ipl_level), .reset_in(reset_in), .halt_in(halt_in),
        .snoop_avail(snoop_avail), .snoop_rec(snoop_rec), .snoop_pop(snoop_pop),
        .snoop_rst(snoop_rst), .flags_clr(flags_clr),
        .led(led), .drv_reset(drv_reset), .drv_halt(drv_halt), .bus_slow(bus_slow), .wr_setup(wr_setup)
    );

    // ------------------------------------------------------------------
    // E clock + VMA
    // ------------------------------------------------------------------
    wire e_clk, vma_req, vma_n_int, vma_done;
    e_clock u_e (.clk(clk), .tick16(tick16), .e(e_clk),
                 .vma_req(vma_req), .vma_n(vma_n_int), .vma_done(vma_done));

    // ------------------------------------------------------------------
    // 68000 bus master engine
    // ------------------------------------------------------------------
    wire        own;
    wire [23:1] la_o;
    wire        as_n_r, as_n_f, uds_n_r, uds_n_f, lds_n_r, lds_n_f, rw_o;
    wire [2:0]  fc_o;
    wire [15:0] ld_o;
    wire        ld_drive;
    wire        i_buf_d_dir, i_buf_d_oe_n, i_buf_a_dir, i_buf_a_oe_n;
    wire [23:1] la_in;                 // pad taps of the bus pins (SB_IO D_IN_0, below)
    wire [15:0] ld_in;
    wire [2:0]  fc_in;
    wire        as_in, uds_in, lds_in, rw_in;

    bus_engine #(.SNOOP_AW(SNOOP_AW)) u_bus (
        .clk(clk), .tick16(tick16), .tick16h(tick16h), .slow(bus_slow), .wr_setup(wr_setup),
        .start(cyc_start), .addr(cyc_addr), .a0(cyc_a0), .wdata(cyc_wdata),
        .rw(cyc_rw), .word(cyc_word), .fc(cyc_fc), .pf(cyc_pf),
        .rdata(rdata), .busy(busy),
        .pf_active(pf_active), .pf_count(pf_count), .pf_head(pf_head), .pf_pop(pf_pop),
        .berr_flag(berr_flag), .timeout_flag(timeout_flag), .vpa_flag(vpa_flag),
        .cmd_full(cmd_full), .cmd_ovf(cmd_ovf), .flags_clr(flags_clr),

        .own(own),
        .la_o(la_o), .as_n_r(as_n_r), .as_n_f(as_n_f), .uds_n_r(uds_n_r), .uds_n_f(uds_n_f), .lds_n_r(lds_n_r), .lds_n_f(lds_n_f),
        .rw_o(rw_o), .fc_o(fc_o),
        .la_i(la_in), .fc_i(fc_in), .as_n_i(as_in), .uds_n_i(uds_in), .lds_n_i(lds_in), .rw_i(rw_in),
        .ld_o(ld_o), .ld_drive(ld_drive), .ld_i(ld_in),

        .dtack_n(l_dtack_n), .berr_n(l_berr_n), .vpa_n(l_vpa_n),
        .br_n(l_br_n), .bgack_n(l_bgack_n), .bg_n(l_bg_n),

        .vma_req(vma_req), .vma_done(vma_done),

        .buf_d_dir(i_buf_d_dir), .buf_d_oe_n(i_buf_d_oe_n),
        .buf_a_dir(i_buf_a_dir), .buf_a_oe_n(i_buf_a_oe_n),

        .snoop_avail(snoop_avail), .snoop_rec(snoop_rec), .snoop_pop(snoop_pop),
        .snoop_ovf(snoop_ovf), .snoop_rst(snoop_rst)
    );

    // ---- bus pins: output registers clocked by the bus clock (docs 57) ----
    // Address/FC/R/W/data change after the falling edge; AS/UDS/LDS are DDR
    // outputs (asserted after a rising edge, released after a falling edge);
    // the enables (own, ld_drive) are the engine's, unregistered.
    // The input taps (D_IN_0) feed the snoop logic and the read data path.
    localparam [5:0] OREG = 6'b100101;     // registered output, enable from the fabric, plain input
    genvar gi;
    generate
        for (gi = 1; gi <= 23; gi = gi + 1) begin : g_la
            SB_IO #(.PIN_TYPE(OREG), .NEG_TRIGGER(1'b1)) u (
                .PACKAGE_PIN(la[gi]), .OUTPUT_CLK(clk16_g), .OUTPUT_ENABLE(own),
                .D_OUT_0(la_o[gi]), .D_IN_0(la_in[gi]),
                .LATCH_INPUT_VALUE(1'b0), .CLOCK_ENABLE(1'b1), .INPUT_CLK(1'b0), .D_OUT_1(1'b0));
        end
        for (gi = 0; gi < 16; gi = gi + 1) begin : g_ld
            SB_IO #(.PIN_TYPE(OREG), .NEG_TRIGGER(1'b1)) u (
                .PACKAGE_PIN(ld[gi]), .OUTPUT_CLK(clk16_g), .OUTPUT_ENABLE(ld_drive),
                .D_OUT_0(ld_o[gi]), .D_IN_0(ld_in[gi]),
                .LATCH_INPUT_VALUE(1'b0), .CLOCK_ENABLE(1'b1), .INPUT_CLK(1'b0), .D_OUT_1(1'b0));
        end
        for (gi = 0; gi < 3; gi = gi + 1) begin : g_fc
            SB_IO #(.PIN_TYPE(OREG), .NEG_TRIGGER(1'b1)) u (
                .PACKAGE_PIN(lfc[gi]), .OUTPUT_CLK(clk16_g), .OUTPUT_ENABLE(own),
                .D_OUT_0(fc_o[gi]), .D_IN_0(fc_in[gi]),
                .LATCH_INPUT_VALUE(1'b0), .CLOCK_ENABLE(1'b1), .INPUT_CLK(1'b0), .D_OUT_1(1'b0));
        end
    endgenerate
    SB_IO #(.PIN_TYPE(OREG), .NEG_TRIGGER(1'b1)) u_rw (
        .PACKAGE_PIN(l_rw), .OUTPUT_CLK(clk16_g), .OUTPUT_ENABLE(own), .D_OUT_0(rw_o), .D_IN_0(rw_in),
        .LATCH_INPUT_VALUE(1'b0), .CLOCK_ENABLE(1'b1), .INPUT_CLK(1'b0), .D_OUT_1(1'b0));
    // the strobes: DDR output, D_OUT_0 shows after the rising edge, D_OUT_1 after the falling edge
    localparam [5:0] ODDR = 6'b100001;
    SB_IO #(.PIN_TYPE(ODDR), .NEG_TRIGGER(1'b0)) u_as (
        .PACKAGE_PIN(l_as_n), .OUTPUT_CLK(clk16_g), .OUTPUT_ENABLE(own), .D_OUT_0(as_n_r), .D_OUT_1(as_n_f), .D_IN_0(as_in),
        .LATCH_INPUT_VALUE(1'b0), .CLOCK_ENABLE(1'b1), .INPUT_CLK(1'b0));
    SB_IO #(.PIN_TYPE(ODDR), .NEG_TRIGGER(1'b0)) u_uds (
        .PACKAGE_PIN(l_uds_n), .OUTPUT_CLK(clk16_g), .OUTPUT_ENABLE(own), .D_OUT_0(uds_n_r), .D_OUT_1(uds_n_f), .D_IN_0(uds_in),
        .LATCH_INPUT_VALUE(1'b0), .CLOCK_ENABLE(1'b1), .INPUT_CLK(1'b0));
    SB_IO #(.PIN_TYPE(ODDR), .NEG_TRIGGER(1'b0)) u_lds (
        .PACKAGE_PIN(l_lds_n), .OUTPUT_CLK(clk16_g), .OUTPUT_ENABLE(own), .D_OUT_0(lds_n_r), .D_OUT_1(lds_n_f), .D_IN_0(lds_in),
        .LATCH_INPUT_VALUE(1'b0), .CLOCK_ENABLE(1'b1), .INPUT_CLK(1'b0));
    assign l_vma_n = own ? vma_n_int : 1'bz;   // E-clock engine, fast domain

    assign l_e        = e_clk;

    // hold the 5V bus isolated until the PLL is locked
    assign buf_d_dir  = i_buf_d_dir;
    assign buf_d_oe_n = pll_lock ? i_buf_d_oe_n : 1'b1;
    assign buf_a_dir  = i_buf_a_dir;
    assign buf_a_oe_n = pll_lock ? i_buf_a_oe_n : 1'b1;
    assign buf_o_oe_n = pll_lock ? 1'b0 : 1'b1;

    assign rst_drv = drv_reset;
    assign hlt_drv = drv_halt;

    // IPL: the three lines do not change simultaneously (e.g. 6 -> 5 passes
    // through 7 = NMI); like a real 68000 only accept a level that has been
    // stable for a while (8 clocks ~ 130ns)
    wire [2:0] ipl_raw;
    sync2 #(.W(3)) s_ipl (.clk(clk), .d(~l_ipl_n), .q(ipl_raw));
    reg  [2:0] ipl_prev = 3'd0, ipl_stable = 3'd0;
    reg  [2:0] ipl_cnt  = 3'd0;
    always @(posedge clk) begin
        ipl_prev <= ipl_raw;
        if (ipl_raw != ipl_prev)
            ipl_cnt <= 3'd0;
        else if (ipl_cnt != 3'd7)
            ipl_cnt <= ipl_cnt + 3'd1;
        else
            ipl_stable <= ipl_raw;
    end
    assign ipl_level = ipl_stable;
    sync2 s_rst (.clk(clk), .d(~l_reset_n), .q(reset_in));
    sync2 s_hlt (.clk(clk), .d(~l_halt_n),  .q(halt_in));

    // LED1 is a common-anode RGB LED whose anode sits on +5V (board 1.0):
    // a pin driven high (3.3V) still leaves 1.7V across the 1k + red LED,
    // enough for a faint red glow.  So an "off" colour floats its pin
    // (no pull-up, see the pcf) and only an "on" colour drives it low.
    assign led_r = led[2] ? 1'b0 : 1'bz;
    assign led_g = led[1] ? 1'b0 : 1'bz;
    assign led_b = led[0] ? 1'b0 : 1'bz;

    assign dtack_drv  = 1'b0;      // Q4 off (core 2.1); no-op elsewhere
    assign psram_ce_n = 2'b11;     // PSRAM deselected (core 2.1); no-op elsewhere

endmodule

module sync2 #(parameter W=1) (
    input wire clk,
    input wire [W-1:0] d,
    output reg [W-1:0] q
);
    reg [W-1:0] m;
    always @(posedge clk) begin
        m <= d;
        q <= m;
    end
endmodule
