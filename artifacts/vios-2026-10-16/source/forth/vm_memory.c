/* Byte-addressed, little-endian VM memory. Host pointers never become cells. */
#include "forth/vm.h"

#ifndef __ez80__
struct region { fcell base; size_t length; unsigned char *data; };
static struct region regions[8];
static unsigned region_count;
#endif

void fvm_memory_reset(void) {
#ifndef __ez80__
    region_count = 0;
#endif
}

int fvm_region(fcell base, size_t length, unsigned char *data) {
#ifdef __ez80__
    (void)base; (void)length; (void)data;
    return 1;
#else
    unsigned i;
    if (!data || !length || base > FVM_MASK || length > 0x1000000UL - base ||
        region_count == 8) {
        fvm_error = FE_MEMORY; return 0;
    }
    for (i = 0; i < region_count; ++i) {
        const struct region *r = &regions[i];
        if (base < r->base + r->length && r->base < base + length) {
            fvm_error = FE_MEMORY; return 0;
        }
    }
    regions[region_count].base = base;
    regions[region_count].length = length;
    regions[region_count++].data = data;
    return 1;
#endif
}

fcell fvm_address(const void *pointer) {
#ifdef __ez80__
    return (fcell)(uintptr_t)pointer;
#else
    unsigned i;
    uintptr_t p = (uintptr_t)pointer;
    for (i = 0; i < region_count; ++i) {
        const struct region *r = &regions[i];
        uintptr_t start = (uintptr_t)r->data;
        if (p >= start && p - start < r->length)
            return r->base + (fcell)(p - start);
    }
    fvm_error = FE_MEMORY;
    return 0;
#endif
}

/* Return validity separately: physical address zero is not an error sentinel. */
static int address(fcell a, unsigned width, volatile unsigned char **result) {
    if (a > FVM_MASK || a > FVM_MASK - (width - 1u)) {
        fvm_error = FE_MEMORY; return 0;
    }
#ifdef __ez80__
    *result = (volatile unsigned char *)(uintptr_t)a;
    return 1;
#else
    unsigned i;
    for (i = 0; i < region_count; ++i) {
        const struct region *r = &regions[i];
        if (a >= r->base && (size_t)(a - r->base) < r->length &&
            width <= r->length - (size_t)(a - r->base)) {
            *result = r->data + (size_t)(a - r->base);
            return 1;
        }
    }
    fvm_error = FE_MEMORY;
    return 0;
#endif
}

unsigned char fvm_read8(fcell a) {
    volatile unsigned char *p;
    return address(a, 1, &p) ? *p : 0;
}
void fvm_write8(fcell a, unsigned char value) {
    volatile unsigned char *p;
    if (address(a, 1, &p)) *p = value;
}
fcell fvm_read_cell(fcell a) {
    volatile unsigned char *p;
    if (!address(a, 3, &p)) return 0;
    return (fcell)p[0] | ((fcell)p[1] << 8) | ((fcell)p[2] << 16);
}
void fvm_write_cell(fcell a, fcell value) {
    volatile unsigned char *p;
    if (address(a, 3, &p)) {
        p[0] = (unsigned char)value;
        p[1] = (unsigned char)(value >> 8);
        p[2] = (unsigned char)(value >> 16);
    }
}
