// SPDX-License-Identifier: MIT
//
// vmpu68 — iCE40HX4K bus interface between Raspberry Pi (68k emulator)
// and X68000 XVI 68000 socket.  Phase 2.
//
// Clocking: the 16MHz bus clock feeds an SB_PLL40_CORE producing a 64MHz
// internal clock.  Everything runs in that single domain; 68000-bus state
// machines advance on tick16 (clk16 rising edge detected in the fast
// domain), so bus timing is identical to a 16MHz design while the Pi
// interface samples 4x faster.  Buffer output enables are gated by PLL
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
    output wire        led_b
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
    wire clk, pll_lock;
    SB_PLL40_CORE #(
        .FEEDBACK_PATH("SIMPLE"),
        .DIVR(4'd0), .DIVF(7'd60), .DIVQ(3'd4), .FILTER_RANGE(3'd1)
    ) u_pll (
        .REFERENCECLK(clk16),
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
    always @(negedge clk) c16n <= clk16;
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
        .clk(clk), .clk16(clk16), .pll_lock(pll_lock), .pi_ad(pi_ad), .pi_reg_a(pi_reg_a),
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
    wire        as_n_o, uds_n_o, lds_n_o, rw_o;
    wire [2:0]  fc_o;
    wire [15:0] ld_o;
    wire        ld_drive;
    wire        i_buf_d_dir, i_buf_d_oe_n, i_buf_a_dir, i_buf_a_oe_n;

    bus_engine #(.SNOOP_AW(SNOOP_AW)) u_bus (
        .clk(clk), .tick16(tick16), .tick16h(tick16h), .slow(bus_slow), .wr_setup(wr_setup),
        .start(cyc_start), .addr(cyc_addr), .a0(cyc_a0), .wdata(cyc_wdata),
        .rw(cyc_rw), .word(cyc_word), .fc(cyc_fc), .pf(cyc_pf),
        .rdata(rdata), .busy(busy),
        .pf_active(pf_active), .pf_count(pf_count), .pf_head(pf_head), .pf_pop(pf_pop),
        .berr_flag(berr_flag), .timeout_flag(timeout_flag), .vpa_flag(vpa_flag),
        .cmd_full(cmd_full), .cmd_ovf(cmd_ovf), .flags_clr(flags_clr),

        .own(own),
        .la_o(la_o), .as_n_o(as_n_o), .uds_n_o(uds_n_o), .lds_n_o(lds_n_o),
        .rw_o(rw_o), .fc_o(fc_o),
        .la_i(la), .fc_i(lfc), .as_n_i(l_as_n), .uds_n_i(l_uds_n), .lds_n_i(l_lds_n), .rw_i(l_rw),
        .ld_o(ld_o), .ld_drive(ld_drive), .ld_i(ld),

        .dtack_n(l_dtack_n), .berr_n(l_berr_n), .vpa_n(l_vpa_n),
        .br_n(l_br_n), .bgack_n(l_bgack_n), .bg_n(l_bg_n),

        .vma_req(vma_req), .vma_done(vma_done),

        .buf_d_dir(i_buf_d_dir), .buf_d_oe_n(i_buf_d_oe_n),
        .buf_a_dir(i_buf_a_dir), .buf_a_oe_n(i_buf_a_oe_n),

        .snoop_avail(snoop_avail), .snoop_rec(snoop_rec), .snoop_pop(snoop_pop),
        .snoop_ovf(snoop_ovf), .snoop_rst(snoop_rst)
    );

    // ---- tri-state pin drivers (buf_a group follows own) ----
    assign la      = own ? la_o    : 23'bz;
    assign l_as_n  = own ? as_n_o  : 1'bz;
    assign l_uds_n = own ? uds_n_o : 1'bz;
    assign l_lds_n = own ? lds_n_o : 1'bz;
    assign l_rw    = own ? rw_o    : 1'bz;
    assign lfc     = own ? fc_o    : 3'bz;
    assign l_vma_n = own ? vma_n_int : 1'bz;
    assign ld      = ld_drive ? ld_o : 16'bz;

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
