#ifndef VIOS_CONFIG_H
#define VIOS_CONFIG_H
/* Agon resource budgets. Ports may override these at compile time. Neither
 * capacity changes native instruction encoding or compiled call dispatch. */
#ifndef VIOS_FORTH_WORDS_MAX
#define VIOS_FORTH_WORDS_MAX 512
#endif
#ifndef VIOS_FORTH_HASH_BUCKETS
#define VIOS_FORTH_HASH_BUCKETS 128
#endif
#if VIOS_FORTH_WORDS_MAX < 64 || VIOS_FORTH_WORDS_MAX > 65535
#error "Dictionary capacity must fit 16-bit links and the bootstrap vocabulary"
#endif
#if VIOS_FORTH_HASH_BUCKETS < 1 || (VIOS_FORTH_HASH_BUCKETS & (VIOS_FORTH_HASH_BUCKETS - 1))
#error "Dictionary hash bucket count must be a power of two"
#endif
#endif
