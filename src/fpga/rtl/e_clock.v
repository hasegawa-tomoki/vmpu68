// SPDX-License-Identifier: MIT
// 68000 E clock: free-running CLK16/10, 6 clk16 low then 4 high.
// Runs in the fast clock domain, advancing on tick16 (one pulse per
// 16MHz bus-clock edge) so timing semantics match a real 68000.
`default_nettype none

module e_clock (
    input  wire clk,
    input  wire tick16,
    output reg  e,
    input  wire vma_req,
    output reg  vma_n,
    output reg  vma_done
);
    reg [3:0] ph;          // 0..9;  low for 0..5, high for 6..9

    initial begin
        ph = 4'd0;
        e = 1'b0;
        vma_n = 1'b1;
        vma_done = 1'b0;
    end

    always @(posedge clk) if (tick16) begin
        ph <= (ph == 4'd9) ? 4'd0 : ph + 4'd1;
        e  <= (ph >= 4'd5) && (ph != 4'd9);

        vma_done <= 1'b0;
        if (vma_req) begin
            if (!vma_n) begin
                if (ph == 4'd9) begin
                    vma_n    <= 1'b1;
                    vma_done <= 1'b1;
                end
            end else if (ph == 4'd2)
                vma_n <= 1'b0;
        end else
            vma_n <= 1'b1;
    end
endmodule
