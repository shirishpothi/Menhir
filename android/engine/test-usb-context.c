/* Test the shim without a guest: completions belong to their libusb context. */
#include "../../host/libusb_shim.c"
#include <assert.h>

int hv_vusb_online(HvVirtioUsb *d, uint32_t *session) { (void)d; if (session) *session = 1; return 1; }
int hv_usb_reset(HvVirtioUsb *d, int ms) { (void)d; (void)ms; return 0; }
int hv_usb_control(HvVirtioUsb *d, uint8_t rt, uint8_t req, uint16_t val, uint16_t ix, void *buf, uint16_t len, int ms) {
    (void)d; (void)rt; (void)req; (void)val; (void)ix; (void)ms;
    memset(buf, 0, len); return len;
}
int hv_vusb_send(HvVirtioUsb *d, uint8_t t, uint8_t ep, const void *buf, uint32_t len) {
    (void)d; (void)t; (void)ep; (void)buf; (void)len; return 0;
}
int hv_vusb_send_in(HvVirtioUsb *d, uint8_t ep, uint32_t len) { (void)d; (void)ep; (void)len; return 0; }
int hv_vusb_recv(HvVirtioUsb *d, uint8_t ep, HvVusbMsg *msg, int ms) {
    (void)d; (void)ep; (void)msg; (void)ms; return 0;
}
static int callbacks;
static void done(struct libusb_transfer *t) { (void)t; callbacks++; }
int main(void) {
    libusb_context *other;
    assert(libusb_init(&other) == 0);
    HvVirtioUsb fake = {0}; hv_libusb_shim_attach(&fake);
    libusb_device **a, **b;
    assert(libusb_get_device_list(NULL, &a) == 1);
    assert(libusb_get_device_list(other, &b) == 1);
    libusb_device_handle *ha, *hb;
    assert(libusb_open(a[0], &ha) == 0 && libusb_open(b[0], &hb) == 0);
    struct libusb_transfer *ta = libusb_alloc_transfer(0), *tb = libusb_alloc_transfer(0);
    libusb_fill_bulk_transfer(ta, ha, 1, NULL, 0, done, NULL, 0);
    libusb_fill_bulk_transfer(tb, hb, 1, NULL, 0, done, NULL, 0);
    /* Deterministic completion order, as if the endpoint worker finished both. */
    complete(xfer_of(ta), LIBUSB_TRANSFER_COMPLETED, 0);
    complete(xfer_of(tb), LIBUSB_TRANSFER_COMPLETED, 0);
    const struct libusb_pollfd **pa = libusb_get_pollfds(NULL), **pb = libusb_get_pollfds(other);
    assert(pa && pb && pa[0]->fd != pb[0]->fd);
    struct timeval tv = {0, 0};
    libusb_handle_events_timeout(other, &tv);
    if (callbacks != 1) { fprintf(stderr, "foreign context dispatched %d callbacks (expected 1)\n", callbacks); return 1; }
    struct pollfd ready = {pa[0]->fd, POLLIN, 0};
    assert(poll(&ready, 1, 0) == 1); /* the foreign loop left our wakeup intact */
    libusb_handle_events_timeout(NULL, &tv);
    assert(callbacks == 2);
    libusb_free_pollfds(pa); libusb_free_pollfds(pb);
    libusb_free_transfer(ta); libusb_free_transfer(tb);
    libusb_close(ha); libusb_close(hb);
    libusb_free_device_list(a, 1); libusb_free_device_list(b, 1);
    libusb_exit(other);
    puts("libusb context completion isolation: passed");
    return 0;
}
