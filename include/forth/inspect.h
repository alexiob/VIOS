#ifndef VIOS_FORTH_INSPECT_H
#define VIOS_FORTH_INSPECT_H
#include "forth/system.h"
enum forth_word_kind {
 FORTH_WORD_PRIMITIVE=0, FORTH_WORD_COLON=1, FORTH_WORD_CONSTANT=2,
 FORTH_WORD_VARIABLE=3, FORTH_WORD_DEFER=4, FORTH_WORD_NATIVE=5,
 FORTH_WORD_DOES=6, FORTH_WORD_MARKER=7   /* a DOES> child; a MARKER */
};
struct forth_word_info {
 const char *name,*source,*module,*module_version,*target_name;
 fcell xt,token_address,direct_address,value,target_xt;
 unsigned kind,origin,source_length,token_bytes,direct_bytes;
 uint32_t module_crc32;
 int value_valid,immediate;
};
struct forth_resources {
 unsigned words,words_max;
 size_t token_bytes,token_capacity,direct_bytes,direct_capacity;
 size_t data_bytes,data_capacity,source_bytes,source_capacity;
 enum forth_engine engine;
};
/* Word indices/xt and borrowed pointers are valid within this boot/revision.
 * Word source is canonical compiler source and is not NUL terminated. Module
 * source editing uses forth_module_source() to retain exact original text. */
uint32_t forth_catalog_revision(void);
unsigned forth_word_count(void);
int forth_inspect_word(unsigned zero_based_index,struct forth_word_info *out);
void forth_inspect_resources(struct forth_resources *out);
/* Synchronous idle observer; error receives the observer failure while the
 * caller's application stack, BASE, engine and VM diagnostic state persist.
 * Callbacks are trusted native services; their device/data effects persist.
 * A successful hook leaves its temporary data stack empty. */
int forth_observe(const char *name,fcell budget,char *error,size_t capacity);
#endif
