#include <stdlib.h>
#include <stdio.h>
#include <furi.h>
#include <gui/gui.h>
#include <gui/view_port.h>
#include <notification/notification_messages.h>
#include <notification/notification.h>
#include <furi_hal.h>
#include <input/input.h>
#include <furi_hal_light.h>

#define MIN_RSSI_DBM       -80.0f
#define CAL_SAMPLES        10
#define CAL_DELAY_MS       5
#define THRESH_DB          8
#define SLOT_MS            14
#define FRAME_MS           57

#define UP_START_FREQ      380000000U
#define UP_END_FREQ        385000000U
#define FREQ_STEP          25000U
#define UP_NUM_CH          (((UP_END_FREQ - UP_START_FREQ) / FREQ_STEP) + 1)

static const uint32_t STATIC_FREQS[] = {
    // original static channels
    389540000U, 388790000U, 389170000U,
    // newly captured beacons
    380450000U, 380425000U, 380400000U,
    379650000U, 380500000U, 379625000U,
    380375000U, 379375000U, 380325000U,
    380300000U, 379300000U, 380025000U,
	384437500U, 384712500U
};
#define STATIC_CH (sizeof(STATIC_FREQS)/sizeof(STATIC_FREQS[0]))

#define TUNE_MS            5
#define RX_MS              5
#define SCAN_MS            FRAME_MS    // dwell ~1 full TETRA frame (~57 ms)
#define POPUP_MS           2000
#define LOCK_HOLD_MS       20000  // 20 seconds
#define LOCK_RSSI_DROP     5.0f   // additional dB drop when locked

// Application modes
typedef enum { OFF, ONCE, REP8, REP12, REP3, REP6 } AlertMode;
static AlertMode mode = ONCE;
static const char* mode_names[] = {"Off","Once","8s","12s","3s","6s"};
static bool free_scan = false;
static const char* scan_names[] = {"Static","Free"};
static bool debug = false;
static bool tdma = false;
static int sens = 3;

// Lock state
static bool locked = false;
static uint32_t lock_start = 0;

// State
static float baseline[UP_NUM_CH];
static NotificationApp* notif;
static char popup[32];
static uint32_t popup_until;

// Helpers
static inline uint32_t ticks(uint32_t ms) { return furi_ms_to_ticks(ms); }
static inline float read_rssi(uint32_t freq) {
    furi_hal_subghz_idle();
    furi_hal_subghz_set_frequency(freq);
    furi_delay_ms(RX_MS);
    return furi_hal_subghz_get_rssi();
}
static bool persist(float threshold) {
    uint32_t end = furi_get_tick() + ticks(SLOT_MS);
    while(furi_get_tick() < end) {
        if(furi_hal_subghz_get_rssi() < threshold) return false;
    }
    return true;
}

// Input handler
static void input_cb(InputEvent* e, void* ctx) {
    bool* exit = ctx;
    if(e->type == InputTypePress) {
        switch(e->key) {
            case InputKeyUp:
                mode = (mode + 1) % 6;
                snprintf(popup,sizeof(popup),"Mode: %s",mode_names[mode]);
                break;
            case InputKeyLeft:
                free_scan = !free_scan;
                snprintf(popup,sizeof(popup),"Scan: %s",scan_names[free_scan]);
                break;
            case InputKeyRight:
                sens = (sens < 5 ? sens + 1 : 1);
                snprintf(popup,sizeof(popup),"Sens: %d",sens);
                break;
            case InputKeyDown:
                debug = !debug;
                snprintf(popup,sizeof(popup),"Debug: %s",debug?"On":"Off");
                break;
            case InputKeyOk:
                tdma = !tdma;
                snprintf(popup,sizeof(popup),"TDMA: %s",tdma?"On":"Off");
                break;
            case InputKeyBack:
                *exit = true;
                break;
            default: break;
        }
        popup_until = furi_get_tick() + ticks(POPUP_MS);
        notification_message_block(notif, &sequence_semi_success);
    }
}

// Render callback
typedef struct { float r; uint32_t f; bool p; } State;
static void render(Canvas* c, void* ctx) {
    State* st = ctx;
    float r = st->r;
    uint32_t f = st->f;
    bool p = st->p;

    canvas_clear(c);
    int w = canvas_width(c);
    int h = canvas_height(c);
    int fh = canvas_current_font_height(c);

    // Top hints
    canvas_draw_str(c, 2, fh, scan_names[free_scan]);
    canvas_draw_str(c, w-60, fh, tdma?"TDMA On":"TDMA Off");

    // Popup
    if(popup[0] && furi_get_tick() < popup_until) {
        int box_h = fh + 6;
        int pw = canvas_string_width(c, popup) + 8;
        int px = (w - pw)/2;
        int py = fh*2 + 4;
        canvas_draw_box(c, px, py, pw, box_h);
        canvas_set_color(c, ColorWhite);
        canvas_draw_str(c, px+4, py + (box_h - fh)/2 + 1, popup);
        canvas_set_color(c, ColorBlack);
    }

    // Debug view
    if(debug) {
        char buf[32];
        snprintf(buf,sizeof(buf),"Freq: %lu.%03lu MHz", (unsigned long)(f/1000000), (unsigned long)((f%1000000)/1000));
        canvas_draw_str(c,2,fh*3,buf);
        snprintf(buf,sizeof(buf),"Packet: %s", p?"YES":"NO");
        canvas_draw_str(c,2,fh*4+4,buf);
        snprintf(buf,sizeof(buf),"RSSI: %.1f dBm", (double)r);
        canvas_draw_str(c,2,fh*5+8,buf);
        // Lock indicator bottom-right
        char lstr[4]; snprintf(lstr, sizeof(lstr),"L:%c", locked? 'Y':'N');
        int lw = canvas_string_width(c, lstr);
        canvas_draw_str(c, w-lw-2, fh*5+8, lstr);
        return;
    }

    // Main view
    const char* status = p?"Locked":"Scanning";
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

int32_t python_detector_app(void) {
    furi_hal_subghz_set_path(FuriHalSubGhzPathExternal);
    Gui* gui = furi_record_open("gui");
    notif = furi_record_open("notification");
    if(!gui||!notif) return -1;

    // Calibrate
    for(size_t i=0;i<UP_NUM_CH;i++) {
        baseline[i] = read_rssi(UP_START_FREQ + i*FREQ_STEP);
        furi_delay_ms(CAL_DELAY_MS);
    }

    ViewPort* vp = view_port_alloc();
    State state={MIN_RSSI_DBM,UP_START_FREQ,false};
    bool exit=false;
    view_port_draw_callback_set(vp,render,&state);
    view_port_input_callback_set(vp,input_cb,&exit);
    gui_add_view_port(gui,vp,GuiLayerFullscreen);

    while(!exit) {
        uint32_t now = furi_get_tick();
        // Locked behaviour
        if(locked) {
            uint32_t freq = state.f;
            float r0=read_rssi(freq);
            // original threshold
            float base = free_scan? baseline[(freq-UP_START_FREQ)/FREQ_STEP]
                                    : MIN_RSSI_DBM;
            float thr = base + THRESH_DB - sens - LOCK_RSSI_DROP;
            bool pk=(r0>thr)&&persist(thr);
            if(pk) lock_start = now; // reset hold timer
            else if(now - lock_start > ticks(LOCK_HOLD_MS)) {
                locked=false; // unlock
            }
            state.r=r0; state.f=freq; state.p=pk;
            view_port_update(vp);
            furi_delay_ms(SCAN_MS);
            continue;
        }
        // Scanning behaviour
        size_t cnt = free_scan? UP_NUM_CH : STATIC_CH;
        for(size_t i=0;i<cnt&&!exit;i++) {
            uint32_t freq = free_scan? (UP_START_FREQ+i*FREQ_STEP) : STATIC_FREQS[i];
            float r0=read_rssi(freq);
            float base = free_scan? baseline[i] : MIN_RSSI_DBM;
            float thr = base + THRESH_DB - sens;
            bool pk=(r0>thr)&&persist(thr);
            if(pk) {
                // lock on detection
                locked=true;
                lock_start = now;
                state.f=freq;
                // alert
                notification_message_block(notif, &sequence_audiovisual_alert);
                break;
            }
            state.r=r0; state.f=freq; state.p=pk;
            view_port_update(vp);
            furi_delay_ms(SCAN_MS);
        }
    }

    gui_remove_view_port(gui,vp);
    view_port_free(vp);
    furi_record_close("gui");
    furi_record_close("notification");
    furi_hal_subghz_set_path(FuriHalSubGhzPathInternal);
    return 0;
}
