#ifndef VIOS_PLATFORM_VDP_H
#define VIOS_PLATFORM_VDP_H
#include <stddef.h>
#include <stdint.h>
void platform_console_flush(void);
void platform_vdp_send(const void *data,size_t length);
int platform_vdp_request(const void *command,size_t length,unsigned char reply,void *data,size_t capacity,unsigned *actual);
uint32_t platform_millis(void);
unsigned platform_console_dropped(void);
/* Host-service request/reply over USB channel 4 (resident board). */
int platform_host_request(const void *request,size_t length,void *reply,size_t capacity,unsigned *actual);
void platform_boot_ready(void);
void platform_boot_trial(void);
/* Counts ESP restarts seen while running (packet FA): the display server's
 * state (windows, surfaces) is gone after each; callers that own some redo it. */
unsigned platform_vdp_epoch(void);
/* 1 once per display-server nudge (packet F0: events are waiting); no traffic. */
int platform_vdp_nudged(void);
#endif
