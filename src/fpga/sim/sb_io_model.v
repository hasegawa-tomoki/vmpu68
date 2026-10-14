// SPDX-License-Identifier: MIT
// Behavioural SB_IO / SB_GB_IO for simulation - the subset vmpu68 uses:
//   PIN_TYPE[5:4]  01 = output always on, 10 = tristate output, enable from the fabric, 00 = input only
//   PIN_TYPE[3:2]  01 = registered output (OUTPUT_CLK; NEG_TRIGGER = 1 -> falling edge), 00 = DDR, 1x = direct
//   PIN_TYPE[1:0]  01 = plain input (D_IN_0 = pad)
// Clock-to-out is zero here; on the device it is a few ns (plus the 8T245).
`timescale 1ns/1ps
module SB_IO #(
    parameter [5:0] PIN_TYPE = 6'b000001,
    parameter [0:0] PULLUP = 1'b0,
    parameter [0:0] NEG_TRIGGER = 1'b0,
    parameter IO_STANDARD = "SB_LVCMOS"
) (
    inout  wire PACKAGE_PIN,
    input  wire LATCH_INPUT_VALUE,
    input  wire CLOCK_ENABLE,
    input  wire INPUT_CLK,
    input  wire OUTPUT_CLK,
    input  wire OUTPUT_ENABLE,
    input  wire D_OUT_0,
    input  wire D_OUT_1,
    output wire D_IN_0,
    output wire D_IN_1
);
    reg  oq = 1'b0, oq1 = 1'b0;
    wire oclk = OUTPUT_CLK ^ NEG_TRIGGER;
    always @(posedge oclk) oq  <= D_OUT_0;
    always @(negedge oclk) oq1 <= D_OUT_1;
    // [3:2] 01 = registered, 00 = DDR (D_OUT_0 while the clock is high, D_OUT_1 while low), 1x = direct.
    // The DDR mux follows a copy of the clock delayed by two delta cycles so the
    // register updates scheduled at the edge land first (no zero-width glitch).
    reg  oclk_d1, oclk_d2;
    always @* oclk_d1 <= oclk;
    always @* oclk_d2 <= oclk_d1;
    wire o  = PIN_TYPE[3] ? D_OUT_0 : (PIN_TYPE[2] ? oq : (oclk_d2 ? oq : oq1));
    wire en = (PIN_TYPE[5:4] == 2'b01) ? 1'b1 : (PIN_TYPE[5:4] == 2'b00) ? 1'b0 : OUTPUT_ENABLE;
    assign PACKAGE_PIN = en ? o : 1'bz;
    assign D_IN_0 = PACKAGE_PIN;
    assign D_IN_1 = PACKAGE_PIN;
endmodule

module SB_GB_IO #(
    parameter [5:0] PIN_TYPE = 6'b000001,
    parameter [0:0] PULLUP = 1'b0,
    parameter [0:0] NEG_TRIGGER = 1'b0,
    parameter IO_STANDARD = "SB_LVCMOS"
) (
    inout  wire PACKAGE_PIN,
    output wire GLOBAL_BUFFER_OUTPUT,
    input  wire LATCH_INPUT_VALUE,
    input  wire CLOCK_ENABLE,
    input  wire INPUT_CLK,
    input  wire OUTPUT_CLK,
    input  wire OUTPUT_ENABLE,
    input  wire D_OUT_0,
    input  wire D_OUT_1,
    output wire D_IN_0,
    output wire D_IN_1
);
    assign GLOBAL_BUFFER_OUTPUT = PACKAGE_PIN;
    assign D_IN_0 = PACKAGE_PIN;
    assign D_IN_1 = PACKAGE_PIN;
endmodule
