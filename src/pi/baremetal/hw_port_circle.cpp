// SPDX-License-Identifier: MIT
// circle implementations of the hw layer platform hooks.
#include <circle/spinlock.h>
#include <circle/timer.h>
#include <circle/logger.h>
#include "../mgmtd/hw_port.h"

static CSpinLock s_Lock (TASK_LEVEL);

extern "C" void hw_port_lock (void)   { s_Lock.Acquire (); }
extern "C" void hw_port_unlock (void) { s_Lock.Release (); }

extern "C" void hw_port_usleep (unsigned us)
{
    CTimer::SimpleusDelay (us);
}

extern "C" void hw_port_log (const char *msg)
{
    CLogger::Get ()->Write ("hw", LogNotice, "%s", msg);
}
