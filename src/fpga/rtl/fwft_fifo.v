// SPDX-License-Identifier: MIT
// First-word-fall-through FIFO with registered (BRAM-friendly) read port.
// `rdy`/`head` are registered and become valid one clock after the state
// they reflect; consumers polling at tick16 (or slower) never see the gap.
`default_nettype none

module fwft_fifo #(parameter AW = 9, DW = 48, AFULL_MARGIN = 64) (
    input  wire          clk,
    input  wire          push,
    input  wire [DW-1:0] din,
    input  wire          pop,        // consume head (ignored when !rdy)
    output wire [DW-1:0] head,
    output reg           rdy,
    output wire          full,
    output wire          afull,      // fewer than AFULL_MARGIN entries free
    output reg           ovf,        // sticky: push while full
    input  wire          ovf_clr
);
    reg [DW-1:0] mem [0:(1<<AW)-1];
    reg [AW:0] wp = 0, rp = 0;
    reg [DW-1:0] head_r;

    assign full = (wp[AW-1:0] == rp[AW-1:0]) && (wp[AW] != rp[AW]);
    wire [AW:0] used = wp - rp;
    assign afull = (used >= ((1 << AW) - AFULL_MARGIN));

    wire        do_push = push && !full;
    wire        do_pop  = pop && rdy && (wp != rp);
    wire [AW:0] rp_n    = do_pop ? rp + 1'b1 : rp;
    wire [AW:0] wp_n    = do_push ? wp + 1'b1 : wp;

    initial begin rdy = 0; ovf = 0; end

    always @(posedge clk) begin
        if (do_push) mem[wp[AW-1:0]] <= din;
        wp <= wp_n;
        rp <= rp_n;
        head_r <= mem[rp_n[AW-1:0]];
        // note: OLD wp, not wp_n — rdy must not assert until one clock
        // after the memory write, when head_r has caught up
        rdy <= (wp != rp_n);
        if (push && full) ovf <= 1'b1;
        else if (ovf_clr) ovf <= 1'b0;
    end

    assign head = head_r;
endmodule
