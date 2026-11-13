/* AAudio output for virtio-snd. Same single-device lifecycle as audio_mac.c. */
#include "audio_mac.h"
#include <aaudio/AAudio.h>
static AAudioStream *stream;
static unsigned current_channels,current_format,current_rate;
static aaudio_data_callback_result_t render(AAudioStream *s,void *opaque,void *data,int32_t frames) {
    (void)s;
    hv_virtio_snd_pull(opaque,data,(uint32_t)frames*current_channels*(current_format==HV_VSND_FMT_S16?2:4));
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}
void hv_audio_detach(void) {
    if(stream) { AAudioStream_requestStop(stream); AAudioStream_close(stream);stream=NULL; }
}
static int start(void *opaque,unsigned channels,unsigned format,unsigned rate) {
    if(stream && (channels!=current_channels||format!=current_format||rate!=current_rate)) hv_audio_detach();
    if(!stream) {
        if(!channels||!rate||(format!=HV_VSND_FMT_S16&&format!=HV_VSND_FMT_FLOAT))return -1;
        AAudioStreamBuilder *b=NULL;
        if(AAudio_createStreamBuilder(&b)!=AAUDIO_OK)return -1;
        AAudioStreamBuilder_setDirection(b,AAUDIO_DIRECTION_OUTPUT);
        AAudioStreamBuilder_setSharingMode(b,AAUDIO_SHARING_MODE_SHARED);
        AAudioStreamBuilder_setChannelCount(b,(int32_t)channels);
        AAudioStreamBuilder_setSampleRate(b,(int32_t)rate);
        AAudioStreamBuilder_setFormat(b,format==HV_VSND_FMT_S16?AAUDIO_FORMAT_PCM_I16:AAUDIO_FORMAT_PCM_FLOAT);
        AAudioStreamBuilder_setDataCallback(b,render,opaque);
        current_channels=channels;current_format=format;current_rate=rate;
        aaudio_result_t r=AAudioStreamBuilder_openStream(b,&stream);
        AAudioStreamBuilder_delete(b);
        if(r!=AAUDIO_OK) { stream=NULL;return -1; }
    }
    return AAudioStream_requestStart(stream)==AAUDIO_OK?0:-1;
}
static void stop(void *opaque) {
    (void)opaque;
    /* Close joins AAudio's callback thread before the device ring is reset. */
    hv_audio_detach();
}
void hv_audio_attach(HvVirtioSnd *d) { d->backend=(HvVirtioSndBackend){start,stop,d}; }
