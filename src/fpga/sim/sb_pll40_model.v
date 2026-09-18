// SPDX-License-Identifier: MIT
// Behavioral SB_PLL40_CORE for simulation: free-running 64MHz output,
// LOCK after 1us.  Phase relative to REFERENCECLK is arbitrary — the
// design synchronizes clk16 as data, so this is sufficient.
`timescale 1ns/1ps

module SB_PLL40_CORE #(
    parameter FEEDBACK_PATH = "SIMPLE",
    parameter [3:0] DIVR = 0,
    parameter [6:0] DIVF = 0,
    parameter [2:0] DIVQ = 0,
    parameter [2:0] FILTER_RANGE = 0
) (
    input  wire REFERENCECLK,
    output reg  PLLOUTGLOBAL,
    output wire PLLOUTCORE,
    output reg  LOCK,
    input  wire RESETB,
    input  wire BYPASS
);
    assign PLLOUTCORE = PLLOUTGLOBAL;
    initial begin
        PLLOUTGLOBAL = 0;
        LOCK = 0;
        #1000 LOCK = 1;
    end
    always #7.8125 PLLOUTGLOBAL = ~PLLOUTGLOBAL;
endmodule
