// SPDX-License-Identifier: MIT
// QPI PSRAM controller for core 2.1: two VTI7064 (8 MB each, APS6404-class)
// sharing SCLK and SIO0-3, selected by CE0/CE1 (addr[23]).
//
// One request = one CE frame: command (2 SCLK), 24-bit address (6 SCLK),
// then for a read WAIT_EB dummy cycles and 4 SCLK of data in, for a write
// 2 or 4 SCLK of data out.  Words are stored big-endian like the 68000 bus:
// the byte at the even address is D15:8.  A byte write (be = 10 or 01)
// writes one byte at addr or addr+1.
//
// SCLK = clk / DIV (DIV = 2: ~31 MHz at the 61-64 MHz PLL clock).  The
// controller drives SIO on the SCLK falling edge and samples the device's
// data at the NEXT falling edge (a full period after the device launched
// it), which is safe against the device's tCLQV plus both I/O delays.
//
// Power-up: wait INIT_WAIT clocks (>= 150 us), then per chip in 1-bit SPI
// mode: RESET_EN (66h), RESET (99h), ENTER QPI (35h).  A self test then
// writes/reads one word in each chip; `present` reports the result and
// `ready` that the sequence is over.  On boards without the PSRAM the pins
// float: the self test fails, present = 0, and the engine never routes
// cycles here.
`default_nettype none

module psram_qpi #(
    parameter DIV       = 2,        // SCLK = clk / DIV (even, >= 2)
    parameter WAIT_EB   = 6,        // dummy cycles of the EBh quad read
    parameter INIT_WAIT = 14000     // clocks before the first command (~225 us @ 62 MHz)
) (
    input  wire        clk,

    input  wire        req,         // start a transfer (ignored while busy)
    input  wire        we,
    input  wire        two,         // read: fetch 2 words (addr, addr+2) in one frame; rdata2 = the second
    input  wire [23:0] addr,        // byte address; addr[23] selects the chip
    input  wire [15:0] wdata,
    input  wire [1:0]  be,          // [1] = byte at addr (D15:8), [0] = byte at addr+1 (D7:0)
    output reg  [15:0] rdata,
    output reg  [15:0] rdata2,
    output reg         ack,         // one clock: transfer done (rdata valid for reads)
    output wire        busy,
    output reg         ready,       // init + self test finished
    output reg         present,     // self test passed on chip 0 (and chip 1 if it answers)
    output wire [2:0]  fst_o,       // frame engine state (debug)

    output reg  [1:0]  ce_n,
    output reg         sclk,
    output reg  [3:0]  sio_o,
    output reg         sio_oe,      // 1 = drive SIO
    input  wire [3:0]  sio_i
);
    initial begin
        ce_n = 2'b11; sclk = 1'b0; sio_o = 4'h0; sio_oe = 1'b0;
        ack = 1'b0; ready = 1'b0; present = 1'b0; rdata = 16'h0; rdata2 = 16'h0;
    end

    // ---- frame engine ----
    // out_sr: up to 16 nibbles (cmd 2 + addr 6 + data 4 = 12 used); n_out of them
    // are shifted out MSB-nibble first.  In SPI-1 mode one bit per cycle goes
    // out on SIO0 (n_out counts bits, out_sr[63] first).
    reg  [63:0] out_sr;
    reg  [5:0]  n_out, n_wait, n_in;    // cycles of each phase
    reg         spi1;                   // 1-bit SPI frame (init commands)
    reg  [5:0]  cyc;                    // SCLK cycle counter inside the frame
    reg  [31:0] in_sr;
    reg  [3:0]  half;                   // half-period divider
    reg         chip;

    localparam F_IDLE = 3'd0, F_SETUP = 3'd1, F_RUN = 3'd2, F_LAST = 3'd3, F_END = 3'd4, F_GAP = 3'd5;
    reg [2:0]  fst = F_IDLE;
    reg [1:0]  gap;

    // ---- init / self-test sequencer ----
    localparam I_WAIT = 4'd0, I_RSTEN = 4'd1, I_RST = 4'd2, I_RSTWAIT = 4'd3, I_QPI = 4'd4,
               I_NEXT = 4'd5, I_TW0 = 4'd6, I_TR0 = 4'd7, I_TW1 = 4'd8, I_TR1 = 4'd9, I_DONE = 4'd10;
    reg [3:0]  ist = I_WAIT;
    reg [15:0] iwait = 16'd0;
    reg        ichip = 1'b0;
    reg        test0 = 1'b0;

    // a frame request, from the user (ready) or the sequencer (!ready)
    reg        f_go;
    reg        f_we, f_spi1, f_two;
    reg [23:0] f_addr;
    reg [15:0] f_wdata;
    reg [1:0]  f_be;
    reg [7:0]  f_cmd;                   // SPI-1 command byte

    // busy also while a request is being taken (req this clock, f_go next):
    // a second requester that checked busy a clock ago would otherwise be dropped
    assign busy = (fst != F_IDLE) || !ready || f_go || req;
    assign fst_o = fst;

    wire half_tick = (half == DIV / 2 - 1);

    always @(posedge clk) begin
        ack  <= 1'b0;
        f_go <= 1'b0;

        // ---- sequencer ----
        case (ist)
        I_WAIT: begin
            if (iwait != INIT_WAIT) iwait <= iwait + 16'd1;
            else begin iwait <= 16'd0; ist <= I_RSTEN; end
        end
        I_RSTEN:   if (fst == F_IDLE && !f_go) begin f_two <= 1'b0; f_spi1 <= 1'b1; f_cmd <= 8'h66; f_addr <= {ichip, 23'd0}; f_go <= 1'b1; ist <= I_RST; end
        I_RST:     if (fst == F_IDLE && !f_go) begin f_spi1 <= 1'b1; f_cmd <= 8'h99; f_addr <= {ichip, 23'd0}; f_go <= 1'b1; ist <= I_RSTWAIT; end
        I_RSTWAIT: begin                      // tRST: 2 us is plenty
            if (fst == F_IDLE && !f_go) begin
                if (iwait != 16'd200) iwait <= iwait + 16'd1;
                else begin iwait <= 16'd0; ist <= I_QPI; end
            end
        end
        I_QPI:     if (fst == F_IDLE && !f_go) begin f_spi1 <= 1'b1; f_cmd <= 8'h35; f_addr <= {ichip, 23'd0}; f_go <= 1'b1; ist <= I_NEXT; end
        I_NEXT:    if (fst == F_IDLE && !f_go) begin
                       if (!ichip) begin ichip <= 1'b1; ist <= I_RSTEN; end
                       else ist <= I_TW0;
                   end
        I_TW0:     if (fst == F_IDLE && !f_go) begin f_spi1 <= 1'b0; f_we <= 1'b1; f_addr <= 24'h000000; f_wdata <= 16'hA55A; f_be <= 2'b11; f_go <= 1'b1; ist <= I_TR0; end
        I_TR0:     if (fst == F_IDLE && !f_go) begin f_we <= 1'b0; f_addr <= 24'h000000; f_go <= 1'b1; ist <= I_TW1; end
        I_TW1:     if (ack) begin test0 <= (rdata == 16'hA55A); end
                   else if (fst == F_IDLE && !f_go) begin f_we <= 1'b1; f_addr <= 24'h800000; f_wdata <= 16'h5AA5; f_be <= 2'b11; f_go <= 1'b1; ist <= I_TR1; end
        I_TR1:     if (fst == F_IDLE && !f_go) begin f_we <= 1'b0; f_addr <= 24'h800000; f_go <= 1'b1; ist <= I_DONE; end
        I_DONE:    if (ack) begin present <= test0; ready <= 1'b1; end   // chip 1 is optional (reported by the host's own probe)
        default:   ist <= I_WAIT;
        endcase

        // ---- user request ----
        if (ready && req && fst == F_IDLE && !f_go) begin
            f_spi1 <= 1'b0; f_we <= we; f_two <= two && !we; f_addr <= addr; f_wdata <= wdata; f_be <= be;
            f_go <= 1'b1;
        end

        // ---- frame engine ----
        case (fst)
        F_IDLE: if (f_go) begin
            chip <= f_addr[23];
            half <= 4'd0;
            cyc  <= 6'd0;
            sclk <= 1'b0;
            spi1 <= f_spi1;
            if (f_spi1) begin
                out_sr <= {f_cmd, 56'd0};
                n_out  <= 6'd8; n_wait <= 6'd0; n_in <= 6'd0;
            end else if (f_we) begin
                // 38h quad write: cmd, addr, data (1 or 2 bytes)
                if (f_be == 2'b01) begin
                    out_sr <= {8'h38, f_addr + 24'd1, f_wdata[7:0], 24'd0};   // the odd byte
                end else if (f_be == 2'b10) begin
                    out_sr <= {8'h38, f_addr, f_wdata[15:8], 24'd0};
                end else begin
                    out_sr <= {8'h38, f_addr, f_wdata, 16'd0};
                end
                n_out  <= (f_be == 2'b11) ? 6'd12 : 6'd10;
                n_wait <= 6'd0; n_in <= 6'd0;
            end else begin
                // EBh quad read: cmd, addr, WAIT_EB dummies, 4 nibbles in
                out_sr <= {8'hEB, f_addr, 32'd0};
                n_out  <= 6'd8; n_wait <= WAIT_EB[5:0]; n_in <= f_two ? 6'd8 : 6'd4;
            end
            fst <= F_SETUP;
        end

        F_SETUP: begin                      // CE low, first nibble on the pins before the first rising edge
            ce_n[chip]  <= 1'b0;
            ce_n[~chip] <= 1'b1;
            sio_oe <= 1'b1;
            if (spi1) sio_o <= {3'b000, out_sr[63]};
            else      sio_o <= out_sr[63:60];
            out_sr <= spi1 ? {out_sr[62:0], 1'b0} : {out_sr[59:0], 4'd0};
            cyc  <= 6'd1;                   // cycle 0's data is on the pins
            half <= 4'd0;
            fst  <= F_RUN;
        end

        F_RUN: begin
            if (half != DIV / 2 - 1) half <= half + 4'd1;
            else begin
                half <= 4'd0;
                if (!sclk) begin
                    sclk <= 1'b1;           // rising edge: the device samples SIO
                end else begin
                    // falling edge: sample what the device launched a period ago,
                    // then put the next nibble out (or release the lines)
                    sclk <= 1'b0;
                    if (cyc > n_out + n_wait && cyc <= n_out + n_wait + n_in)
                        in_sr <= {in_sr[27:0], sio_i};
                    if (cyc < n_out) begin
                        if (spi1) sio_o <= {3'b000, out_sr[63]};
                        else      sio_o <= out_sr[63:60];
                        out_sr <= spi1 ? {out_sr[62:0], 1'b0} : {out_sr[59:0], 4'd0};
                    end else
                        sio_oe <= 1'b0;
                    if (cyc == n_out + n_wait + n_in) begin
                        // the last rising edge has happened; for a read the last
                        // nibble was just sampled
                        fst <= F_END;
                    end
                    cyc <= cyc + 6'd1;
                end
            end
        end

        F_END: begin
            ce_n   <= 2'b11;
            sio_oe <= 1'b0;
            sclk   <= 1'b0;
            if (n_in == 6'd8) begin rdata <= in_sr[31:16]; rdata2 <= in_sr[15:0]; end   // first word, second word
            else if (n_in != 6'd0) rdata <= in_sr[15:0];
            ack <= 1'b1;
            gap <= 2'd0;
            fst <= F_GAP;
        end

        F_GAP: begin                        // tCPH: CE high between frames
            if (gap != 2'd3) gap <= gap + 2'd1;
            else fst <= F_IDLE;
        end

        default: fst <= F_IDLE;
        endcase
    end

endmodule
