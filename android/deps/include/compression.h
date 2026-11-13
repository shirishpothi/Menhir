/* Android subset of Apple's documented buffer API, for snapshot.c. */
#ifndef MENHIR_COMPRESSION_H
#define MENHIR_COMPRESSION_H
#include <stddef.h>
#include <stdint.h>
typedef enum { COMPRESSION_LZ4=0x100 } compression_algorithm;
size_t compression_encode_scratch_buffer_size(compression_algorithm algorithm);
size_t compression_decode_scratch_buffer_size(compression_algorithm algorithm);
size_t compression_encode_buffer(uint8_t *dst,size_t dst_size,const uint8_t *src,size_t src_size,void *scratch,compression_algorithm algorithm);
size_t compression_decode_buffer(uint8_t *dst,size_t dst_size,const uint8_t *src,size_t src_size,void *scratch,compression_algorithm algorithm);
#endif
