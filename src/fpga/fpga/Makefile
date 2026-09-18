# vmpu68 FPGA build + simulation
RTL   = ../rtl/vmpu68_top.v ../rtl/pi_if.v ../rtl/bus_engine.v ../rtl/e_clock.v ../rtl/fwft_fifo.v
TB    = ../sim/tb_top.v ../sim/sb_pll40_model.v
TOP   = vmpu68_top
PCF   = vmpu68.pcf
DEV   = --hx4k
PKG   = tq144

sim: tb_top.vvp
	vvp tb_top.vvp

tb_top.vvp: $(RTL) $(TB)
	iverilog -g2012 -o $@ $(TB) $(RTL)

bit: $(TOP).bin

$(TOP).json: $(RTL)
	yosys -q -p "synth_ice40 -top $(TOP) -json $@" $(RTL)

$(TOP).asc: $(TOP).json $(PCF)
	nextpnr-ice40 $(DEV) --package $(PKG) --json $(TOP).json --pcf $(PCF) --asc $@ --freq 64

$(TOP).bin: $(TOP).asc
	icepack $< $@

clean:
	rm -f *.vvp *.vcd *.json *.asc *.bin

.PHONY: sim bit clean
