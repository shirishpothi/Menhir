/* Compiles the functions of a metallib to SPIR-V the way the Vulkan backend does (devices/air/air_spirv.c), for
   checking the shader route on the Mac and on the device.

     aircc [-n] [-o DIR] LIBRARY.metallib...

   -n prints each function's NIR; -o writes each SPIR-V module as DIR/<function>.spv. Exit status: the number of
   functions that failed. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "air_library.h"
#include "air_spirv.h"

static uint8_t *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc((size_t)n);
    if (fread(d, 1, (size_t)n, f) != (size_t)n) { free(d); d = NULL; }
    fclose(f);
    *len = (size_t)n;
    return d;
}

int main(int argc, char **argv) {
    int print_nir = 0, failed = 0, done = 0;
    setvbuf(stdout, NULL, _IOLBF, 0);
    const char *out_dir = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-n")) { print_nir = 1; continue; }
        if (!strcmp(argv[i], "-o") && i + 1 < argc) { out_dir = argv[++i]; continue; }
        size_t len;
        uint8_t *lib = slurp(argv[i], &len);
        AirFunction fn[64];
        int n = lib ? air_library_functions(lib, len, fn, 64) : -1;
        if (n < 0) { printf("%s: not a metallib\n", argv[i]); failed++; free(lib); continue; }
        for (int k = 0; k < n && k < 64; k++) {
            AirSpirv s;
            /* A fragment function links with the library's first vertex function, if it has one. */
            AirSpirvOptions o = {.print_nir = print_nir, .workgroup = {64, 1, 1}};
            for (int v = 0; fn[k].type == AIR_FRAGMENT && v < n && v < 64; v++)
                if (fn[v].type == AIR_VERTEX) {
                    o.link_bitcode = fn[v].bitcode, o.link_size = fn[v].bitcode_size, o.link_entry = fn[v].name;
                    break;
                }
            int rc = air_compile_spirv(fn[k].bitcode, fn[k].bitcode_size, fn[k].name, &o, &s);
            done++;
            if (rc) {
                printf("%s %s: FAILED: %s\n", argv[i], fn[k].name, s.error);
                failed++;
                continue;
            }
            printf("%s %s: %zu SPIR-V words\n", argv[i], fn[k].name, s.count);
            if (out_dir) {
                char path[1024];
                snprintf(path, sizeof path, "%s/%s.spv", out_dir, fn[k].name);
                FILE *f = fopen(path, "wb");
                if (f) fwrite(s.words, 4, s.count, f), fclose(f);
            }
            air_spirv_free(&s);
        }
        free(lib);
    }
    printf("aircc: %d functions, %d failed\n", done, failed);
    return failed;
}
