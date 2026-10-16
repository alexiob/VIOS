/* VIOS USB multiplex integration declarations; original VDP API remains intact. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <vios_wire.h>
void vios_bridge_poll();
void vios_bridge_boot();
void vios_usb_console_write(const uint8_t *data,size_t length);

void vios_legacy_hexload_notice();
void vios_boot_health(uint8_t flag);

bool vios_cpu_is_paused();
void vios_unsupported(const char *feature);

void vios_bridge_probe();

bool vios_bridge_init();
bool vios_bridge_start();
void vios_bridge_service_vdp();
unsigned long vios_vdp_wait_running();
void vios_vdp_parser_busy(bool busy);
void vios_vdp_tx_begin();
void vios_vdp_tx_end();

void vios_bridge_handoff();
