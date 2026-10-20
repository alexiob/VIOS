#ifndef VIOS_FORTH_MODULES_H
#define VIOS_FORTH_MODULES_H
#include "forth/system.h"
#define FORTH_MODULES_MAX 8
#define FORTH_MODULE_SOURCE_MAX 4096
#define FORTH_MODULE_SOURCE_POOL 32768
#define FORTH_MODULE_DEPS_MAX 4
struct forth_module_dependency { const char *name; uint32_t source_crc32; };
struct forth_module_info {
    const char *name,*version,*test_word;
    uint32_t source_crc32;
    unsigned source_length,dependency_count;
    int active,previous;
    unsigned words;
    size_t token_bytes,direct_bytes,data_bytes;
};
/* One pending source, eight names, two tracked generations per name (current
 * and previous); activating again retires the older one, whose code and source
 * stay allocated until restart, so repeated accepts use the pools.
 * Source is exact ASCII (CRC32 is an integrity check, not authentication).
 * Stage uses a decimal, checked reference VM and leaves live state untouched.
 * Optional tests return exactly one true flag; their data writes are discarded.
 * Activation repeats validation/tests against the current live environment.
 * A single latest activation can be rolled back only before later dictionary
 * allocation. Rollback restores all managed data and VM state at that point,
 * including subsequent variable/IS changes, not external device effects.
 * Source archives reconstruct module programs, not arbitrary live RAM images.
 */
void forth_modules_reset(void);
uint32_t forth_module_crc32(const char *source);
/* Raw CRC-32 update (start with 0xffffffff, finish with ~). */
uint32_t forth_crc32_update(uint32_t crc,const unsigned char *p,size_t length);
int forth_module_stage(const char *name,const char *version,
    const struct forth_module_dependency *dependencies,unsigned dependency_count,
    const char *source,const char *test_word);
int forth_module_activate(const char *name);
int forth_module_discard(void);
int forth_module_rollback(const char *name);
unsigned forth_module_count(void);
int forth_module_info(unsigned index,struct forth_module_info *out);
int forth_module_previous_info(const char *name,struct forth_module_info *out);
int forth_module_pending_info(struct forth_module_info *out);
const char *forth_module_pending_source(void);
/* Borrowed until the next pending-module mutation; no VM state changes. */
int forth_module_pending_dependency_at(unsigned index,struct forth_module_dependency *out);
int forth_module_dependency_at(const char *name,int previous,unsigned index,
    struct forth_module_dependency *out);
const char *forth_module_source(const char *name,int previous);
int forth_module_origin(unsigned id,const char **name,const char **version,uint32_t *crc);
/* Reference-memory policy; enabled only during synchronous module staging. */
int fvm_overlay_begin(fcell base,size_t length,unsigned char *data);
void fvm_overlay_copy(unsigned char *out);
void fvm_overlay_restore(const unsigned char *data);
void fvm_overlay_commit(void);
void fvm_overlay_discard(void);
#endif
