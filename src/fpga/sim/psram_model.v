// Behavioral QPI PSRAM (VTI7064 / APS6404 class) for simulation.
// SPI mode 0.  Understands, in 1-bit SPI mode: 66h (reset enable), 99h
// (reset), 35h (enter QPI); in QPI mode: EBh (fast read quad, WAIT dummy
// cycles), 38h (quad write).  Only the low AW address bits are stored.
`timescale 1ns/1ps
`default_nettype none

module psram_model #(parameter AW = 16, parameter WAIT = 6, parameter NAME = "psram") (
    input  wire       ce_n,
    input  wire       sclk,
    inout  wire [3:0] sio
);
    reg [7:0] mem [0:(1<<AW)-1];
    integer i;
    initial for (i = 0; i < (1<<AW); i = i + 1) mem[i] = 8'h00;

    reg        qpi = 0;
    reg        rst_en = 0;
    reg [7:0]  cmd;
    reg [23:0] addr;
    integer    cyc;                 // SCLK rising edges since CE fell
    reg [31:0] sh;                  // input shift
    reg        drive = 0;
    reg [3:0]  dout = 4'h0;
    assign sio = drive ? dout : 4'bzzzz;

    // address phase length in cycles
    localparam SPI_CMD = 8;         // bits
    localparam QPI_CMD = 2, QPI_ADDR = 6;

    always @(negedge ce_n) begin
        cyc = 0; sh = 0; drive = 0; cmd = 8'h00;
    end
    always @(posedge ce_n) begin
        drive = 0;
        // commands that act at CE rise
        if (!qpi) begin
            if (cmd == 8'h66) rst_en = 1;
            else if (cmd == 8'h99) begin if (rst_en) begin qpi = 0; end rst_en = 0; end
            else if (cmd == 8'h35) begin qpi = 1; `ifdef DEBUG $display("[%0s] enter QPI @%0t", NAME, $time); `endif end
        end
    end

    // sample on the rising edge
    always @(posedge sclk) if (!ce_n) begin
        if (!qpi) begin
            sh = {sh[30:0], sio[0]};
            cyc = cyc + 1;
            if (cyc == SPI_CMD) cmd = sh[7:0];
        end else begin
            sh = {sh[27:0], sio};
            cyc = cyc + 1;
            if (cyc == QPI_CMD) cmd = sh[7:0];
            if (cyc == QPI_CMD + QPI_ADDR) addr = sh[23:0];
            if (cmd == 8'h38 && cyc > QPI_CMD + QPI_ADDR && ((cyc - QPI_CMD - QPI_ADDR) % 2) == 0) begin
                // a full byte has arrived
                mem[addr[AW-1:0]] = sh[7:0];
                `ifdef DEBUG $display("[%0s] WR %06x <= %02x @%0t", NAME, addr, sh[7:0], $time); `endif
                addr = addr + 24'd1;
            end
        end
    end

    // drive on the falling edge (read data after the dummy cycles)
    always @(negedge sclk) if (!ce_n && qpi && cmd == 8'hEB) begin
        if (cyc >= QPI_CMD + QPI_ADDR + WAIT) begin
            // nibble index within the data stream: cycles elapsed since the
            // end of the wait phase
            case ((cyc - QPI_CMD - QPI_ADDR - WAIT) % 2)
                0: dout = mem[addr[AW-1:0]][7:4];
                1: begin dout = mem[addr[AW-1:0]][3:0]; end
            endcase
            if (((cyc - QPI_CMD - QPI_ADDR - WAIT) % 2) == 1) addr = addr + 24'd1;
            drive = 1;
        end
    end
endmodule
