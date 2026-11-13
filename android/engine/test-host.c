/* The upstream tests use host addresses as 48-bit guest virtual addresses.
   Android heap tags are host metadata, not part of those guest addresses. */
#include <malloc.h>
__attribute__((constructor)) static void test_heap_addresses(void) {
    mallopt(M_BIONIC_SET_HEAP_TAGGING_LEVEL, 0);
}
