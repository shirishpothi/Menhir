#include "audio_mac.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
uint32_t hv_virtio_snd_pull(HvVirtioSnd *d,void *out,uint32_t bytes) { (void)d;memset(out,0,bytes);return bytes; }
int main(void) {
 HvVirtioSnd d={0};hv_audio_attach(&d);
 int r=d.backend.start(d.backend.opaque,2,HV_VSND_FMT_S16,48000);
 printf("AAudio stream start: %d\n",r);if(!r)usleep(100000);
 hv_audio_detach();return r?1:0;
}
