#include <furi.h>
#include <gui/gui.h>
#include <gui/view_port.h>
#include <input/input.h>
#include <notification/notification.h>
#include <notification/notification_messages.h>
#include <subghz/devices/types.h>
#include <subghz/devices/registry.h>

#define MIN_RSSI_DBM       -80.0f
#define CAL_DELAY_MS       5
#define THRESH_DB          8
#define SLOT_MS            14
#define FRAME_MS           57

#define UP_START_FREQ      380000000U
#define UP_END_FREQ        385000000U
#define FREQ_STEP          25000U
#define UP_NUM_CH          (((UP_END_FREQ - UP_START_FREQ) / FREQ_STEP) + 1)

static const uint32_t STATIC_FREQS[] = {
    389540000U, 388790000U, 389170000U,
    380450000U, 380425000U, 380400000U,
    379650000U, 380500000U, 379625000U,
    380375000U, 379375000U, 380325000U,
    380300000U, 379300000U, 380025000U,
    384437500U, 384712500U
};
#define STATIC_CH (sizeof(STATIC_FREQS)/sizeof(STATIC_FREQS[0]))

#define POPUP_MS           2000
#define LOCK_HOLD_MS       20000
#define LOCK_RSSI_DROP     5.0f

typedef enum { OFF, ONCE, REP8, REP12, REP3, REP6 } AlertMode;
static const char* mode_names[] = {"Off","Once","8s","12s","3s","6s"};
static const char* scan_names[] = {"Static","Free"};

typedef struct {
    float rssi;
    uint32_t freq;
    bool detected;
} State;

typedef struct {
    bool exit;
    float baseline[UP_NUM_CH];
    NotificationApp* notif;
    bool locked;
    uint32_t lock_start;
    char popup[32];
    uint32_t popup_until;
    AlertMode mode;
    bool free_scan;
    bool debug;
    bool tdma;
    int sens;
    uint32_t last_freq; // For debug and resume
    float last_rssi;
    bool last_pk;
    // Plugin system radio device
    const SubGhzDevice* radio_device;
    SubGhzDeviceConf device_conf;
} AppCtx;

// Helpers
static inline uint32_t ticks(uint32_t ms) { return furi_ms_to_ticks(ms); }

static inline float read_rssi(AppCtx* app, uint32_t freq) {
    app->radio_device->interconnect->set_frequency(freq);
    furi_delay_ms(CAL_DELAY_MS);
    return app->radio_device->interconnect->get_rssi();
}

static bool persist(AppCtx* app, float threshold) {
    uint32_t end = furi_get_tick() + ticks(SLOT_MS);
    while(furi_get_tick() < end) {
        if(app->radio_device->interconnect->get_rssi() < threshold) return false;
    }
    return true;
}

static void input_cb(InputEvent* e, void* ctx) {
    AppCtx* app = ctx;
    if(e->type == InputTypePress) {
        switch(e->key) {
            case InputKeyUp:
                app->mode = (app->mode + 1) % 6;
                snprintf(app->popup,sizeof(app->popup),"Mode: %s",mode_names[app->mode]);
                break;
            case InputKeyLeft:
                app->free_scan = !app->free_scan;
                snprintf(app->popup,sizeof(app->popup),"Scan: %s",scan_names[app->free_scan]);
                break;
            case InputKeyRight:
                app->sens = (app->sens < 5 ? app->sens + 1 : 1);
                snprintf(app->popup,sizeof(app->popup),"Sens: %d",app->sens);
                break;
            case InputKeyDown:
                app->debug = !app->debug;
                snprintf(app->popup,sizeof(app->popup),"Debug: %s",app->debug?"On":"Off");
                break;
            case InputKeyOk:
                app->tdma = !app->tdma;
                snprintf(app->popup,sizeof(app->popup),"TDMA: %s",app->tdma?"On":"Off");
                break;
            case InputKeyBack:
                app->exit = true;
                break;
            default: break;
        }
        app->popup_until = furi_get_tick() + ticks(POPUP_MS);
        notification_message_block(app->notif, &sequence_semi_success);
    }
}

static void render(Canvas* c, void* ctx) {
    AppCtx* app = ctx;
    float r = app->last_rssi;
    uint32_t f = app->last_freq;
    bool detected = app->last_pk;

    int w = canvas_width(c);
    int h = canvas_height(c);
    int fh = canvas_current_font_height(c);

    canvas_clear(c);
    // Top hints
    canvas_draw_str(c, 2, fh, scan_names[app->free_scan]);
    canvas_draw_str(c, w-60, fh, app->tdma?"TDMA On":"TDMA Off");

    // Popup
    if(app->popup[0] && furi_get_tick() < app->popup_until) {
        int box_h = fh + 6;
        int pw = canvas_string_width(c, app->popup) + 8;
        int px = (w - pw)/2;
        int py = fh*2 + 4;
        canvas_draw_box(c, px, py, pw, box_h);
        canvas_set_color(c, ColorWhite);
        canvas_draw_str(c, px+4, py + (box_h - fh)/2 + 1, app->popup);
        canvas_set_color(c, ColorBlack);
    }

    // Debug view
    if(app->debug) {
        char buf[40];
        snprintf(buf,sizeof(buf),"Freq: %lu.%03lu MHz", (unsigned long)(f/1000000), (unsigned long)((f%1000000)/1000));
        canvas_draw_str(c,2,fh*3,buf);
        snprintf(buf,sizeof(buf),"Packet: %s", detected?"YES":"NO");
        canvas_draw_str(c,2,fh*4+4,buf);
        snprintf(buf,sizeof(buf),"RSSI: %.1f dBm", (double)r);
        canvas_draw_str(c,2,fh*5+8,buf);
        char lstr[4]; snprintf(lstr, sizeof(lstr),"L:%c", app->locked? 'Y':'N');
        int lw = canvas_string_width(c, lstr);
        canvas_draw_str(c, w-lw-2, fh*5+8, lstr);
        return;
    }

    // Main view
    const char* status = detected?"Locked":"Scanning";
    int tw = canvas_string_width(c,status);
    canvas_draw_str(c,(w-tw)/2,fh*2,status);

    // Strength bar
    int bx=8, by=(h/2)-6, bw=w-16, bh=12;
    float norm=(r-MIN_RSSI_DBM)/(-MIN_RSSI_DBM);
    norm = norm<0?0:(norm>1?1:norm);
    int fill=(int)(norm*(bw-2));
    canvas_draw_frame(c,bx,by,bw,bh);
    if(fill>0) canvas_draw_box(c,bx+1,by+1,fill,bh-2);
    // Labels
    for(int i=1;i<=5;i++) {
        int xi=bx+i*(bw/5);
        canvas_draw_line(c,xi,by,xi,by+bh-1);
        char lbl[2]={(char)('0'+i),'\0'};
        canvas_draw_str(c, xi-(fh/2), by+bh+fh, lbl);
    }
}

int32_t python_detector_app(void* p) {
    UNUSED(p);
    AppCtx app = {
        .exit = false,
        .mode = ONCE,
        .free_scan = false,
        .debug = false,
        .tdma = false,
        .sens = 3,
        .locked = false,
        .lock_start = 0,
        .last_freq = UP_START_FREQ,
        .last_rssi = MIN_RSSI_DBM,
        .last_pk = false,
        .radio_device = NULL,
    };

    Gui* gui = furi_record_open("gui");
    app.notif = furi_record_open("notification");
    if(!gui || !app.notif) return -1;

    // Plugin system: get the radio device from registry
    subghz_device_registry_init(); // make sure registry is initialized
    // Use the first available plugin device (after the built-in one)
    app.radio_device = subghz_device_registry_get_by_index(1);
    if(!app.radio_device) {
        subghz_device_registry_deinit();
        return -1;
    }
    // Optionally: check device->name and pick a specific one if you want

    // Device init
    if(!app.radio_device->interconnect->begin(&app.device_conf)) {
        subghz_device_registry_deinit();
        return -1;
    }

    // Calibrate
    for(size_t i=0;i<UP_NUM_CH;i++) {
        app.baseline[i] = read_rssi(&app, UP_START_FREQ + i*FREQ_STEP);
        furi_delay_ms(CAL_DELAY_MS);
    }

    ViewPort* vp = view_port_alloc();
    view_port_draw_callback_set(vp, render, &app);
    view_port_input_callback_set(vp, input_cb, &app);
    gui_add_view_port(gui, vp, GuiLayerFullscreen);

    while(!app.exit) {
        uint32_t now = furi_get_tick();
        // Locked behaviour
        if(app.locked) {
            uint32_t freq = app.last_freq;
            float r0 = read_rssi(&app, freq);
            float base = app.free_scan
                ? app.baseline[(freq-UP_START_FREQ)/FREQ_STEP]
                : MIN_RSSI_DBM;
            float thr = base + THRESH_DB - app.sens - LOCK_RSSI_DROP;
            bool pk = (r0 > thr) && persist(&app, thr);
            if(pk) app.lock_start = now;
            else if(now - app.lock_start > ticks(LOCK_HOLD_MS)) {
                app.locked=false;
            }
            app.last_rssi=r0; app.last_freq=freq; app.last_pk=pk;
            view_port_update(vp);
            furi_delay_ms(FRAME_MS);
            continue;
        }
        // Scanning behaviour
        size_t cnt = app.free_scan? UP_NUM_CH : STATIC_CH;
        for(size_t i=0;i<cnt && !app.exit;i++) {
            uint32_t freq = app.free_scan? (UP_START_FREQ+i*FREQ_STEP) : STATIC_FREQS[i];
            float r0 = read_rssi(&app, freq);
            float base = app.free_scan? app.baseline[i] : MIN_RSSI_DBM;
            float thr = base + THRESH_DB - app.sens;
            bool pk = (r0 > thr) && persist(&app, thr);
            if(pk) {
                app.locked=true;
                app.lock_start = now;
                app.last_freq=freq;
                notification_message_block(app.notif, &sequence_audiovisual_alert);
                break;
            }
            app.last_rssi=r0; app.last_freq=freq; app.last_pk=pk;
            view_port_update(vp);
            furi_delay_ms(FRAME_MS);
        }
    }

    gui_remove_view_port(gui,vp);
    view_port_free(vp);
    app.radio_device->interconnect->end();
    subghz_device_registry_deinit();
    furi_record_close("gui");
    furi_record_close("notification");
    return 0;
}