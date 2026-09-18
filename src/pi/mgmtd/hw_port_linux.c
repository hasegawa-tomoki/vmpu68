/* SPDX-License-Identifier: MIT */
#include <pthread.h>
#include <unistd.h>
#include <stdio.h>
#include "hw_port.h"

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;

void hw_port_lock(void)   { pthread_mutex_lock(&mu); }
void hw_port_unlock(void) { pthread_mutex_unlock(&mu); }
int  hw_port_lock_held(void) { if (pthread_mutex_trylock(&mu) == 0) { pthread_mutex_unlock(&mu); return 0; } return 1; }   /* debug */
void hw_port_usleep(unsigned us) { usleep(us); }
void hw_port_log(const char *msg) { fprintf(stderr, "%s\n", msg); }
