// Standalone test of psram_qpi against the behavioral PSRAM model.
`timescale 1ns/1ps
`default_nettype none

module tb_psram;
    reg clk = 0;
    always #8 clk = ~clk;              // 62.5 MHz

    reg        req = 0, we = 0;
    reg [23:0] addr = 0;
    reg [15:0] wdata = 0;
    reg [1:0]  be = 2'b11;
    wire [15:0] rdata;
    wire ack, busy, ready, present;
    wire [1:0] ce_n;
    wire sclk;
    wire [3:0] sio_o;
    wire sio_oe;
    wire [3:0] sio;
    assign sio = sio_oe ? sio_o : 4'bzzzz;

    psram_qpi #(.DIV(2), .WAIT_EB(6), .INIT_WAIT(300)) dut (
        .clk(clk), .req(req), .we(we), .addr(addr), .wdata(wdata), .be(be),
        .rdata(rdata), .ack(ack), .busy(busy), .ready(ready), .present(present),
        .ce_n(ce_n), .sclk(sclk), .sio_o(sio_o), .sio_oe(sio_oe), .sio_i(sio)
    );
    psram_model #(.AW(16), .NAME("chip0")) m0 (.ce_n(ce_n[0]), .sclk(sclk), .sio(sio));
    psram_model #(.AW(16), .NAME("chip1")) m1 (.ce_n(ce_n[1]), .sclk(sclk), .sio(sio));

    integer errors = 0;
    task xfer(input w, input [23:0] a, input [15:0] d, input [1:0] b, output [15:0] out);
        begin
            @(posedge clk); while (busy) @(posedge clk);
            #1 we = w; addr = a; wdata = d; be = b; req = 1;
            @(posedge clk); #1 req = 0;
            @(posedge ack); out = rdata;
            @(posedge clk);
        end
    endtask
    task check(input [63:0] name, input [15:0] got, input [15:0] exp);
        if (got !== exp) begin $display("FAIL %0s: got %04x expected %04x", name, got, exp); errors = errors + 1; end
        else $display("pass %0s: %04x", name, got);
    endtask

    reg [15:0] r;
    time t0;
    initial begin #400000 $display("TIMEOUT ist=%0d fst=%0d cyc=%0d ready=%b", dut.ist, dut.fst, dut.cyc, ready); $finish; end
    initial begin
        $dumpfile("/tmp/tb_psram.vcd"); $dumpvars(1, tb_psram); $dumpvars(1, dut);
        wait (ready);
        $display("ready @%0t present=%b (chip0 A55A at 0: %02x%02x, chip1 5AA5 at 0: %02x%02x)", $time, present,
                 m0.mem[0], m0.mem[1], m1.mem[0], m1.mem[1]);
        if (!present) begin $display("FAIL self test"); errors = errors + 1; end

        xfer(1, 24'h000100, 16'h1234, 2'b11, r);
        xfer(0, 24'h000100, 16'h0000, 2'b11, r); check("word", r, 16'h1234);
        xfer(1, 24'h000102, 16'hBEEF, 2'b11, r);
        xfer(0, 24'h000102, 16'h0000, 2'b11, r); check("word2", r, 16'hBEEF);
        xfer(1, 24'h000100, 16'hAB00, 2'b10, r);           // upper byte only
        xfer(0, 24'h000100, 16'h0000, 2'b11, r); check("byte hi", r, 16'hAB34);
        xfer(1, 24'h000100, 16'h00CD, 2'b01, r);           // lower byte only
        xfer(0, 24'h000100, 16'h0000, 2'b11, r); check("byte lo", r, 16'hABCD);
        xfer(1, 24'h80FFFE, 16'h5678, 2'b11, r);           // chip 1, top of the mirrored window
        xfer(0, 24'h80FFFE, 16'h0000, 2'b11, r); check("chip1", r, 16'h5678);
        xfer(0, 24'h000102, 16'h0000, 2'b11, r); check("chip0 still", r, 16'hBEEF);
        // timing of one read
        @(posedge clk); while (busy) @(posedge clk);
        t0 = $time; #1 we = 0; addr = 24'h000100; req = 1; @(posedge clk); #1 req = 0; @(posedge ack);
        $display("read frame: %0d ns", $time - t0);
        t0 = $time; @(posedge clk); while (busy) @(posedge clk); #1 we = 1; addr = 24'h000200; wdata = 16'h1111; be = 2'b11; req = 1; @(posedge clk); #1 req = 0; @(posedge ack);
        $display("write frame: %0d ns (incl. gap after the read)", $time - t0);

        if (errors == 0) $display("ALL TESTS PASSED"); else $display("%0d FAILURES", errors);
        $finish;
    end
endmodule
