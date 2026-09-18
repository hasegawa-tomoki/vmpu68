// SPDX-License-Identifier: MIT
//
// vmpu68 bare-metal firmware.
// Console on UART + USB CDC gadget, WLAN + HTTP status server,
// firmware self-update and file upload over the console.
//
// NOTE: linking against circle makes the distributed binary GPLv3
// (see LICENSE); this source file itself remains MIT.
#ifndef _kernel_h
#define _kernel_h

#include <circle/actled.h>
#include <circle/koptions.h>
#include <circle/devicenameservice.h>
#include <circle/serial.h>
#include <circle/exceptionhandler.h>
#include <circle/interrupt.h>
#include <circle/timer.h>
#include <circle/logger.h>
#include <circle/sched/scheduler.h>
#include <circle/bcmwatchdog.h>
#include <circle/cputhrottle.h>
#include <circle/usb/gadget/usbcdcgadget.h>
#include <circle/multicore.h>
#include <SDCard/emmc.h>
#include <fatfs/ff.h>
#include <wlan/bcm4343.h>
#include <wlan/hostap/wpa_supplicant/wpasupplicant.h>
#include <circle/net/netsubsystem.h>
#include <circle/types.h>

enum TShutdownMode
{
    ShutdownNone,
    ShutdownHalt,
    ShutdownReboot
};

// core 1 runs the 68000 emulator; cores 2/3 idle
class CEmuCore : public CMultiCoreSupport
{
public:
    CEmuCore (CMemorySystem *pMemorySystem) : CMultiCoreSupport (pMemorySystem) {}
    void Run (unsigned nCore) override;
};

class CKernel
{
public:
    CKernel (void);
    ~CKernel (void);

    boolean Initialize (void);
    TShutdownMode Run (void);

    // keep watchdog / USB / network alive while the main loop is busy
    // (package install); must be called at least every ~20 ms
    void ServiceDuringUpdate (void);
    void ActLedOn (void)  { m_ActLED.On (); }     // FPGA flash access indicator (act_fast_tick)
    void ActLedOff (void) { m_ActLED.Off (); }

private:
    void FileReceive (CDevice *pDev, const char *pPath, unsigned nLength);
    void FpgaFlash (CDevice *pDev, const char *pPath);
    boolean FpgaFlashImage (CDevice *pDev, const u8 *pImage, unsigned nLength);
    int     FpgaFlashCompare (CDevice *pDev, const u8 *img, unsigned len);   // 0 same, 1 differs, -1 no chip
    void FpgaDiag (CDevice *pDev);
    boolean Update (CDevice *pDev, const char *pPath, boolean bTry, boolean bForce);
    void RunUpdate (CDevice *pDev, const char *pPath);
    void YieldTest (CDevice *pDev, unsigned nUs, unsigned nSecs);
    void StartWLAN (void);
    void BootX68 (const char *pWhy, unsigned nStatus, boolean bPowerOn);

private:
    CActLED             m_ActLED;
    CKernelOptions      m_Options;
    CDeviceNameService  m_DeviceNameService;
    CSerialDevice      *m_pSerial;             // console UART: 0 (GPIO14/15, core 1.x) or 2 (GPIO0/1, core 2.x), chosen in Initialize
    CExceptionHandler   m_ExceptionHandler;
    CInterruptSystem    m_Interrupt;
    CTimer              m_Timer;
    CLogger             m_Logger;
    CScheduler          m_Scheduler;
    CBcmWatchdog        m_Watchdog;
    CCPUThrottle        m_CPUThrottle;
    CUSBCDCGadget       m_CDCGadget;
    CEMMCDevice         m_EMMC;
    FATFS               m_FileSystem;
    CBcm4343Device      m_WLAN;
    CNetSubSystem       m_Net;
    CWPASupplicant      m_WPASupplicant;
    CEmuCore            m_EmuCore;

    boolean             m_bNetOK;
    boolean             m_bWebStarted;
    boolean             m_bWLANStarted;
};

#endif
