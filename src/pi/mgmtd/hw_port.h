/* SPDX-License-Identifier: MIT
 * Platform hooks for the hw layer.
 *   Linux:  hw_port_linux.c  (pthread mutex, usleep, stderr)
 *   circle: sw/baremetal/hw_port_circle.cpp (spinlock, CTimer, CLogger)
 */
#ifndef VMPU68_HW_PORT_H
#define VMPU68_HW_PORT_H

#ifdef __cplusplus
extern "C" {
#endif

void hw_port_lock(void);
void hw_port_unlock(void);
void hw_port_usleep(unsigned us);
void hw_port_log(const char *msg);

#ifdef __cplusplus
}
#endif

#endif
