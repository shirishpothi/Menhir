#ifndef MENHIR_AIR_LIBRARY_H
#define MENHIR_AIR_LIBRARY_H

#include <stddef.h>
#include <stdint.h>

/* The functions of a metallib (the container is described in devices/metallib.c): each one's name, stage and AIR
   bitcode, pointing into the library's bytes. */

enum { AIR_VERTEX = 0, AIR_FRAGMENT = 1, AIR_KERNEL = 2, AIR_UNQUALIFIED = 3, AIR_VISIBLE = 4, AIR_EXTERN = 5,
       AIR_INTERSECTION = 6, AIR_MESH = 7, AIR_OBJECT = 8, AIR_TILE = 9 }; /* the TYPE tag's values */

typedef struct {
    char name[256];
    int type;               /* AIR_*, -1 if the library did not say */
    const uint8_t *bitcode; /* LLVM bitcode, out of its 0x0B17C0DE wrapper */
    size_t bitcode_size;
} AirFunction;

/* Fills up to max functions; returns how many the library has, or -1 if it is not a library this reads. */
int air_library_functions(const uint8_t *lib, size_t len, AirFunction *out, int max);

#endif
