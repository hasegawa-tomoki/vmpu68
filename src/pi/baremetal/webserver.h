// SPDX-License-Identifier: MIT
// HTTP management server for the vmpu68 bare-metal firmware.
// Endpoints:
//   GET  /                          update page (browser: pick a .vpk, install)
//   GET  /api/status                JSON status (+ update progress while installing)
//   GET  /api/peers                 LAN peers heard on the UDP 6868 beacon (vhd68/vfd68)
//   GET  /api/bus/read?addr=A       bus read
//   POST /api/put?path=P&off=N[&last=1]   hex-encoded file chunk to SD
//   GET  /api/update[?mode=try][&force=1] validate SD:/update.vpk, then install
//                                   it from the main loop and reboot
//   GET  /api/reboot                reboot (deferred to main loop)
//   GET  /api/tbr                   tryboot reboot (deferred to main loop)
//   GET  /api/emu/reset|regs        emulator control  (WITH_EMU builds)
//   GET  /api/emu/run?cycles=N      run emulator      (WITH_EMU builds)
//   POST /api/emu/load?addr=A       hex chunk into emulator memory
#ifndef _webserver_h
#define _webserver_h

#include <circle/net/httpdaemon.h>
#include "vpk.h"

// deferred actions for the main loop (GetContent must return a response
// before the action is taken): 0 none, 1 reboot, 2 tryboot reboot,
// 3 hang (watchdog test), 5 install SD:/update.vpk
extern volatile int g_WebAction;
void vmpu68_led_locate (unsigned ms);
void vmpu68_apply_settings (int mhz, int wb, int jit, boolean save);   // -1 = leave as is (kernel.cpp)
unsigned vmpu68_mhz_limit (void);
const char *vmpu68_hw_rev (void);
int  vmpu68_sramboot_set (int on, boolean save);   // install/remove the SRAM boot program (X68030-style boot screen); 0 = ok (kernel.cpp)
int  vmpu68_smi_set (int on, boolean save);        // SMI transport on core 2.x (Web UI / VMPU68.X -t): 0 ok, -1 not this board, -2 FPGA did not answer (kernel.cpp)
int  vmpu68_sramboot_state (void);                 // 1 installed and current, 0 absent, 2 another SRAM program, -1 no bus   // board revision for the LAN ("1.0", "2.1"; vmpu68.cfg hw= overrides) (kernel.cpp)   // status LED: every colour blinks 4 Hz for ms ("find me")

// package install: parameters set by the requester, progress published by
// the installer (kernel.cpp) and reported by /api/status
struct TUpdateState
{
    volatile int      busy;            // installer running: hardware endpoints refuse
    int               try_mode;        // install as try kernel + tryboot
    int               force;           // rewrite kernel + FPGA even if unchanged
    const char       *phase;           // "check", "fpga erase", ..., "reboot", "error", "idle"
    volatile unsigned done, total;     // progress within the phase
    char              msg[96];         // last result / error text
};
extern TUpdateState g_Update;

#define UPDATE_FILE     "SD:/update.vpk"
#define FPGA_SUM_FILE   "SD:/fpga.sum"     // "<sum> <len>" of the bitstream last programmed

// kernel.cpp: parse + checksum a package on the SD; returns FALSE with a
// reason in pMsg.  bFpgaSame is set when the bitstream matches FPGA_SUM_FILE,
// bKernelSame when the kernel matches the installed SD:/kernel8-rpi4.img.
boolean vmpu68_vpk_check (const char *pPath, vpk_hdr_t *pHdr, boolean *pFpgaSame,
                          boolean *pKernelSame, char *pMsg, unsigned nMsg);

class CVmpuWebServer : public CHTTPDaemon
{
public:
    CVmpuWebServer (CNetSubSystem *pNetSubSystem, CSocket *pSocket = 0);
    ~CVmpuWebServer (void);

    THTTPStatus GetContent (const char *pPath, const char *pParams,
                            const char *pFormData, u8 *pBuffer,
                            unsigned *pLength, const char **ppContentType) override;

    CHTTPDaemon *CreateWorker (CNetSubSystem *pNetSubSystem, CSocket *pSocket) override;

private:
    CNetSubSystem *m_pNet;
};

#endif
