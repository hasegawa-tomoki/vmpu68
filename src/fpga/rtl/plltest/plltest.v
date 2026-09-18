`default_nettype none
module plltest (input wire pi_ad2, input wire bgack49, output wire pi_irq, output wire led_r, output wire led_g, output wire led_b);
    wire clk, lock;
    
    SB_PLL40_CORE #(.FEEDBACK_PATH("SIMPLE"), .DIVR(4'd0), .DIVF(7'd60), .DIVQ(3'd4), .FILTER_RANGE(3'd1)) u_pll (
        .REFERENCECLK(pi_ad2), .PLLOUTGLOBAL(clk), .LOCK(lock), .RESETB(1'b1), .BYPASS(1'b0));
    reg [24:0] c = 0; always @(posedge clk) c <= c + 1;
    reg [21:0] r = 0; always @(posedge pi_ad2) r <= r + 1;
    assign pi_irq = lock;
    assign led_g = ~lock;
    assign led_r = ~(c[24] & lock);
    assign led_b = ~(r[21] ^ bgack49);
endmodule
