/* Minimal headless libretro host: run a core N frames, dump video frames as
 * PPM, report audio energy, and optionally poke joypad input at given frames. */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

typedef void (*rcb)(void);
static void (*p_init)(void), (*p_deinit)(void), (*p_run)(void);
static bool (*p_load)(const void*);
static void (*p_set_env)(void*), (*p_set_video)(void*), (*p_set_audio)(void*),
            (*p_set_audio_batch)(void*), (*p_set_input_poll)(void*), (*p_set_input_state)(void*);
static void (*p_get_avinfo)(void*);

static unsigned frame_no, dump_at, dump_at2;
static uint64_t audio_energy; static unsigned audio_samples;
static unsigned pixfmt = 0; /* 0=0RGB1555 1=XRGB8888 2=RGB565 */
static int16_t pad_state = 0; static unsigned press_frame = 0, press_mask = 0;
static unsigned press2_frame = 0, press2_mask = 0;

static void log_shim(int level, const char *fmt, ...) { (void)level; (void)fmt; }
struct retro_log_iface { void (*log)(int, const char*, ...); };

struct retro_variable { const char *key; const char *value; };

static bool env_cb(unsigned cmd, void *data) {
    cmd &= 0xFFFF;  /* strip experimental flag */
    if (cmd == 15) {                                /* GET_VARIABLE */
        struct retro_variable *v = data;
        if (v->key && !strcmp(v->key, "picodrive_input1")) { v->value = "6 button pad"; return true; }
        return false;
    }
    if (cmd == 10) { pixfmt = *(unsigned*)data; return true; }     /* SET_PIXEL_FORMAT */
    if (cmd == 27) { ((struct retro_log_iface*)data)->log = log_shim; return true; }
    if (cmd == 3) { *(bool*)data = true; return true; }            /* GET_CAN_DUPE */
    if (cmd == 9) { *(const char**)data = "/tmp"; return true; }   /* SYSTEM_DIR */
    if (cmd == 31) { *(const char**)data = "/tmp"; return true; }  /* SAVE_DIR */
    return false;
}
static void save_ppm(const char *name, const void *data, unsigned w, unsigned h, size_t pitch) {
    FILE *f = fopen(name, "wb");
    fprintf(f, "P6\n%u %u\n255\n", w, h);
    const uint8_t *row = data;
    for (unsigned y = 0; y < h; y++, row += pitch)
        for (unsigned x = 0; x < w; x++) {
            unsigned r,g,b;
            if (pixfmt == 2) { uint16_t p=((uint16_t*)row)[x]; r=(p>>11)<<3; g=((p>>5)&63)<<2; b=(p&31)<<3; }
            else if (pixfmt == 1) { uint32_t p=((uint32_t*)row)[x]; r=(p>>16)&255; g=(p>>8)&255; b=p&255; }
            else { uint16_t p=((uint16_t*)row)[x]; r=((p>>10)&31)<<3; g=((p>>5)&31)<<3; b=(p&31)<<3; }
            fputc(r,f); fputc(g,f); fputc(b,f);
        }
    fclose(f);
}
static void video_cb(const void *data, unsigned w, unsigned h, size_t pitch) {
    if (!data) return;
    if (frame_no == dump_at)  { char n[64]; sprintf(n,"/tmp/frame_%u.ppm",frame_no); save_ppm(n,data,w,h,pitch); }
    if (frame_no == dump_at2) { char n[64]; sprintf(n,"/tmp/frame_%u.ppm",frame_no); save_ppm(n,data,w,h,pitch); }
}
static unsigned meas_from, meas_to;
static double meas_sq; static unsigned long meas_n;
static void note(int16_t l, int16_t r) {
    audio_energy += (l<0?-l:l)+(r<0?-r:r); audio_samples++;
    if (frame_no>=meas_from && frame_no<meas_to) {
        double m = (double)l+(double)r; meas_sq += m*m; meas_n++;
    }
}
static void audio_cb(int16_t l, int16_t r) { note(l,r); }
static size_t audio_batch_cb(const int16_t *d, size_t frames) {
    for (size_t i=0;i<frames;i++) note(d[2*i],d[2*i+1]);
    return frames;
}
static void input_poll_cb(void) {}
static int16_t input_state_cb(unsigned port, unsigned dev, unsigned idx, unsigned id) {
    static int logged = 0;
    if (logged < 8 && frame_no > 100) { fprintf(stderr,"input? port=%u dev=%u idx=%u id=%u\n",port,dev,idx,id); logged++; }
    if (id==0x100 || dev==0) return 0;
    if (port==0 && press_mask && frame_no>=press_frame && frame_no<press_frame+8)
        return (press_mask>>id)&1;
    if (port==0 && press2_mask && frame_no>=press2_frame && frame_no<press2_frame+8)
        return (press2_mask>>id)&1;
    return 0;
}

struct retro_game_info { const char *path; const void *data; size_t size; const char *meta; };

int main(int argc, char **argv) {
    const char *so = argv[1], *rom = argv[2];
    unsigned frames = atoi(argv[3]);
    dump_at = atoi(argv[4]); dump_at2 = argc>5?atoi(argv[5]):0;
    if (argc>7) { press_frame=atoi(argv[6]); press_mask=atoi(argv[7]); }
    if (argc>9) { meas_from=atoi(argv[8]); meas_to=atoi(argv[9]); }
    if (argc>11) { press2_frame=atoi(argv[10]); press2_mask=atoi(argv[11]); }
    void *h = dlopen(so, RTLD_NOW);
    if (!h) { fprintf(stderr,"dlopen: %s\n",dlerror()); return 1; }
    p_init=dlsym(h,"retro_init"); p_deinit=dlsym(h,"retro_deinit"); p_run=dlsym(h,"retro_run");
    p_load=dlsym(h,"retro_load_game");
    p_set_env=dlsym(h,"retro_set_environment"); p_set_video=dlsym(h,"retro_set_video_refresh");
    p_set_audio=dlsym(h,"retro_set_audio_sample"); p_set_audio_batch=dlsym(h,"retro_set_audio_sample_batch");
    p_set_input_poll=dlsym(h,"retro_set_input_poll"); p_set_input_state=dlsym(h,"retro_set_input_state");
    p_set_env(env_cb); p_init();
    p_set_video(video_cb); p_set_audio(audio_cb); p_set_audio_batch(audio_batch_cb);
    p_set_input_poll(input_poll_cb); p_set_input_state(input_state_cb);
    FILE *f=fopen(rom,"rb"); fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
    void *buf=malloc(sz); if (fread(buf,1,sz,f)!=(size_t)sz) return 1; fclose(f);
    struct retro_game_info gi={rom,buf,(size_t)sz,NULL};
    if (!p_load(&gi)) { fprintf(stderr,"load_game failed\n"); return 1; }
    for (frame_no=0; frame_no<frames; frame_no++) {
        uint64_t before=audio_energy;
        p_run();
        if (frame_no%120==0)
            printf("frame %4u  audio-energy/frame %llu\n", frame_no,
                   (unsigned long long)(audio_energy-before));
    }
    printf("done: %u frames, %u audio samples, avg |amp| %.1f\n",
           frames, audio_samples, audio_samples?(double)audio_energy/audio_samples/2:0);
    if (meas_n) {
        double rms = meas_sq/meas_n; unsigned long i; double r=1;
        /* integer-free sqrt via Newton */
        for (i=0;i<60;i++) r = 0.5*(r+rms/r);
        printf("MEAS frames %u..%u  RMS %.2f\n", meas_from, meas_to, r);
    }
    p_deinit();
    return 0;
}
