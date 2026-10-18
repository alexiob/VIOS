#ifndef VIOS_RUNTIME_H
#define VIOS_RUNTIME_H
#include <stddef.h>
#include <stdint.h>
#include "forth/modules.h"
int vios_runtime_init(void);
int vios_services_init(void);
int vios_prelude_init(void); /* core library words in Forth (kernel/vios_prelude.c) */
/* Handles, /dev endpoints and pipes (kernel/vios_handles.c). */
int vios_handles_init(void);
void vios_handles_report(void);
int vios_load_source(const char *path);
int vios_module_command(const char *word,const char **rest);
int vios_boot_command(const char *word,const char **rest);
int vios_module_save(const char *name,const char *path);
int vios_module_load(const char *path);
int vios_module_path(const char *name,char *out,size_t capacity);
int vios_boot_startup(void);
int vios_boot_trial(void);
int vios_boot_confirm(void);
int vios_boot_revert(void);
void vios_boot_status(void);
void vios_out(const char *text);
void vios_uint(uint32_t value);
void vios_hex(uint32_t value,unsigned digits);
int vios_arg(const char **rest,char *out,size_t capacity);
int vios_same(const char *a,const char *b);
size_t vios_length(const char *s);
uint32_t vios_crc32(const void *data,size_t length);
void vios_put32(unsigned char *p,uint32_t value);
uint32_t vios_get32(const unsigned char *p);
#endif
