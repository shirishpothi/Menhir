/* Force the restore stack and USB shim into a real executable without a VM.
 * Only the engine's virtual USB transport is substituted. */
#include "install.h"
#include "usb_host.h"
#include <stdio.h>
int hv_usb_reset(HvVirtioUsb *d,int t) { (void)d;(void)t;return -1; }
int hv_usb_control(HvVirtioUsb *d,uint8_t a,uint8_t b,uint16_t c,uint16_t e,void *p,uint16_t n,int t) { (void)d;(void)a;(void)b;(void)c;(void)e;(void)p;(void)n;(void)t;return -1; }
int hv_vusb_online(HvVirtioUsb *d,uint32_t *s) { (void)d;(void)s;return 0; }
int hv_vusb_send(HvVirtioUsb *d,uint8_t t,uint8_t ep,const void *p,uint32_t n) { (void)d;(void)t;(void)ep;(void)p;(void)n;return -1; }
int hv_vusb_send_in(HvVirtioUsb *d,uint8_t ep,uint32_t n) { (void)d;(void)ep;(void)n;return -1; }
int hv_vusb_recv(HvVirtioUsb *d,uint8_t ep,HvVusbMsg *m,int t) { (void)d;(void)ep;(void)m;(void)t;return -1; }
int main(void) {
 /* Calling with no USB returns before starting any daemon, but retains all
  * restore references in install.o and the transitive restore archives. */
 int r=hv_install_start(NULL,"","",0,NULL,NULL);
 printf("restore stack and virtual libusb linked; null USB rejected: %d\n",r);
 return r==-1?0:1;
}
