// vmpu68 phase-1 testbench.
//
// Structure:
//   vmpu68_top (DUT)
//     |  FPGA-side nets (f_*)
//   74LVC8T245 behavioral transceivers (dir/oe from DUT)
//     |  5V bus nets (b_*)
//   X68000 motherboard model: RAM + DTACK gen, VPA device, BERR region,
//   DMAC bus master, IPL driver
// plus a Raspberry Pi master model on pi_ad/pi_reg_a/pi_wr_n/pi_rd_n.
`timescale 1ns/1ps
`default_nettype none

module tb_top;

    // ---------------- clock ----------------
    reg clk16 = 0;
    always #31.25 clk16 = ~clk16;      // 16 MHz

    // ---------------- FPGA side nets ----------------
    wire [23:1] f_la;
    wire [15:0] f_ld;
    wire f_as_n, f_uds_n, f_lds_n, f_rw;
    wire [2:0] f_fc;
    wire f_vma_n;
    wire f_bg_n, f_e;
    wire rst_drv, hlt_drv;
    wire buf_d_dir, buf_d_oe_n, buf_a_dir, buf_a_oe_n, buf_o_oe_n;
    wire pi_irq;
    wire led_r, led_g, led_b;

    // inputs to DUT from bus (through input-fixed buffers, modeled as wires)
    wire b_dtack_n, b_berr_n, b_vpa_n;
    wire b_br_n, b_bgack_n;
    wire b_reset_n, b_halt_n;
    tri1 b_reset_int, b_halt_int;
    assign b_reset_n = b_reset_int;
    assign b_halt_n  = b_halt_int;
    reg  [2:0] b_ipl_n = 3'b111;

    // ---------------- 5V bus nets ----------------
    wire [23:1] b_la;
    wire [15:0] b_ld;
    wire b_as_n, b_uds_n, b_lds_n, b_rw;
    wire [2:0] b_fc;
    wire b_vma_n;

    // pullups on open/tri-stated control lines (motherboard has them)
    pullup(b_as_n); pullup(b_uds_n); pullup(b_lds_n); pullup(b_rw);
    pullup(b_vma_n);

    // ---------------- Pi interface ----------------
    wire [15:0] pi_ad;
    reg  [15:0] pi_ad_o;
    reg         pi_ad_drv = 0;
    reg  [1:0]  pi_reg_a = 0;
    reg         pi_wr_n = 1, pi_rd_n = 1;
    assign pi_ad = pi_ad_drv ? pi_ad_o : 16'bz;

    // ---------------- DUT ----------------
    // SNOOP_AW=5 (32-deep) so the overflow test stays short; hw default is 9
    vmpu68_top #(.SNOOP_AW(5)) dut (
        .clk16(clk16),
        .la(f_la), .ld(f_ld),
        .l_as_n(f_as_n), .l_uds_n(f_uds_n), .l_lds_n(f_lds_n), .l_rw(f_rw),
        .lfc(f_fc), .l_vma_n(f_vma_n),
        .l_dtack_n(b_dtack_n), .l_bgack_n(b_bgack_n), .l_br_n(b_br_n),
        .l_halt_n(b_halt_n), .l_reset_n(b_reset_n),
        .l_vpa_n(b_vpa_n), .l_berr_n(b_berr_n), .l_ipl_n(b_ipl_n),
        .l_bg_n(f_bg_n), .l_e(f_e),
        .rst_drv(rst_drv), .hlt_drv(hlt_drv),
        .buf_d_dir(buf_d_dir), .buf_d_oe_n(buf_d_oe_n),
        .buf_a_dir(buf_a_dir), .buf_a_oe_n(buf_a_oe_n), .buf_o_oe_n(buf_o_oe_n),
        .pi_ad(pi_ad), .pi_reg_a(pi_reg_a), .pi_wr_n(pi_wr_n), .pi_rd_n(pi_rd_n),
        .pi_irq(pi_irq),
        .led_r(led_r), .led_g(led_g), .led_b(led_b)
    );

    // ---------------- 8T245 transceivers ----------------
    // A = FPGA side, B = bus side. dir=1: A->B.
    xcvr #(.W(23)) u_ba (.a(f_la), .b(b_la), .dir(buf_a_dir), .oe_n(buf_a_oe_n));
    xcvr #(.W(1)) u_bas (.a(f_as_n), .b(b_as_n), .dir(buf_a_dir), .oe_n(buf_a_oe_n));
    xcvr #(.W(1)) u_bu (.a(f_uds_n), .b(b_uds_n), .dir(buf_a_dir), .oe_n(buf_a_oe_n));
    xcvr #(.W(1)) u_bl (.a(f_lds_n), .b(b_lds_n), .dir(buf_a_dir), .oe_n(buf_a_oe_n));
    xcvr #(.W(1)) u_br (.a(f_rw), .b(b_rw), .dir(buf_a_dir), .oe_n(buf_a_oe_n));
    xcvr #(.W(3)) u_bf (.a(f_fc), .b(b_fc), .dir(buf_a_dir), .oe_n(buf_a_oe_n));
    xcvr #(.W(1)) u_bv (.a(f_vma_n), .b(b_vma_n), .dir(buf_a_dir), .oe_n(buf_a_oe_n));
    xcvr #(.W(16)) u_bd (.a(f_ld), .b(b_ld), .dir(buf_d_dir), .oe_n(buf_d_oe_n));

    // ---------------- turnaround assertions ----------------
    // DIR must only change while the buffer group's OE is inactive
    always @(buf_a_dir) if ($time > 2000 && buf_a_oe_n !== 1'b1) begin
        $display("FAIL turnaround: buf_a_dir changed while OE active @%0t", $time);
        errors = errors + 1;
    end
    always @(buf_d_dir) if ($time > 2000 && buf_d_oe_n !== 1'b1) begin
        $display("FAIL turnaround: buf_d_dir changed while OE active @%0t", $time);
        errors = errors + 1;
    end

    // ---------------- RESET/HALT loopback (Q1/Q2 open-drain model) --------
    // rst_drv=1 turns on Q1 which pulls the bus RESET line low; sensed back
    // through the input buffer as l_reset_n.
    assign b_reset_int = rst_drv ? 1'b0 : 1'bz;
    assign b_halt_int  = hlt_drv ? 1'b0 : 1'bz;

    // ---------------- X68000 motherboard model ----------------
    x68k_model mobo (
        .clk(clk16),
        .la(b_la), .ld(b_ld),
        .as_n(b_as_n), .uds_n(b_uds_n), .lds_n(b_lds_n), .rw(b_rw), .fc(b_fc),
        .vma_n(b_vma_n), .e(f_e),
        .dtack_n(b_dtack_n), .berr_n(b_berr_n), .vpa_n(b_vpa_n),
        .br_n(b_br_n), .bg_n(f_bg_n), .bgack_n(b_bgack_n)
    );

    // ---------------- Pi bus tasks ----------------
    integer errors = 0;

    // AS-to-AS spacing monitor (bus cycle length seen by the motherboard)
    reg mon_as = 0;
    time as_fall = 0, as_min = 0, as_max = 0;
    integer as_n_cyc = 0;
    time as_lo_min = 0, as_lo_max = 0;
    always @(negedge b_as_n) if (mon_as) begin
        if (as_fall != 0) begin
            if (as_min == 0 || $time - as_fall < as_min) as_min = $time - as_fall;
            if ($time - as_fall > as_max) as_max = $time - as_fall;
            as_n_cyc = as_n_cyc + 1;
        end
        as_fall = $time;
    end
    always @(posedge b_as_n) if (mon_as && as_fall != 0) begin
        if (as_lo_min == 0 || $time - as_fall < as_lo_min) as_lo_min = $time - as_fall;
        if ($time - as_fall > as_lo_max) as_lo_max = $time - as_fall;
    end

    task pi_write(input [1:0] a, input [15:0] d);
        begin
            pi_reg_a = a; pi_ad_o = d; pi_ad_drv = 1;
            #50 pi_wr_n = 0;
            #250 pi_wr_n = 1;          // >= 4 clk16 low
            #100 pi_ad_drv = 0;
        end
    endtask

    task pi_read(input [1:0] a, output [15:0] d);
        begin
            pi_reg_a = a;
            #50 pi_rd_n = 0;
            #250 d = pi_ad;            // sample near end of strobe
            pi_rd_n = 1;
            #100;
        end
    endtask

    // same as pi_read but with a ~40ns turnaround between two consecutive
    // accesses (2.4 clk here; the software guarantees >= ~100ns on hardware)
    task pi_read_tight(input [1:0] a, output [15:0] d);
        begin
            pi_reg_a = a;
            #20 pi_rd_n = 0;
            #200 d = pi_ad;
            pi_rd_n = 1;
            #20;
        end
    endtask

    reg [15:0] st, rd;

    task bus_cycle(input rw, input word, input [23:0] addr,
                   input [15:0] wd, output [15:0] out);
        begin
            if (!rw) pi_write(2'd0, wd);
            pi_write(2'd1, addr[15:0]);
            pi_write(2'd2, {1'b0, 3'b101, 2'b00, word, rw, addr[23:16]});
            st = 16'h0001;
            while (st[0]) pi_read(2'd3, st);
            if (rw) pi_read(2'd0, out); else out = 16'h0;
        end
    endtask

    task bus_cycle_fc(input [2:0] fc, input rw, input word, input [23:0] addr,
                      input [15:0] wd, output [15:0] out);
        begin
            if (!rw) pi_write(2'd0, wd);
            pi_write(2'd1, addr[15:0]);
            pi_write(2'd2, {1'b0, fc, 2'b00, word, rw, addr[23:16]});
            st = 16'h0001;
            while (st[0]) pi_read(2'd3, st);
            if (rw) pi_read(2'd0, out); else out = 16'h0;
        end
    endtask

    task post_write(input [23:0] addr, input [15:0] wd);   // fire-and-forget
        begin
            pi_write(2'd0, wd);
            pi_write(2'd1, addr[15:0]);
            pi_write(2'd2, {1'b0, 3'b101, 2'b00, 1'b1, 1'b0, addr[23:16]});
        end
    endtask

    // prefetch-stream read: CTRL bit10 set, poll STATUS until the FIFO
    // holds a word (or the stream died), then pop it from DATA
    task pf_read(input [23:0] addr, output [15:0] out);
        begin
            pi_write(2'd1, addr[15:0]);
            pi_write(2'd2, {1'b0, 3'b101, 1'b0, 1'b1, 1'b1, 1'b1, addr[23:16]});
            st = 16'h0001;
            while (st[13:12] == 2'b00 && (st[0] || st[14])) pi_read(2'd3, st);
            if (st[13:12] != 2'b00) pi_read(2'd0, out); else out = 16'hDEAD;
        end
    endtask

    // next word of the stream: STATUS then DATA (returns DEAD when the
    // stream is over and nothing is buffered)
    task pf_next(output [15:0] out);
        begin
            st = 16'h4000;
            while (st[13:12] == 2'b00 && st[14]) pi_read(2'd3, st);
            if (st[13:12] != 2'b00) pi_read(2'd0, out); else out = 16'hDEAD;
        end
    endtask

    task check(input [127:0] name, input [15:0] got, input [15:0] exp);
        if (got !== exp) begin
            $display("FAIL %0s: got %04x expected %04x", name, got, exp);
            errors = errors + 1;
        end else
            $display("pass %0s: %04x", name, got);
    endtask

    // ---------------- test sequence ----------------
    reg [15:0] hi, lo, da;
    integer i;
    initial begin
        $dumpfile("tb_top.vcd");
        $dumpvars(0, tb_top);
        #1500;                          // PLL lock

        // 0. signature
        pi_read(2'd2, rd);
        check("signature", rd & 16'hFF00, 16'h5600);
        check("hello clear after config", rd & 16'h0001, 16'h0000);
        pi_write(2'd3, 16'h0400);                      // hello
        pi_read(2'd2, rd);
        check("hello echoed", rd & 16'h0001, 16'h0001);
        pi_write(2'd3, 16'h0000);

        // 1. word write / read
        bus_cycle(1'b0, 1'b1, 24'h001234, 16'hBEEF, rd);
        bus_cycle(1'b1, 1'b1, 24'h001234, 16'h0, rd);
        check("word rw", rd, 16'hBEEF);

        // 2. byte write / read (odd address -> LDS, low byte)
        bus_cycle(1'b0, 1'b0, 24'h000101, 16'h005A, rd);
        bus_cycle(1'b1, 1'b0, 24'h000101, 16'h0, rd);
        check("byte rw odd", rd, 16'h005A);
        bus_cycle(1'b1, 1'b0, 24'h000100, 16'h0, rd);   // even byte untouched RAM=0
        check("byte rw even", rd, 16'h0000);

        // 3. VPA / 6800 cycle read (+ vpa flag, bit8)
        bus_cycle(1'b1, 1'b1, 24'hE80002, 16'h0, rd);
        check("vpa read", rd, 16'h00AB);
        pi_read(2'd3, st);
        check("vpa flag set", st & 16'h0100, 16'h0100);
        bus_cycle(1'b1, 1'b1, 24'h001234, 16'h0, rd);
        pi_read(2'd3, st);
        check("vpa flag auto-clear", st & 16'h0100, 16'h0000);

        // 4. timeout on empty region; fault is sticky until flags_clr
        bus_cycle(1'b1, 1'b1, 24'h400000, 16'h0, rd);
        pi_read(2'd3, st);
        check("timeout flag", st & 16'h0002, 16'h0002);
        bus_cycle(1'b1, 1'b1, 24'h001234, 16'h0, rd);
        pi_read(2'd3, st);
        check("fault sticky", st & 16'h0002, 16'h0002);
        pi_write(2'd3, 16'h0004);                      // flags_clr
        pi_read(2'd3, st);
        check("fault cleared", st & 16'h0002, 16'h0000);

        // 5. DMAC: 3 writes while bus granted, then read snoop FIFO
        mobo.dma_burst(24'h002000, 16'hD001, 3);
        #200;
        pi_read(2'd3, st);
        check("snoop avail", st & 16'h0004, 16'h0004);
        pi_write(2'd3, 16'h0080);                      // reset snoop phase
        pi_read(2'd1, hi); pi_read(2'd1, lo); pi_read(2'd1, da);
        check("snoop0 hi", hi & 16'hE0FF, {2'b11, 6'b0, 8'h00});  // uds+lds, rw=0, A23:16=00 (fc/n masked)
        check("snoop0 n>=4", hi & 16'h0300, 16'h0300);           // full-length cycle
        check("snoop0 lo", lo, 16'h2000);
        check("snoop0 data", da, 16'hD001);
        pi_read(2'd1, hi); pi_read(2'd1, lo); pi_read(2'd1, da);
        check("snoop1 lo", lo, 16'h2002);
        check("snoop1 data", da, 16'hD002);
        pi_read(2'd1, hi); pi_read(2'd1, lo); pi_read(2'd1, da);
        check("snoop2 lo", lo, 16'h2004);
        check("snoop2 data", da, 16'hD003);
        pi_read(2'd3, st);
        check("snoop drained", st & 16'h0004, 16'h0000);
        // and the writes really landed in RAM
        bus_cycle(1'b1, 1'b1, 24'h002002, 16'h0, rd);
        check("dma ram", rd, 16'hD002);

        // 6. IPL + IRQ
        pi_write(2'd3, 16'h0040);                      // irq_en
        b_ipl_n = 3'b010;                              // level 5
        #500;
        pi_read(2'd3, st);
        check("ipl level", (st >> 3) & 16'h0007, 16'h0005);
        if (pi_irq !== 1'b1) begin $display("FAIL pi_irq"); errors = errors + 1; end
        else $display("pass pi_irq");
        b_ipl_n = 3'b111;

        // 6b. busy_irq: a wt read holds PI_IRQ high until it completes;
        // the line must be up within a few clocks of the ADDR/CTRL strobe
        // (the host waits >= 250ns before it looks) and DATA must be valid
        // by the time it drops.  Posted writes (wt=0) leave it alone.
        begin : busy_irq_t
            integer t_up, t_dn;
            pi_write(2'd3, 16'h0240);                  // busy_irq + irq_en
            pi_read(2'd3, st);
            check("busy_irq echo", st & 16'h8000, 16'h8000);
            if (pi_irq !== 1'b0) begin $display("FAIL busy_irq idle line"); errors = errors + 1; end
            pi_write(2'd1, 16'h1234);                  // $001234 = BEEF from test 1
            pi_reg_a = 2'd2; pi_ad_o = {1'b0, 3'b101, 1'b1, 1'b0, 1'b1, 1'b1, 8'h00}; pi_ad_drv = 1;
            #50 pi_wr_n = 0;
            #250 pi_wr_n = 1;
            t_up = 0;
            while (pi_irq !== 1'b1 && t_up < 1000) begin #10 t_up = t_up + 10; end
            #100 pi_ad_drv = 0;
            if (t_up >= 1000) begin $display("FAIL busy_irq never rose"); errors = errors + 1; end
            else $display("pass busy_irq up after %0d ns", t_up);
            t_dn = 0;
            while (pi_irq !== 1'b0 && t_dn < 5000) begin #10 t_dn = t_dn + 10; end
            if (t_dn >= 5000) begin $display("FAIL busy_irq never fell"); errors = errors + 1; end
            else $display("pass busy_irq down after %0d ns", t_dn);
            pi_read(2'd3, st);
            check("busy_irq status idle", st & 16'h0001, 16'h0000);
            pi_read(2'd0, rd);
            check("busy_irq data", rd, 16'hBEEF);
            post_write(24'h004010, 16'h7777);
            #100;
            if (pi_irq !== 1'b0) begin $display("FAIL busy_irq posted write raised line"); errors = errors + 1; end
            else $display("pass busy_irq posted write quiet");
            st = 16'h0001;
            while (st[0]) pi_read(2'd3, st);
            // a pending interrupt keeps the line high regardless (host falls back to STATUS)
            b_ipl_n = 3'b010;
            #500;
            if (pi_irq !== 1'b1) begin $display("FAIL busy_irq ipl line"); errors = errors + 1; end
            b_ipl_n = 3'b111;
            #500;
            if (pi_irq !== 1'b0) begin $display("FAIL busy_irq line stuck"); errors = errors + 1; end
            else $display("pass busy_irq ipl passthrough");
            // a faulting wt read: the sticky fault keeps the line high until
            // flags_clr, so the host cannot mistake it for a clean completion
            pi_write(2'd1, 16'h0000);
            pi_write(2'd2, {1'b0, 3'b101, 1'b1, 1'b0, 1'b1, 1'b1, 8'hF0});   // $F00000 -> BERR
            #6000;
            if (pi_irq !== 1'b1) begin $display("FAIL busy_irq fault line low"); errors = errors + 1; end
            pi_read(2'd3, st);
            check("busy_irq fault status", st & 16'h0003, 16'h0002);
            pi_write(2'd3, 16'h0244);                  // flags_clr
            #200;
            if (pi_irq !== 1'b0) begin $display("FAIL busy_irq fault line stuck"); errors = errors + 1; end
            else $display("pass busy_irq fault holds line");
            pi_write(2'd3, 16'h0040);                  // back to plain irq_en
        end

        // 7. BERR region
        bus_cycle(1'b1, 1'b1, 24'hF00000, 16'h0, rd);
        pi_read(2'd3, st);
        check("berr flag", st & 16'h0002, 16'h0002);
        pi_write(2'd3, 16'h0044);                      // clear flags, keep irq_en

        // 8. posted writes: fire 8 without polling, then verify
        mon_as = 1; as_fall = 0; as_min = 0; as_max = 0; as_n_cyc = 0; as_lo_min = 0; as_lo_max = 0;
        for (i = 0; i < 8; i = i + 1)
            post_write(24'h004000 + 2 * i[23:0], 16'h7A00 + i[15:0]);
        pi_read(2'd3, st);
        check("cmd fifo not full", st & 16'h0400, 16'h0000);
        while (st[0]) pi_read(2'd3, st);               // wait drain
        mon_as = 0;
        $display("info posted write AS-to-AS: %0d cycles, min %0t max %0t; AS low min %0t max %0t",
                 as_n_cyc, as_min, as_max, as_lo_min, as_lo_max);
        if (as_lo_max > 64'd400) begin                      // ns
            $display("FAIL write cycle longer than 5 bus clocks");
            errors = errors + 1;
        end
        errors = errors;
        for (i = 0; i < 8; i = i + 1) begin
            bus_cycle(1'b1, 1'b1, 24'h004000 + 2 * i[23:0], 16'h0, rd);
            if (rd !== 16'h7A00 + i[15:0]) begin
                $display("FAIL posted[%0d]: %04x", i, rd);
                errors = errors + 1;
            end
        end
        $display("pass posted writes x8");

        // 8b. prefetch stream over the 8 posted words: one command, then
        // STATUS/DATA pairs only
        begin : pf_seq
            integer bad, k;
            bad = 0;
            mon_as = 1; as_fall = 0; as_min = 0; as_max = 0; as_n_cyc = 0; as_lo_min = 0; as_lo_max = 0;
            pf_read(24'h004000, rd);
            if (rd !== 16'h7A00) bad = bad + 1;
            for (k = 1; k < 8; k = k + 1) begin
                pf_next(rd);
                if (rd !== 16'h7A00 + k[15:0]) begin
                    $display("FAIL pf word[%0d]: %04x", k, rd); bad = bad + 1;
                end
            end
            mon_as = 0;
            check("prefetch stream x8", bad[15:0], 16'd0);
            $display("info prefetch AS-to-AS: %0d cycles, min %0t max %0t", as_n_cyc, as_min, as_max);
            pi_read(2'd3, st);
            check("prefetch still active", st & 16'h4000, 16'h4000);
            // a normal command flushes the stream
            bus_cycle(1'b1, 1'b1, 24'h004002, 16'h0, rd);
            check("read after stream", rd, 16'h7A01);
            pi_read(2'd3, st);
            check("stream flushed", st & 16'h7000, 16'h0000);
            // the stream stops at the 64KB boundary (never enters the next block)
            post_write(24'h00FFFC, 16'h1234);
            post_write(24'h00FFFE, 16'h5678);
            pf_read(24'h00FFFC, rd);
            check("pf boundary word 0", rd, 16'h1234);
            pf_next(rd);
            check("pf boundary word 1", rd, 16'h5678);
            pf_next(rd);
            check("pf boundary end", rd, 16'hDEAD);
            pi_read(2'd3, st);
            check("pf boundary inactive", st & 16'h7000, 16'h0000);
            // a faulting pf command: sticky flag, nothing buffered
            pf_read(24'hF00000, rd);
            check("pf berr no data", rd, 16'hDEAD);
            pi_read(2'd3, st);
            check("pf berr flag", st & 16'h7002, 16'h0002);
            pi_write(2'd3, 16'h0044);
            bus_cycle(1'b1, 1'b1, 24'h004004, 16'h0, rd);
            check("read after pf berr", rd, 16'h7A02);
        end

        // 9. deep snoop + overflow (FIFO is 32 in this TB): 40-write DMA burst.
        // The 32 kept records come out first, then the lost range as two
        // strobe-less records (n=1: low bound, n=2: high bound); ovf stays
        // sticky until the stream reset.
        mobo.dma_burst(24'h003000, 16'hA000, 40);
        #400;
        pi_read(2'd3, st);
        check("snoop ovf set", st & 16'h0200, 16'h0200);
        begin : deep_drain
            integer k;
            k = 0;
            pi_read(2'd3, st);
            while ((st & 16'h0004) && k < 40) begin
                pi_read(2'd1, hi); pi_read(2'd1, lo); pi_read(2'd1, da);
                if (k == 0) begin
                    check("deep snoop first lo", lo, 16'h3000);
                    check("deep snoop first data", da, 16'hA000);
                end
                if (k == 31) check("deep snoop last kept lo", lo, 16'h303E);
                if (k == 32) begin
                    check("range lo hi-word", hi, 16'h0100);   // no strobes, fc=0, n=1, A23:16=0
                    check("range lo", lo, 16'h3040);
                end
                if (k == 33) begin
                    check("range hi hi-word", hi, 16'h0200);
                    check("range hi", lo, 16'h304E);
                end
                k = k + 1;
                pi_read(2'd3, st);
            end
            check("deep snoop kept+range", k[15:0], 16'd34);
        end
        pi_read(2'd3, st);
        check("snoop ovf still set", st & 16'h0200, 16'h0200);
        pi_write(2'd3, 16'h00C0);                      // snoop reset(+ovf clr), irq_en
        pi_read(2'd3, st);
        check("snoop ovf cleared", st & 16'h0200, 16'h0000);

        // 9a. snoop2 stream (REG3 bit13): HI reads 0 when empty, sequential
        // word writes come as continuation records (HI bit13, LO skipped)
        pi_write(2'd3, 16'h20C0);                      // snoop2 + snoop reset + irq_en
        pi_read(2'd2, hi);
        check("snoop2 echo", hi & 16'h0008, 16'h0008);
        pi_read(2'd1, hi);
        check("snoop2 empty hi", hi, 16'h0000);
        pi_read(2'd1, hi);
        check("snoop2 empty hi again", hi, 16'h0000);
        mobo.dma_burst(24'h005000, 16'hC000, 3);
        #400;
        pi_read(2'd1, hi); pi_read(2'd1, lo); pi_read(2'd1, da);
        check("snoop2 rec0 hi", hi & 16'hE3FF, 16'hC300);  // uds+lds, cont=0, n=3, A23:16=0
        check("snoop2 rec0 lo", lo, 16'h5000);
        check("snoop2 rec0 data", da, 16'hC000);
        pi_read(2'd1, hi); pi_read(2'd1, da);
        check("snoop2 rec1 hi", hi & 16'hE3FF, 16'hE300);  // cont
        check("snoop2 rec1 data", da, 16'hC001);
        pi_read(2'd1, hi); pi_read(2'd1, da);
        check("snoop2 rec2 hi", hi & 16'hE3FF, 16'hE300);
        check("snoop2 rec2 data", da, 16'hC002);
        pi_read(2'd1, hi);
        check("snoop2 drained hi", hi, 16'h0000);
        pi_read(2'd3, st);
        check("snoop2 drained", st & 16'h0004, 16'h0000);
        // overflow under snoop2: 32 kept (first full, rest cont), then the range
        mobo.dma_burst(24'h003000, 16'hA000, 40);
        #400;
        begin : deep_drain2
            integer k;
            k = 0;
            pi_read(2'd1, hi);
            while (hi != 16'h0000 && k < 40) begin
                if (hi & 16'h2000) begin
                    pi_read(2'd1, da);
                end else begin
                    pi_read(2'd1, lo); pi_read(2'd1, da);
                end
                if (k == 0)  check("snoop2 deep first lo", lo, 16'h3000);
                if (k == 1)  check("snoop2 deep cont", hi & 16'h2000, 16'h2000);
                if (k == 31) check("snoop2 deep last data", da, 16'hA01F);
                if (k == 32) begin
                    check("snoop2 range lo hi-word", hi, 16'h0100);
                    check("snoop2 range lo", lo, 16'h3040);
                end
                if (k == 33) begin
                    check("snoop2 range hi hi-word", hi, 16'h0200);
                    check("snoop2 range hi", lo, 16'h304E);
                end
                k = k + 1;
                pi_read(2'd1, hi);
            end
            check("snoop2 deep kept+range", k[15:0], 16'd34);
        end
        // a record after a drop carries a full address again
        mobo.dma_burst(24'h003050, 16'hA050, 1);
        #400;
        pi_read(2'd1, hi); pi_read(2'd1, lo); pi_read(2'd1, da);
        check("snoop2 after drop not cont", hi & 16'h2000, 16'h0000);
        check("snoop2 after drop lo", lo, 16'h3050);
        pi_write(2'd3, 16'h00C0);                      // back to the legacy stream
        pi_read(2'd2, hi);
        check("snoop2 off echo", hi & 16'h0008, 16'h0000);

        // 9b. back-to-back STATUS/REG1 reads (~15ns turnaround) must not
        // slip the hi/lo/data phase (a STATUS read counted as a REG1 read)
        mobo.dma_burst(24'h004000, 16'hB000, 3);
        #400;
        pi_read_tight(2'd3, st); pi_read_tight(2'd1, hi); pi_read_tight(2'd1, lo); pi_read_tight(2'd1, da);
        check("tight rec0 lo", lo, 16'h4000); check("tight rec0 data", da, 16'hB000);
        pi_read_tight(2'd3, st); pi_read_tight(2'd1, hi); pi_read_tight(2'd1, lo); pi_read_tight(2'd1, da);
        check("tight rec1 lo", lo, 16'h4002); check("tight rec1 data", da, 16'hB001);
        pi_read_tight(2'd3, st); pi_read_tight(2'd1, hi); pi_read_tight(2'd1, lo); pi_read_tight(2'd1, da);
        check("tight rec2 lo", lo, 16'h4004); check("tight rec2 data", da, 16'hB002);
        pi_read(2'd3, st);
        check("tight drained", st & 16'h0004, 16'h0000);

        // 10. IACK level 5 -> vectored (0x40 via DTACK), no VPA flag
        bus_cycle_fc(3'b111, 1'b1, 1'b0, 24'hFFFFFB, 16'h0, rd);
        pi_read(2'd3, st);
        check("iack vector data", rd & 16'h00FF, 16'h0040);
        check("iack vector no-vpa", st & 16'h0100, 16'h0000);

        // 11. IACK level 3 -> autovector (VPA flag)
        bus_cycle_fc(3'b111, 1'b1, 1'b0, 24'hFFFFF7, 16'h0, rd);
        pi_read(2'd3, st);
        check("iack autovector vpa", st & 16'h0100, 16'h0100);

        // 12. RESET/HALT drive -> sense loopback through Q1/Q2 model
        pi_write(2'd3, 16'h0043);       // drv_reset + drv_halt + irq_en
        #1000;
        pi_read(2'd3, st);
        check("reset/halt loopback", st & 16'h00C0, 16'h00C0);
        pi_write(2'd3, 16'h0040);
        #1000;
        pi_read(2'd3, st);
        check("reset/halt released", st & 16'h00C0, 16'h0000);

        // 13. DMA burst arriving in the middle of a posted-write stream
        fork
            begin
                for (i = 0; i < 6; i = i + 1)
                    post_write(24'h005000 + 2 * i[23:0], 16'h6600 + i[15:0]);
            end
            begin
                #300;
                mobo.dma_burst(24'h006000, 16'hB000, 3);
            end
        join
        st = 16'h0001;
        while (st[0]) pi_read(2'd3, st);
        begin : dma_mid_check
            integer bad;
            bad = 0;
            for (i = 0; i < 6; i = i + 1) begin
                bus_cycle(1'b1, 1'b1, 24'h005000 + 2 * i[23:0], 16'h0, rd);
                if (rd !== 16'h6600 + i[15:0]) bad = bad + 1;
            end
            check("posted x6 with DMA interleave", bad[15:0], 16'd0);
        end
        bus_cycle(1'b1, 1'b1, 24'h006002, 16'h0, rd);
        check("interleaved dma ram", rd, 16'hB001);
        pi_write(2'd3, 16'h00C0);
        pi_read(2'd3, st);
        begin : dma_mid_snoop
            integer k;
            k = 0;
            while ((st & 16'h0004) && k < 10) begin
                pi_read(2'd1, hi); pi_read(2'd1, lo); pi_read(2'd1, da);
                if (k == 0) check("interleave snoop lo", lo, 16'h6000);
                k = k + 1;
                pi_read(2'd3, st);
            end
            check("interleave snoop count", k[15:0], 16'd3);
        end

        // 12. prefetch vs DMA: a bus request stops the fetching but the
        // buffered words stay readable; a normal read works afterwards
        begin : pf_dma
            integer k;
            pf_read(24'h005000, rd);
            check("pf before dma", rd, 16'h6600);
            #3000;                                     // let the FIFO fill up
            pi_read(2'd3, st);
            check("pf fifo full-ish", st & 16'h3000, 16'h3000);
            mobo.dma_burst(24'h007000, 16'hC000, 4);
            pi_read(2'd3, st);
            check("pf stopped by BR", st & 16'h4000, 16'h0000);
            for (k = 1; k < 6; k = k + 1) begin
                pf_next(rd);
                if (rd !== 16'h6600 + k[15:0]) begin
                    $display("FAIL pf after dma word[%0d]: %04x", k, rd); errors = errors + 1;
                end
            end
            bus_cycle(1'b1, 1'b1, 24'h007002, 16'h0, rd);
            check("read after pf+dma", rd, 16'hC001);
            pi_write(2'd3, 16'h00C0);                  // snoop reset for a clean end
        end

        // ---- ai write mode (autostart + CTRL bit15): REG0 strobes drive
        // sequential writes, REG1 only sets the address ----
        begin
            pi_write(2'd3, 16'h0100);                  // autostart
            // upward word run at 004000: ctrl = ai, fc5, word, rw=0
            pi_write(2'd2, {1'b1, 3'b101, 2'b00, 1'b1, 1'b0, 8'h00});
            pi_write(2'd1, 16'h4000);                  // address only: no cycle yet
            pi_read(2'd3, st);
            check("ai: REG1 does not start", st & 16'h0001, 16'h0000);
            pi_write(2'd0, 16'hA100);                  // 004000
            pi_write(2'd0, 16'hA101);                  // 004002
            pi_write(2'd0, 16'hA102);                  // 004004
            pi_read(2'd3, st);
            while (st[0]) pi_read(2'd3, st);
            // downward word run (pf=1 as direction) from 004100
            pi_write(2'd2, {1'b1, 3'b101, 1'b0, 1'b1, 1'b1, 1'b0, 8'h00});
            pi_write(2'd1, 16'h4100);
            pi_write(2'd0, 16'hB100);                  // 004100
            pi_write(2'd0, 16'hB0FE);                  // 0040FE
            pi_write(2'd0, 16'hB0FC);                  // 0040FC
            // upward byte run from 004201 (odd start): lds, then uds of the next word
            pi_write(2'd2, {1'b1, 3'b101, 2'b00, 1'b0, 1'b0, 8'h00});
            pi_write(2'd1, 16'h4201);
            pi_write(2'd0, 16'h0011);                  // 004201
            pi_write(2'd0, 16'h0022);                  // 004202
            pi_write(2'd0, 16'h0033);                  // 004203
            pi_read(2'd3, st);
            while (st[0]) pi_read(2'd3, st);
            // a read in between (rw=1: REG1 starts as usual), then a
            // non-sequential ai write = REG1 + REG0
            pi_write(2'd2, {1'b0, 3'b101, 2'b00, 1'b1, 1'b1, 8'h00});
            pi_write(2'd1, 16'h4002);
            pi_read(2'd3, st);
            while (st[0]) pi_read(2'd3, st);
            pi_read(2'd0, rd);
            check("ai: read between runs", rd, 16'hA101);
            pi_write(2'd2, {1'b1, 3'b101, 2'b00, 1'b1, 1'b0, 8'h00});
            pi_write(2'd1, 16'h4300);
            pi_write(2'd0, 16'hC300);                  // 004300
            pi_write(2'd0, 16'hC302);                  // 004302
            pi_write(2'd3, 16'h0000);                  // autostart off again
            pi_write(2'd2, {1'b0, 3'b101, 2'b00, 1'b1, 1'b0, 8'h00});   // ai off
            pi_read(2'd3, st);
            while (st[0]) pi_read(2'd3, st);
            bus_cycle(1'b1, 1'b1, 24'h004000, 16'h0, rd); check("ai up 0", rd, 16'hA100);
            bus_cycle(1'b1, 1'b1, 24'h004002, 16'h0, rd); check("ai up 1", rd, 16'hA101);
            bus_cycle(1'b1, 1'b1, 24'h004004, 16'h0, rd); check("ai up 2", rd, 16'hA102);
            bus_cycle(1'b1, 1'b1, 24'h004006, 16'h0, rd); check("ai up end (old 7A03 kept)", rd, 16'h7A03);
            bus_cycle(1'b1, 1'b1, 24'h004100, 16'h0, rd); check("ai down 0", rd, 16'hB100);
            bus_cycle(1'b1, 1'b1, 24'h0040FE, 16'h0, rd); check("ai down 1", rd, 16'hB0FE);
            bus_cycle(1'b1, 1'b1, 24'h0040FC, 16'h0, rd); check("ai down 2", rd, 16'hB0FC);
            bus_cycle(1'b1, 1'b1, 24'h0040FA, 16'h0, rd); check("ai down end", rd, 16'h0000);
            bus_cycle(1'b1, 1'b1, 24'h004200, 16'h0, rd); check("ai byte 0", rd, 16'h0011);
            bus_cycle(1'b1, 1'b1, 24'h004202, 16'h0, rd); check("ai byte 1-2", rd, 16'h2233);
            bus_cycle(1'b1, 1'b1, 24'h004300, 16'h0, rd); check("ai nonseq 0", rd, 16'hC300);
            bus_cycle(1'b1, 1'b1, 24'h004302, 16'h0, rd); check("ai nonseq 1", rd, 16'hC302);
        end

        // 13. bus cycle length: 4 bus clocks (250ns at 16MHz) in the default
        // timing, 5 with bus_slow (REG3 bit12); the data must survive both.
        begin : cyc_len
            integer k, m;
            for (m = 0; m < 2; m = m + 1) begin
                pi_write(2'd3, m ? 16'h1040 : 16'h0040);   // (bus_slow) + irq_en
                for (k = 0; k < 8; k = k + 1)
                    post_write(24'h004400 + 2 * k[23:0], 16'hD000 + k[15:0] + m[15:0] * 16'h100);
                pi_read(2'd3, st);
                while (st[0]) pi_read(2'd3, st);
                mon_as = 1; as_fall = 0; as_min = 0; as_max = 0; as_n_cyc = 0; as_lo_min = 0; as_lo_max = 0;
                pf_read(24'h004400, rd);
                if (rd !== 16'hD000 + m[15:0] * 16'h100) begin $display("FAIL cyc_len word 0 (slow=%0d): %04x", m, rd); errors = errors + 1; end
                for (k = 1; k < 8; k = k + 1) begin
                    pf_next(rd);
                    if (rd !== 16'hD000 + k[15:0] + m[15:0] * 16'h100) begin
                        $display("FAIL cyc_len word %0d (slow=%0d): %04x", k, m, rd); errors = errors + 1;
                    end
                end
                mon_as = 0;
                $display("info stream AS-to-AS slow=%0d: %0d cycles, min %0t max %0t; AS low min %0t max %0t",
                         m, as_n_cyc, as_min, as_max, as_lo_min, as_lo_max);
                if (as_min != (m ? 64'd312 : 64'd250)) begin        // $time is in ns here
                    $display("FAIL cyc_len: AS-to-AS min %0t, want %0d bus clocks", as_min, m ? 5 : 4);
                    errors = errors + 1;
                end
                bus_cycle(1'b1, 1'b1, 24'h004400, 16'h0, rd);   // flush the stream
            end
            pi_write(2'd3, 16'h0040);
        end

        // 6c (last: it grants the bus to the DMAC model and would upset the measurements below)
            // 6c. hold read vs. a stale prefetch FIFO (docs 30/32).  Stream
            // words are only flushed when the engine picks the NEXT command
            // up; a hold read (RD# low on DATA while the line is polled)
            // that starts before that must return its own rdata when the
            // line drops, not the FIFO head it saw at the strobe start.
            // Here another master holds the bus so the pickup is late.
            begin : hold_stale_t
                integer t;
                pi_write(2'd3, 16'h0200);                  // busy_irq only (no irq_en: snoop records must not hold the line)
                pi_write(2'd3, 16'h0280);                  // + snoop_rst pulse
                bus_cycle(1'b0, 1'b1, 24'h002000, 16'h1111, rd);
                bus_cycle(1'b0, 1'b1, 24'h002002, 16'h2222, rd);
                bus_cycle(1'b0, 1'b1, 24'h002004, 16'h3333, rd);
                bus_cycle(1'b0, 1'b1, 24'h003000, 16'hABCD, rd);
                pf_read(24'h002000, rd);                   // stream: the FIFO fills behind it
                check("stale-test stream head", rd, 16'h1111);
                #1500;                                     // words buffered, engine idle
                pi_read(2'd3, st);
                if (st[13:12] == 2'b00) begin $display("FAIL stale-test: no words buffered"); errors = errors + 1; end
                fork
                    mobo.dma_burst(24'h004000, 16'h5A00, 10);   // another master holds the bus ~3.5us
                    begin
                        #500;                              // granted: the engine sits in GRUN
                        pi_write(2'd1, 16'h3000);
                        pi_reg_a = 2'd2; pi_ad_o = {1'b0, 3'b101, 1'b1, 1'b0, 1'b1, 1'b1, 8'h00}; pi_ad_drv = 1;
                        #50 pi_wr_n = 0; #250 pi_wr_n = 1; #10 pi_ad_drv = 0;
                        pi_reg_a = 2'd0; #10 pi_rd_n = 0;  // hold read on DATA, RD# 20 ns after the command strobe (before it is even synchronised)
                        t = 0; while (pi_irq !== 1'b1 && t < 2000) begin #10 t = t + 10; end
                        if (t >= 2000) begin $display("FAIL stale-test: line never rose"); errors = errors + 1; end
                        t = 0; while (pi_irq !== 1'b0 && t < 20000) begin #10 t = t + 10; end
                        if (t >= 20000) begin $display("FAIL stale-test: line never fell"); errors = errors + 1; end
                        rd = pi_ad;                        // what the host samples as the line drops
                        #100 pi_rd_n = 1;
                        check("hold read after stale stream", rd, 16'hABCD);
                    end
                join
                #500;
                pi_read(2'd3, st);
                check("stale-test idle", st & 16'h0001, 16'h0000);
                pi_write(2'd3, 16'h0080);                  // snoop_rst: leave a clean FIFO
            end


        if (errors == 0) $display("ALL TESTS PASSED");
        else $display("%0d TEST(S) FAILED", errors);
        $finish;
    end

    initial begin
        #8_000_000;
        $display("GLOBAL TIMEOUT");
        $finish;
    end

endmodule

// ---------------- behavioral 74LVC8T245 ----------------
module xcvr #(parameter W=8) (
    inout wire [W-1:0] a,
    inout wire [W-1:0] b,
    input wire dir,          // 1: A->B
    input wire oe_n
);
    assign b = (!oe_n &&  dir) ? a : {W{1'bz}};
    assign a = (!oe_n && !dir) ? b : {W{1'bz}};
endmodule

// ---------------- X68000 motherboard model ----------------
module x68k_model (
    input  wire        clk,
    inout  wire [23:1] la,
    inout  wire [15:0] ld,
    inout  wire        as_n,
    inout  wire        uds_n,
    inout  wire        lds_n,
    inout  wire        rw,
    inout  wire [2:0]  fc,
    input  wire        vma_n,
    input  wire        e,
    output wire        dtack_n,
    output wire        berr_n,
    output wire        vpa_n,
    output wire        br_n,
    input  wire        bg_n,
    output wire        bgack_n
);
    // RAM 0x000000-0x00FFFF
    reg [7:0] ram_hi [0:32767];   // even bytes (D15:8)
    reg [7:0] ram_lo [0:32767];   // odd bytes
    integer i;
    initial for (i = 0; i < 32768; i = i + 1) begin
        ram_hi[i] = 8'h00; ram_lo[i] = 8'h00;
    end

    wire [23:0] addr = {la, 1'b0};
    wire iack     = (fc == 3'b111) && !as_n;
    wire [2:0] iack_lvl = la[3:1];
    // IACK: level 5 -> vectored (0x40, DTACK); level 3 -> autovector (VPA)
    wire iack_vec = iack && (iack_lvl == 3'd5);
    wire iack_av  = iack && (iack_lvl == 3'd3);
    wire sel_ram  = (addr[23:16] == 8'h00) && !iack;
    wire sel_vpa  = ((addr[23:16] == 8'hE8) && !iack) || iack_av;
    wire sel_berr = (addr[23:16] == 8'hF0) && !iack;

    // DTACK: 2-clock delay after strobes for RAM and vectored IACK
    reg [1:0] dt;
    always @(posedge clk)
        if (!as_n && (sel_ram || iack_vec) && !(uds_n && lds_n))
            dt <= {dt[0], 1'b1};
        else
            dt <= 2'b00;
    assign dtack_n = dt[1] ? 1'b0 : 1'b1;

    // vectored IACK data
    assign ld[7:0] = (iack_vec && !lds_n) ? 8'h40 : 8'bz;

    // RAM read drive / write latch
    wire reading = !as_n && sel_ram && rw;
    assign ld[15:8] = (reading && !uds_n) ? ram_hi[la[15:1]] : 8'bz;
    assign ld[7:0]  = (reading && !lds_n) ? ram_lo[la[15:1]] : 8'bz;

    always @(posedge clk)
        if (!as_n && sel_ram && !rw && dt[1]) begin
            if (!uds_n) ram_hi[la[15:1]] <= ld[15:8];
            if (!lds_n) ram_lo[la[15:1]] <= ld[7:0];
`ifdef DEBUG
            $display("[mobo] WR addr=%h uds=%b lds=%b ld=%h t=%0t",
                     {la,1'b0}, uds_n, lds_n, ld, $time);
`endif
        end

    // VPA device: read data 0x00AB, uses E cycle
    assign vpa_n = (!as_n && sel_vpa) ? 1'b0 : 1'b1;
    assign ld[15:8] = (!as_n && sel_vpa && rw && !vma_n && e) ? 8'h00 : 8'bz;
    assign ld[7:0]  = (!as_n && sel_vpa && rw && !vma_n && e) ? 8'hAB : 8'bz;

    // BERR region
    reg [1:0] be;
    always @(posedge clk)
        if (!as_n && sel_berr) be <= {be[0], 1'b1};
        else be <= 2'b00;
    assign berr_n = be[1] ? 1'b0 : 1'b1;

    // ---------------- DMAC master model ----------------
    reg drq = 0, dack = 0;
    reg [23:1] d_la; reg [15:0] d_ld;
    reg d_as = 1, d_uds = 1, d_lds = 1, d_rw = 1;
    reg d_drv = 0;

    assign br_n    = drq  ? 1'b0 : 1'b1;
    assign bgack_n = dack ? 1'b0 : 1'b1;
    assign la   = d_drv ? d_la : 23'bz;
    assign ld   = (d_drv && !d_rw) ? d_ld : 16'bz;
    assign as_n = d_drv ? d_as : 1'bz;
    assign uds_n = d_drv ? d_uds : 1'bz;
    assign lds_n = d_drv ? d_lds : 1'bz;
    assign rw   = d_drv ? d_rw : 1'bz;
    assign fc   = d_drv ? 3'b101 : 3'bz;   // DMAC drives FC during its cycles

    task dma_burst(input [23:0] base, input [15:0] d0, input integer count);
        integer k;
        begin
            drq = 1;
            wait (bg_n === 1'b0);
            @(posedge clk);
            dack = 1; drq = 0;
            #200;                       // buffers turn around
            d_drv = 1;
            for (k = 0; k < count; k = k + 1) begin
                d_la = base[23:1] + k[22:0];
                d_rw = 0;
                @(posedge clk); d_as = 0;
                @(posedge clk); d_uds = 0; d_lds = 0; d_ld = d0 + k[15:0];
                wait (dtack_n === 1'b0);
                @(posedge clk);
                d_as = 1; d_uds = 1; d_lds = 1;
                @(posedge clk);
            end
            d_drv = 0; d_rw = 1;
            #100 dack = 0;
        end
    endtask

endmodule
