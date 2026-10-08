/*
 * xlib-hello : 툴킷(GTK/Qt) 없이 X11 프로토콜만으로 만든 GUI 테스트 앱
 *
 * 목표: "버튼", "텍스트 입력창", "그림판" 같은 위젯이 실제로는
 *       [사각형 그리기 + 마우스 좌표 판정 + 다시 그리기] 일 뿐이라는 걸 직접 보기.
 *
 * 전체 흐름
 *   1) XOpenDisplay   : X 서버(디스플레이 서버)에 소켓으로 접속
 *   2) XCreateWindow  : 서버에 "창 하나 만들어줘" 요청
 *   3) XSelectInput   : 어떤 이벤트(마우스/키보드/다시그리기)를 받을지 등록
 *   4) XMapWindow     : 창을 화면에 표시
 *   5) 이벤트 루프    : XNextEvent 로 이벤트를 하나씩 꺼내 처리 → 상태 변경 → 다시 그리기
 *
 * 빌드: make      실행: ./xlib-hello      (이벤트 로그를 터미널에도 찍으려면 -v)
 */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#define WIN_W 640
#define WIN_H 480
#define LOG_LINES 8
#define MAX_POINTS 4096
#define TEXT_MAX 64

/* ---- 아주 단순한 "위젯" : 그냥 사각형 영역 ---- */
typedef struct { int x, y, w, h; } Rect;

static int rect_hit(Rect r, int px, int py)
{
    return px >= r.x && px < r.x + r.w && py >= r.y && py < r.y + r.h;
}

/* ---- 앱 상태 (툴킷이라면 위젯 객체 안에 숨겨져 있을 것들) ---- */
typedef struct {
    Display *dpy;
    int screen;
    Window win;
    Pixmap back;          /* 더블 버퍼: 여기 먼저 그린 뒤 한 번에 창으로 복사 */
    GC gc;
    XFontStruct *font;
    Atom wm_delete;       /* 창 닫기(X 버튼) 메시지를 받기 위한 atom */
    int width, height;

    unsigned long c_bg, c_panel, c_btn, c_btn_hover, c_btn_down,
                  c_text, c_accent, c_input, c_border;

    Rect btn, clear_btn, input, canvas;
    int hover_btn, hover_clear, pressed_btn, focus_input;
    int clicks;
    char text[TEXT_MAX + 1];

    /* 그림판: 선분 끝점들. x<0 이면 획(stroke) 구분자 */
    XPoint pts[MAX_POINTS];
    int npts, drawing;

    char log[LOG_LINES][96];
    int log_head;
    int verbose;
    unsigned long event_count;
} App;

static void app_log(App *a, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(a->log[a->log_head], sizeof a->log[0], fmt, ap);
    va_end(ap);
    if (a->verbose)
        printf("[event #%lu] %s\n", a->event_count, a->log[a->log_head]);
    a->log_head = (a->log_head + 1) % LOG_LINES;
}

static unsigned long rgb(App *a, unsigned r, unsigned g, unsigned b)
{
    /* X 서버에 색을 "할당" 요청. TrueColor 화면이면 사실상 즉시 픽셀값을 돌려줌 */
    XColor c = { .red = r * 257, .green = g * 257, .blue = b * 257,
                 .flags = DoRed | DoGreen | DoBlue };
    XAllocColor(a->dpy, DefaultColormap(a->dpy, a->screen), &c);
    return c.pixel;
}

/* 창 크기가 바뀌면 위젯 배치(layout)를 다시 계산 — 툴킷의 레이아웃 매니저가 하는 일 */
static void layout(App *a)
{
    int m = 16;
    a->btn       = (Rect){ m, m, 160, 36 };
    a->clear_btn = (Rect){ m + 176, m, 120, 36 };
    a->input     = (Rect){ m, m + 52, a->width - 2 * m, 32 };
    int log_h = LOG_LINES * 15 + 12;
    int cy = m + 100;
    a->canvas = (Rect){ m, cy, a->width - 2 * m, a->height - cy - log_h - 2 * m };
    if (a->canvas.h < 40) a->canvas.h = 40;
}

static void recreate_backbuffer(App *a)
{
    if (a->back) XFreePixmap(a->dpy, a->back);
    a->back = XCreatePixmap(a->dpy, a->win, a->width, a->height,
                            DefaultDepth(a->dpy, a->screen));
}

static void draw_text(App *a, int x, int y, unsigned long color, const char *s)
{
    XSetForeground(a->dpy, a->gc, color);
    XDrawString(a->dpy, a->back, a->gc, x, y, s, (int)strlen(s));
}

static void draw_box(App *a, Rect r, unsigned long fill, unsigned long border)
{
    XSetForeground(a->dpy, a->gc, fill);
    XFillRectangle(a->dpy, a->back, a->gc, r.x, r.y, r.w, r.h);
    XSetForeground(a->dpy, a->gc, border);
    XDrawRectangle(a->dpy, a->back, a->gc, r.x, r.y, r.w - 1, r.h - 1);
}

/* 텍스트를 사각형 가운데에 — 글자 폭은 폰트 정보로 직접 계산해야 한다 */
static void draw_label_centered(App *a, Rect r, unsigned long color, const char *s)
{
    int tw = XTextWidth(a->font, s, (int)strlen(s));
    int th = a->font->ascent + a->font->descent;
    draw_text(a, r.x + (r.w - tw) / 2, r.y + (r.h - th) / 2 + a->font->ascent, color, s);
}

/* "버튼 위젯" 그리기: 상태(hover/pressed)에 따라 색만 바꾸는 사각형 */
static void draw_button(App *a, Rect r, const char *label, int hover, int pressed)
{
    unsigned long fill = pressed ? a->c_btn_down : hover ? a->c_btn_hover : a->c_btn;
    draw_box(a, r, fill, a->c_border);
    Rect lr = r;
    if (pressed) { lr.x += 1; lr.y += 1; }   /* 눌린 느낌: 글자를 1px 이동 */
    draw_label_centered(a, lr, a->c_text, label);
}

/* 화면 전체를 처음부터 다시 그린다 (immediate-mode 방식, 가장 단순한 전략) */
static void redraw(App *a)
{
    char buf[128];

    XSetForeground(a->dpy, a->gc, a->c_bg);
    XFillRectangle(a->dpy, a->back, a->gc, 0, 0, a->width, a->height);

    snprintf(buf, sizeof buf, "Clicked: %d", a->clicks);
    draw_button(a, a->btn, buf, a->hover_btn, a->pressed_btn);
    draw_button(a, a->clear_btn, "Clear canvas", a->hover_clear, 0);

    /* 텍스트 입력창 + 커서 */
    draw_box(a, a->input, a->c_input, a->focus_input ? a->c_accent : a->c_border);
    const char *shown = a->text[0] || a->focus_input ? a->text : "click here and type...";
    int ty = a->input.y + (a->input.h + a->font->ascent - a->font->descent) / 2;
    draw_text(a, a->input.x + 8, ty, a->text[0] || a->focus_input ? a->c_text : a->c_border, shown);
    if (a->focus_input) {
        int cx = a->input.x + 8 + XTextWidth(a->font, a->text, (int)strlen(a->text)) + 1;
        XSetForeground(a->dpy, a->gc, a->c_accent);
        XFillRectangle(a->dpy, a->back, a->gc, cx, a->input.y + 7, 2, a->input.h - 14);
    }

    /* 그림판 */
    draw_box(a, a->canvas, a->c_panel, a->c_border);
    draw_text(a, a->canvas.x + 8, a->canvas.y + 16, a->c_border, "canvas: drag with left mouse button");
    XRectangle clip = { (short)(a->canvas.x + 1), (short)(a->canvas.y + 1),
                        (unsigned short)(a->canvas.w - 2), (unsigned short)(a->canvas.h - 2) };
    XSetClipRectangles(a->dpy, a->gc, 0, 0, &clip, 1, Unsorted);
    XSetForeground(a->dpy, a->gc, a->c_accent);
    XSetLineAttributes(a->dpy, a->gc, 3, LineSolid, CapRound, JoinRound);
    int start = 0;
    for (int i = 0; i <= a->npts; i++) {
        if (i == a->npts || a->pts[i].x < 0) {
            if (i - start >= 2)
                XDrawLines(a->dpy, a->back, a->gc, &a->pts[start], i - start, CoordModeOrigin);
            start = i + 1;
        }
    }
    XSetLineAttributes(a->dpy, a->gc, 1, LineSolid, CapButt, JoinMiter);
    XSetClipMask(a->dpy, a->gc, None);

    /* 이벤트 로그 패널 */
    Rect lr = { 16, a->canvas.y + a->canvas.h + 16, a->width - 32, LOG_LINES * 15 + 12 };
    draw_box(a, lr, a->c_panel, a->c_border);
    for (int i = 0; i < LOG_LINES; i++) {
        const char *line = a->log[(a->log_head + i) % LOG_LINES];
        if (line[0])
            draw_text(a, lr.x + 8, lr.y + 16 + i * 15, i == LOG_LINES - 1 ? a->c_text : a->c_border, line);
    }

    /* 완성된 백버퍼를 창으로 한 번에 복사 → 깜빡임 없음 */
    XCopyArea(a->dpy, a->back, a->win, a->gc, 0, 0, a->width, a->height, 0, 0);
    /* Xlib 은 요청을 버퍼에 모아두므로, 실제로 서버에 보내려면 flush */
    XFlush(a->dpy);
}

static void add_point(App *a, int x, int y)
{
    if (a->npts < MAX_POINTS) a->pts[a->npts++] = (XPoint){ (short)x, (short)y };
}

/* ---- 이벤트 처리: 툴킷의 "signal/callback" 이 결국 여기서 갈라져 나간다 ---- */
static int handle_event(App *a, XEvent *ev)
{
    int dirty = 0;
    a->event_count++;

    switch (ev->type) {
    case Expose:
        /* 창의 일부가 가려졌다 다시 보이면 서버가 "다시 그려줘"라고 알려준다.
         * X 서버는 창 내용을 기억하지 않는다(기본값) — 그리는 건 전적으로 앱 책임 */
        if (ev->xexpose.count == 0) {
            app_log(a, "Expose  area=%dx%d+%d+%d", ev->xexpose.width, ev->xexpose.height,
                    ev->xexpose.x, ev->xexpose.y);
            dirty = 1;
        }
        break;

    case ConfigureNotify:
        if (ev->xconfigure.width != a->width || ev->xconfigure.height != a->height) {
            a->width = ev->xconfigure.width;
            a->height = ev->xconfigure.height;
            app_log(a, "ConfigureNotify  resized to %dx%d", a->width, a->height);
            layout(a);
            recreate_backbuffer(a);
            dirty = 1;
        }
        break;

    case MotionNotify: {
        int x = ev->xmotion.x, y = ev->xmotion.y;
        int hb = rect_hit(a->btn, x, y), hc = rect_hit(a->clear_btn, x, y);
        if (hb != a->hover_btn || hc != a->hover_clear) {
            a->hover_btn = hb; a->hover_clear = hc;
            app_log(a, "MotionNotify (%d,%d)  hover: %s", x, y,
                    hb ? "button" : hc ? "clear" : "none");
            dirty = 1;
        }
        if (a->drawing) {
            add_point(a, x, y);
            dirty = 1;
        }
        break;
    }

    case ButtonPress: {
        int x = ev->xbutton.x, y = ev->xbutton.y;
        app_log(a, "ButtonPress  button=%u at (%d,%d)", ev->xbutton.button, x, y);
        if (ev->xbutton.button != Button1) break;
        /* 히트 테스트: "어느 위젯 위에서 눌렸나?" 를 직접 판정 */
        a->focus_input = rect_hit(a->input, x, y);
        if (rect_hit(a->btn, x, y)) a->pressed_btn = 1;
        if (rect_hit(a->canvas, x, y)) {
            if (a->npts && a->npts < MAX_POINTS) add_point(a, -1, -1);  /* 획 구분 */
            add_point(a, x, y);
            a->drawing = 1;
        }
        dirty = 1;
        break;
    }

    case ButtonRelease: {
        int x = ev->xbutton.x, y = ev->xbutton.y;
        app_log(a, "ButtonRelease  button=%u at (%d,%d)", ev->xbutton.button, x, y);
        if (ev->xbutton.button != Button1) break;
        /* 진짜 버튼처럼: 눌렀던 위치와 뗀 위치가 둘 다 버튼 안이어야 "클릭" */
        if (a->pressed_btn && rect_hit(a->btn, x, y)) {
            a->clicks++;
            app_log(a, "  -> on_click()  clicks=%d", a->clicks);
        }
        if (rect_hit(a->clear_btn, x, y)) {
            a->npts = 0;
            app_log(a, "  -> canvas cleared");
        }
        a->pressed_btn = 0;
        a->drawing = 0;
        dirty = 1;
        break;
    }

    case KeyPress: {
        char buf[16] = {0};
        KeySym sym;
        /* 키코드(하드웨어 번호) → KeySym(논리 키) → 문자 로 변환 */
        int n = XLookupString(&ev->xkey, buf, sizeof buf - 1, &sym, NULL);
        const char *name = XKeysymToString(sym);
        app_log(a, "KeyPress  keycode=%u keysym=%s text=\"%s\"", ev->xkey.keycode,
                name ? name : "?", n > 0 && buf[0] >= 32 ? buf : "");
        if (sym == XK_Escape) return 0;
        if (!a->focus_input) break;   /* 포커스 없는 위젯은 키 입력 무시 */
        size_t len = strlen(a->text);
        if (sym == XK_BackSpace) {
            if (len) a->text[len - 1] = 0;
        } else if (sym == XK_Return) {
            app_log(a, "  -> on_submit(\"%s\")", a->text);
            a->text[0] = 0;
        } else if (n == 1 && buf[0] >= 32 && buf[0] < 127 && len < TEXT_MAX) {
            a->text[len] = buf[0];
            a->text[len + 1] = 0;
        }
        dirty = 1;
        break;
    }

    case FocusIn:  app_log(a, "FocusIn  (window got keyboard focus)"); dirty = 1; break;
    case FocusOut: app_log(a, "FocusOut"); dirty = 1; break;

    case ClientMessage:
        /* 윈도우 매니저가 보낸 "닫기 버튼 눌림" 메시지 */
        if ((Atom)ev->xclient.data.l[0] == a->wm_delete) {
            app_log(a, "ClientMessage WM_DELETE_WINDOW -> quit");
            return 0;
        }
        break;
    }

    if (dirty) redraw(a);
    return 1;
}

int main(int argc, char **argv)
{
    App a = {0};
    a.verbose = argc > 1 && strcmp(argv[1], "-v") == 0;
    setvbuf(stdout, NULL, _IOLBF, 0);   /* 파이프로 보내도 로그가 즉시 보이도록 */
    a.width = WIN_W;
    a.height = WIN_H;

    /* 1) X 서버 접속. 주소는 $DISPLAY 환경변수 (예: ":0" → /tmp/.X11-unix/X0 소켓) */
    a.dpy = XOpenDisplay(NULL);
    if (!a.dpy) {
        fprintf(stderr, "X 서버에 접속할 수 없습니다. DISPLAY=%s\n",
                getenv("DISPLAY") ? getenv("DISPLAY") : "(unset)");
        return 1;
    }
    a.screen = DefaultScreen(a.dpy);

    a.c_bg        = rgb(&a, 0x1e, 0x22, 0x2a);
    a.c_panel     = rgb(&a, 0x27, 0x2c, 0x36);
    a.c_btn       = rgb(&a, 0x3b, 0x42, 0x52);
    a.c_btn_hover = rgb(&a, 0x4c, 0x56, 0x6a);
    a.c_btn_down  = rgb(&a, 0x2e, 0x34, 0x40);
    a.c_text      = rgb(&a, 0xec, 0xef, 0xf4);
    a.c_accent    = rgb(&a, 0x88, 0xc0, 0xd0);
    a.c_input     = rgb(&a, 0x18, 0x1b, 0x21);
    a.c_border    = rgb(&a, 0x6b, 0x74, 0x86);

    /* 2) 창 생성 요청. 이 시점엔 아직 화면에 안 보인다 */
    a.win = XCreateSimpleWindow(a.dpy, RootWindow(a.dpy, a.screen), 100, 100,
                                a.width, a.height, 0, a.c_border, a.c_bg);
    XStoreName(a.dpy, a.win, "xlib-hello: native GUI without a toolkit");

    /* 3) 받고 싶은 이벤트 종류를 서버에 등록. 등록 안 한 이벤트는 아예 오지 않는다 */
    XSelectInput(a.dpy, a.win,
                 ExposureMask | StructureNotifyMask | KeyPressMask |
                 ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
                 FocusChangeMask);

    /* 윈도우 매니저와의 약속(ICCCM): 닫기 버튼을 누르면 강제종료 대신 메시지로 알려달라 */
    a.wm_delete = XInternAtom(a.dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(a.dpy, a.win, &a.wm_delete, 1);

    a.gc = XCreateGC(a.dpy, a.win, 0, NULL);   /* GC = 펜(색, 선굵기, 폰트) 설정 묶음 */
    a.font = XLoadQueryFont(a.dpy, "-misc-fixed-medium-r-normal--13-*-*-*-*-*-*-*");
    if (!a.font) a.font = XLoadQueryFont(a.dpy, "fixed");
    if (!a.font) { fprintf(stderr, "font 'fixed' not found\n"); return 1; }
    XSetFont(a.dpy, a.gc, a.font->fid);

    layout(&a);
    recreate_backbuffer(&a);
    app_log(&a, "connected to X server \"%s\", screen %d (%dx%d)",
            DisplayString(a.dpy), a.screen,
            DisplayWidth(a.dpy, a.screen), DisplayHeight(a.dpy, a.screen));

    /* 4) 화면에 표시. 실제로 보이면 서버가 Expose 이벤트를 보내준다 */
    XMapWindow(a.dpy, a.win);

    /* 5) 이벤트 루프 — 모든 GUI 앱(GTK, Qt, Electron...)의 심장부 */
    XEvent ev;
    for (;;) {
        XNextEvent(a.dpy, &ev);          /* 이벤트가 올 때까지 블록(잠듦) */
        if (!handle_event(&a, &ev)) break;
    }

    XFreePixmap(a.dpy, a.back);
    XFreeFont(a.dpy, a.font);
    XFreeGC(a.dpy, a.gc);
    XDestroyWindow(a.dpy, a.win);
    XCloseDisplay(a.dpy);
    return 0;
}
