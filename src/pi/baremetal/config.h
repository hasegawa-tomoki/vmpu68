// SPDX-License-Identifier: MIT
// SD:/vmpu68.cfg — "key=value" per line, same shape as vhd68.cfg / vfd68.cfg.
// Keys: name=<display name>  (shown in the Web UI header, /api/status and the
// discovery beacon); ram=<MB> main RAM size when the bus probe must not be
// trusted (0 / absent = probe at every machine boot); mhz=<n> emulated clock
// limit at boot (0 / absent = unlimited, same as the console's espd); wb=<0|1>
// main RAM write-back (absent = 1); jit=<0|1> execution by translation (absent = 0).
// Wi-Fi credentials stay in
// SD:/wpa_supplicant.conf.  mhz=, wb= and jit= are rewritten by cfg_save() when
// changed from the Web UI or VMPU68.X; other lines are kept.
#pragma once

#define CFG_FILE        "SD:/vmpu68.cfg"
#define CFG_NAME_MAX    32

void cfg_load (void);                   // (re)read the file; missing file = defaults
const char *cfg_name (void);            // "" when not configured
unsigned cfg_ram_mb (void);             // 0 = auto (probe)
unsigned cfg_mhz (void);                // 0 = unlimited
unsigned cfg_wb (void);                 // main RAM write-back (1, default) or write-through (0)
unsigned cfg_jit (void);                // jit=<0|1> 68000->AArch64 translation (0, default)
unsigned cfg_sramboot (void);           // sramboot=<0|1> X68030-style boot screen from the SRAM boot program (0, default)
void cfg_set_sramboot (unsigned on);
const char *cfg_hw (void);              // hw=<rev> board revision string ("" = derive from the board profile)
unsigned cfg_board (void);              // board=<1|2> GPIO layout; 0 = auto (probe the FPGA with both layouts)
void cfg_set_board (unsigned id);       // 0 = auto; persisted by cfg_save()
void cfg_set (unsigned mhz, unsigned wb, unsigned jit);   // change the runtime settings (in memory)
void cfg_set_name (const char *v);      // display name (31 bytes max; persisted by cfg_save())
void cfg_set_ram (unsigned mb);         // ram=<MB> fixed main RAM size, 0 = probe; takes effect at the next boot
int  cfg_save (void);                   // rewrite SD:/vmpu68.cfg with the known keys; 0 = ok

// SD:/vmpu68-bus.json — bus timing per bus-clock class.  {"bus":[{...},...]}
// with min_mhz (the class applies from this clock upward, 0.1 MHz steps),
// wr_setup (0-3 or "auto": probed at boot), wait_ns (first STATUS poll after
// a command), io_mhz (I/O access spacing of the 68000 being emulated), ai
// (address-auto-update writes).  Missing file or no valid class = built-in
// defaults for 10 and 16 MHz.
#define CFG_BUS_FILE    "SD:/vmpu68-bus.json"
#define CFG_BUS_MAX     8
struct cfg_bus_class
{
    unsigned min_mhz10;                 // 165 = 16.5 MHz
    int      wr_setup;                  // -1 = auto
    unsigned wait_ns;
    unsigned io_mhz;
    int      ai;
};
void cfg_bus_load (void);               // (re)read the file; defaults when absent/invalid
unsigned cfg_bus_count (void);
const cfg_bus_class *cfg_bus_class_at (unsigned i);
const cfg_bus_class *cfg_bus_select (unsigned mhz10);   // highest min_mhz <= mhz10 (never NULL)
const char *cfg_bus_source (void);      // "built-in" or the file name
