/*
 * gtk-lab : GTK4 로 만든 "이벤트 실험실"
 *
 * xlib-hello 에서는 버튼·입력창·그림판을 사각형 그리기 + 좌표 판정으로 직접 만들었다.
 * 여기서는 같은 일을 GTK 위젯으로 하고, 오른쪽 로그에서 두 층을 나란히 보여준다.
 *
 *   [입력]   GTK 가 디스플레이 서버(X11/Wayland)에서 받은 원시 이벤트
 *            (키 눌림, 마우스 버튼, 포인터 이동) — 창 전체에 붙인 event controller 로 엿본다
 *   [시그널] 위젯이 원시 이벤트를 해석해서 내보내는 고수준 신호
 *            (버튼 "clicked", 입력창 "changed" ...) — 앱 개발자가 보통 연결하는 콜백
 *   [그리기] 위젯이 다시 그려질 때 호출되는 draw 콜백
 *
 * 빌드: make gtk-lab     실행: ./gtk-lab
 */
#include <gtk/gtk.h>

typedef struct {
    GtkWidget *window;
    GtkTextBuffer *log_buf;
    GtkWidget *log_view;
    GtkTextMark *log_end;
    gint64 t0;

    GtkWidget *log_raw_input;   /* "원시 입력 기록" 체크 */
    GtkWidget *log_motion;      /* "마우스 이동 기록" 체크 */
    GtkWidget *log_draw;        /* "draw 호출 기록" 체크 */

    GtkWidget *count_label;
    int clicks;
    GtkWidget *submit_label;
    GtkWidget *scale_label;

    GtkWidget *canvas;
    GArray *points;             /* graphene_point_t, x<0 이면 획 구분자 */
    double drag_x, drag_y;
} App;

/* ------------------------------------------------------------------ 로그 */

static const char *const KIND_TAG[] = { "input", "signal", "draw", "info" };
static const char *const KIND_LABEL[] = { "[입력]", "[시그널]", "[그리기]", "[정보]" };
enum { K_INPUT, K_SIGNAL, K_DRAW, K_INFO };

static void app_log(App *a, int kind, const char *fmt, ...) G_GNUC_PRINTF(3, 4);

static void app_log(App *a, int kind, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char *msg = g_strdup_vprintf(fmt, ap);
    va_end(ap);

    double t = (g_get_monotonic_time() - a->t0) / 1e6;
    char *ts = g_strdup_printf("%7.2fs\t", t);

    GtkTextIter end;
    gtk_text_buffer_get_end_iter(a->log_buf, &end);
    gtk_text_buffer_insert_with_tags_by_name(a->log_buf, &end, ts, -1, "time", NULL);
    gtk_text_buffer_insert_with_tags_by_name(a->log_buf, &end, KIND_LABEL[kind], -1,
                                             KIND_TAG[kind], "bold", NULL);
    gtk_text_buffer_insert(a->log_buf, &end, "\t", -1);
    gtk_text_buffer_insert(a->log_buf, &end, msg, -1);
    gtk_text_buffer_insert(a->log_buf, &end, "\n", -1);

    /* 너무 길어지면 앞쪽을 잘라낸다 */
    int lines = gtk_text_buffer_get_line_count(a->log_buf);
    if (lines > 3000) {
        GtkTextIter s, e;
        gtk_text_buffer_get_start_iter(a->log_buf, &s);
        gtk_text_buffer_get_iter_at_line(a->log_buf, &e, lines - 2500);
        gtk_text_buffer_delete(a->log_buf, &s, &e);
    }
    gtk_text_view_scroll_mark_onscreen(GTK_TEXT_VIEW(a->log_view), a->log_end);

    g_free(ts);
    g_free(msg);
}

static gboolean is_on(GtkWidget *check)
{
    return gtk_check_button_get_active(GTK_CHECK_BUTTON(check));
}

/* 좌표 아래에 있는 위젯을 찾아 "GtkLabel ⊂ GtkButton" 처럼 설명한다.
 * GTK 가 클릭을 어느 위젯에 줄지 정하는 히트 테스트(gtk_widget_pick)와 같은 것 */
static char *describe_widget_at(App *a, double x, double y)
{
    GtkWidget *w = gtk_widget_pick(a->window, x, y, GTK_PICK_DEFAULT);
    if (!w) return g_strdup("(없음)");

    GString *s = g_string_new(G_OBJECT_TYPE_NAME(w));
    static const char *const interesting[] = {
        "GtkButton", "GtkEntry", "GtkCheckButton", "GtkSwitch", "GtkScale", "GtkDrawingArea",
        "GtkTextView", "GtkPaned", NULL
    };
    for (GtkWidget *p = w; p; p = gtk_widget_get_parent(p)) {
        for (int i = 0; interesting[i]; i++) {
            if (g_strcmp0(G_OBJECT_TYPE_NAME(p), interesting[i]) == 0) {
                if (p != w) g_string_append_printf(s, " ⊂ %s", interesting[i]);
                return g_string_free(s, FALSE);
            }
        }
    }
    return g_string_free(s, FALSE);
}

/* ----------------------------------------------- [입력] 원시 이벤트 엿보기 */

static gboolean on_key_pressed(GtkEventControllerKey *c, guint keyval, guint keycode,
                               GdkModifierType state, App *a)
{
    (void)c; (void)state;
    if (is_on(a->log_raw_input)) {
        gunichar ch = gdk_keyval_to_unicode(keyval);
        char utf8[8] = {0};
        if (ch >= 32) g_unichar_to_utf8(ch, utf8);
        app_log(a, K_INPUT, "키 누름 %-10s keycode=%-3u %s%s%s",
                gdk_keyval_name(keyval), keycode,
                utf8[0] ? "문자='" : "", utf8, utf8[0] ? "'" : "");
    }
    return GDK_EVENT_PROPAGATE;   /* 엿보기만 하고 이벤트는 원래 위젯으로 계속 흘려보낸다 */
}

static void on_key_released(GtkEventControllerKey *c, guint keyval, guint keycode,
                            GdkModifierType state, App *a)
{
    (void)c; (void)state;
    if (is_on(a->log_raw_input))
        app_log(a, K_INPUT, "키 뗌   %-10s keycode=%u", gdk_keyval_name(keyval), keycode);
}

static void on_click_pressed(GtkGestureClick *g, int n_press, double x, double y, App *a)
{
    if (!is_on(a->log_raw_input)) return;
    char *who = describe_widget_at(a, x, y);
    app_log(a, K_INPUT, "마우스%u 누름 (%.0f,%.0f)%s → %s",
            gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(g)), x, y,
            n_press > 1 ? " [더블]" : "", who);
    g_free(who);
}

static void on_click_released(GtkGestureClick *g, int n_press, double x, double y, App *a)
{
    (void)n_press;
    if (!is_on(a->log_raw_input)) return;
    app_log(a, K_INPUT, "마우스%u 뗌   (%.0f,%.0f)",
            gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(g)), x, y);
}

static void on_motion(GtkEventControllerMotion *c, double x, double y, App *a)
{
    (void)c;
    if (!is_on(a->log_motion)) return;
    char *who = describe_widget_at(a, x, y);
    app_log(a, K_INPUT, "포인터 이동 (%.0f,%.0f) → %s", x, y, who);
    g_free(who);
}

/* --------------------------------------- [시그널] 위젯이 내보내는 고수준 신호 */

static void on_button_clicked(GtkButton *b, App *a)
{
    (void)b;
    a->clicks++;
    char *t = g_strdup_printf("%d번 눌림", a->clicks);
    gtk_label_set_text(GTK_LABEL(a->count_label), t);
    g_free(t);
    app_log(a, K_SIGNAL, "GtkButton::clicked → on_button_clicked() 카운트=%d", a->clicks);
}

static void on_entry_changed(GtkEditable *e, App *a)
{
    app_log(a, K_SIGNAL, "GtkEntry::changed → \"%s\"", gtk_editable_get_text(e));
}

static void on_entry_activate(GtkEntry *e, App *a)
{
    const char *text = gtk_editable_get_text(GTK_EDITABLE(e));
    app_log(a, K_SIGNAL, "GtkEntry::activate (Enter) → 제출 \"%s\"", text);
    char *t = g_strdup_printf("제출한 내용: %s", text);
    gtk_label_set_text(GTK_LABEL(a->submit_label), t);
    g_free(t);
    gtk_editable_set_text(GTK_EDITABLE(e), "");
}

static void on_check_toggled(GtkCheckButton *c, App *a)
{
    app_log(a, K_SIGNAL, "GtkCheckButton::toggled → %s",
            gtk_check_button_get_active(c) ? "체크됨" : "해제됨");
}

static gboolean on_switch_state_set(GtkSwitch *s, gboolean state, App *a)
{
    (void)s;
    app_log(a, K_SIGNAL, "GtkSwitch::state-set → %s", state ? "켜짐" : "꺼짐");
    return FALSE;   /* FALSE = 기본 동작(스위치 상태 변경)을 그대로 진행 */
}

static void on_scale_changed(GtkRange *r, App *a)
{
    int v = (int)gtk_range_get_value(r);
    char *t = g_strdup_printf("값: %d", v);
    gtk_label_set_text(GTK_LABEL(a->scale_label), t);
    g_free(t);
    app_log(a, K_SIGNAL, "GtkScale::value-changed → %d", v);
}

/* ----------------------------------------------------------------- 그림판 */

static void canvas_draw(GtkDrawingArea *area, cairo_t *cr, int w, int h, gpointer data)
{
    App *a = data;
    (void)area;

    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_paint(cr);

    if (a->points->len == 0) {
        /* 글자는 Pango 로: 한글 폰트 찾기(fontconfig)·글자 모양 잡기(HarfBuzz)를 해준다 */
        PangoLayout *layout = gtk_widget_create_pango_layout(a->canvas,
            "여기를 마우스로 드래그해서 그려보세요");
        cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
        cairo_move_to(cr, 12, 10);
        pango_cairo_show_layout(cr, layout);
        g_object_unref(layout);
    }

    cairo_set_source_rgb(cr, 0.13, 0.45, 0.85);
    cairo_set_line_width(cr, 3);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    gboolean new_stroke = TRUE;
    for (guint i = 0; i < a->points->len; i++) {
        graphene_point_t *p = &g_array_index(a->points, graphene_point_t, i);
        if (p->x < 0) { new_stroke = TRUE; continue; }
        if (new_stroke) cairo_move_to(cr, p->x, p->y);
        else cairo_line_to(cr, p->x, p->y);
        new_stroke = FALSE;
    }
    cairo_stroke(cr);

    if (is_on(a->log_draw))
        app_log(a, K_DRAW, "GtkDrawingArea draw() %dx%d 점 %u개 다시 그림", w, h, a->points->len);
}

static void add_point(App *a, double x, double y)
{
    graphene_point_t p = { (float)x, (float)y };
    g_array_append_val(a->points, p);
}

static void on_drag_begin(GtkGestureDrag *g, double x, double y, App *a)
{
    (void)g;
    if (a->points->len) add_point(a, -1, -1);
    a->drag_x = x;
    a->drag_y = y;
    add_point(a, x, y);
    app_log(a, K_SIGNAL, "GtkGestureDrag::drag-begin (%.0f,%.0f) → 새 획", x, y);
    gtk_widget_queue_draw(a->canvas);
}

static void on_drag_update(GtkGestureDrag *g, double dx, double dy, App *a)
{
    (void)g;
    add_point(a, a->drag_x + dx, a->drag_y + dy);
    /* 상태만 바꾸고 "다시 그려줘" 예약. 실제 그리기는 다음 프레임에 GTK 가 draw 콜백을 부른다 */
    gtk_widget_queue_draw(a->canvas);
}

static void on_drag_end(GtkGestureDrag *g, double dx, double dy, App *a)
{
    (void)g;
    app_log(a, K_SIGNAL, "GtkGestureDrag::drag-end 이동 (%.0f,%.0f) → 획 완성", dx, dy);
}

static void on_clear_canvas(GtkButton *b, App *a)
{
    (void)b;
    g_array_set_size(a->points, 0);
    gtk_widget_queue_draw(a->canvas);
    app_log(a, K_SIGNAL, "GtkButton::clicked → 그림판 지우기");
}

static void on_clear_log(GtkButton *b, App *a)
{
    (void)b;
    gtk_text_buffer_set_text(a->log_buf, "", -1);
    a->t0 = g_get_monotonic_time();
}

/* ------------------------------------------------------------------ 화면 구성 */

/* 섹션 하나 = 제목 + "해보세요" 설명 + 내용 위젯 */
static GtkWidget *section(const char *title, const char *hint, GtkWidget *content)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_add_css_class(box, "section");

    GtkWidget *t = gtk_label_new(title);
    gtk_widget_add_css_class(t, "section-title");
    gtk_label_set_xalign(GTK_LABEL(t), 0);
    gtk_box_append(GTK_BOX(box), t);

    GtkWidget *h = gtk_label_new(hint);
    gtk_widget_add_css_class(h, "hint");
    gtk_label_set_xalign(GTK_LABEL(h), 0);
    gtk_label_set_wrap(GTK_LABEL(h), TRUE);
    gtk_box_append(GTK_BOX(box), h);

    gtk_box_append(GTK_BOX(box), content);
    return box;
}

static GtkWidget *build_playground(App *a)
{
    GtkWidget *col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_start(col, 16);
    gtk_widget_set_margin_end(col, 8);
    gtk_widget_set_margin_top(col, 16);
    gtk_widget_set_margin_bottom(col, 16);

    /* 1. 버튼 */
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *btn = gtk_button_new_with_label("눌러보세요");
    gtk_widget_add_css_class(btn, "suggested-action");
    a->count_label = gtk_label_new("0번 눌림");
    gtk_box_append(GTK_BOX(row), btn);
    gtk_box_append(GTK_BOX(row), a->count_label);
    g_signal_connect(btn, "clicked", G_CALLBACK(on_button_clicked), a);
    gtk_box_append(GTK_BOX(col), section("① 버튼",
        "클릭해 보세요. 로그에 [입력] 마우스 누름/뗌 → [시그널] clicked 순서로 찍힙니다. "
        "버튼 위에서 누르고 밖에서 떼면 clicked 가 안 나옵니다.", row));

    /* 2. 텍스트 입력 */
    GtkWidget *ebox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "여기에 입력하고 Enter (한글도 됩니다)");
    a->submit_label = gtk_label_new("제출한 내용: (없음)");
    gtk_label_set_xalign(GTK_LABEL(a->submit_label), 0);
    gtk_box_append(GTK_BOX(ebox), entry);
    gtk_box_append(GTK_BOX(ebox), a->submit_label);
    g_signal_connect(entry, "changed", G_CALLBACK(on_entry_changed), a);
    g_signal_connect(entry, "activate", G_CALLBACK(on_entry_activate), a);
    gtk_box_append(GTK_BOX(col), section("② 텍스트 입력",
        "글자를 치면 키 하나마다 [입력] 키 누름 → [시그널] changed 가 나옵니다. "
        "한글은 입력기(IME)가 조합하므로 키 이벤트와 글자가 1:1 이 아닌 걸 볼 수 있어요.", ebox));

    /* 3. 토글 + 슬라이더 */
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    GtkWidget *check = gtk_check_button_new_with_label("체크박스");
    GtkWidget *sw = gtk_switch_new();
    gtk_widget_set_valign(sw, GTK_ALIGN_CENTER);
    GtkWidget *scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 100, 1);
    gtk_widget_set_hexpand(scale, TRUE);
    a->scale_label = gtk_label_new("값: 0");
    gtk_grid_attach(GTK_GRID(grid), check, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("스위치"), 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), sw, 2, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), scale, 0, 1, 3, 1);
    gtk_grid_attach(GTK_GRID(grid), a->scale_label, 3, 1, 1, 1);
    g_signal_connect(check, "toggled", G_CALLBACK(on_check_toggled), a);
    g_signal_connect(sw, "state-set", G_CALLBACK(on_switch_state_set), a);
    g_signal_connect(scale, "value-changed", G_CALLBACK(on_scale_changed), a);
    gtk_box_append(GTK_BOX(col), section("③ 체크박스 · 스위치 · 슬라이더",
        "슬라이더를 드래그하면 [입력] 은 누름/뗌 한 번뿐인데 [시그널] value-changed 는 여러 번 나옵니다. "
        "Tab 키로 포커스를 옮기고 방향키로도 조작해 보세요.", grid));

    /* 4. 그림판 */
    GtkWidget *cbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    a->canvas = gtk_drawing_area_new();
    gtk_widget_set_vexpand(a->canvas, TRUE);
    gtk_widget_set_size_request(a->canvas, -1, 160);
    gtk_widget_add_css_class(a->canvas, "canvas");
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(a->canvas), canvas_draw, a, NULL);
    GtkGesture *drag = gtk_gesture_drag_new();
    g_signal_connect(drag, "drag-begin", G_CALLBACK(on_drag_begin), a);
    g_signal_connect(drag, "drag-update", G_CALLBACK(on_drag_update), a);
    g_signal_connect(drag, "drag-end", G_CALLBACK(on_drag_end), a);
    gtk_widget_add_controller(a->canvas, GTK_EVENT_CONTROLLER(drag));
    GtkWidget *clear = gtk_button_new_with_label("그림판 지우기");
    gtk_widget_set_halign(clear, GTK_ALIGN_START);
    g_signal_connect(clear, "clicked", G_CALLBACK(on_clear_canvas), a);
    gtk_box_append(GTK_BOX(cbox), a->canvas);
    gtk_box_append(GTK_BOX(cbox), clear);
    GtkWidget *sec = section("④ 그림판 (GtkDrawingArea + cairo)",
        "드래그해서 그려보세요. 오른쪽 위 \"draw 호출 기록\" 을 켜면 "
        "마우스가 움직일 때마다 화면을 통째로 다시 그리는 게 보입니다.", cbox);
    gtk_widget_set_vexpand(sec, TRUE);
    gtk_box_append(GTK_BOX(col), sec);

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), col);
    return scroll;
}

static void legend_item(GtkWidget *grid, int row, const char *tag_class,
                        const char *label, const char *desc)
{
    GtkWidget *l = gtk_label_new(label);
    gtk_widget_add_css_class(l, tag_class);
    gtk_widget_add_css_class(l, "legend-tag");
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    GtkWidget *d = gtk_label_new(desc);
    gtk_widget_add_css_class(d, "hint");
    gtk_label_set_xalign(GTK_LABEL(d), 0);
    gtk_label_set_wrap(GTK_LABEL(d), TRUE);
    gtk_widget_set_hexpand(d, TRUE);
    gtk_grid_attach(GTK_GRID(grid), l, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), d, 1, row, 1, 1);
}

static GtkWidget *build_log_panel(App *a)
{
    GtkWidget *col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(col, 8);
    gtk_widget_set_margin_end(col, 16);
    gtk_widget_set_margin_top(col, 16);
    gtk_widget_set_margin_bottom(col, 16);

    GtkWidget *head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *title = gtk_label_new("이벤트 로그");
    gtk_widget_add_css_class(title, "section-title");
    gtk_widget_set_hexpand(title, TRUE);
    gtk_label_set_xalign(GTK_LABEL(title), 0);
    GtkWidget *clear = gtk_button_new_with_label("로그 지우기");
    g_signal_connect(clear, "clicked", G_CALLBACK(on_clear_log), a);
    gtk_box_append(GTK_BOX(head), title);
    gtk_box_append(GTK_BOX(head), clear);
    gtk_box_append(GTK_BOX(col), head);

    GtkWidget *legend = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(legend), 10);
    gtk_grid_set_row_spacing(GTK_GRID(legend), 4);
    legend_item(legend, 0, "tag-input", "[입력]",
        "GTK 가 디스플레이 서버(X11/Wayland)로부터 받은 날것의 이벤트. 어느 위젯 위였는지도 표시");
    legend_item(legend, 1, "tag-signal", "[시그널]",
        "위젯이 그 이벤트를 해석해 내보낸 의미 있는 신호. 앱 코드는 보통 여기에 콜백을 연결");
    legend_item(legend, 2, "tag-draw", "[그리기]",
        "위젯이 화면을 다시 그릴 때 호출되는 draw 콜백");
    gtk_box_append(GTK_BOX(col), legend);

    GtkWidget *opts = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
    a->log_raw_input = gtk_check_button_new_with_label("원시 입력 기록");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(a->log_raw_input), TRUE);
    a->log_motion = gtk_check_button_new_with_label("마우스 이동 기록 (많음!)");
    a->log_draw = gtk_check_button_new_with_label("draw 호출 기록");
    gtk_box_append(GTK_BOX(opts), a->log_raw_input);
    gtk_box_append(GTK_BOX(opts), a->log_motion);
    gtk_box_append(GTK_BOX(opts), a->log_draw);
    gtk_box_append(GTK_BOX(col), opts);

    a->log_view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(a->log_view), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(a->log_view), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(a->log_view), TRUE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(a->log_view), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(a->log_view), 8);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(a->log_view), 6);
    /* 시간 | 종류 | 내용 세 칸을 탭 위치로 정렬 */
    PangoTabArray *tabs = pango_tab_array_new_with_positions(2, TRUE,
        PANGO_TAB_LEFT, 84, PANGO_TAB_LEFT, 176);
    gtk_text_view_set_tabs(GTK_TEXT_VIEW(a->log_view), tabs);
    pango_tab_array_free(tabs);
    gtk_widget_add_css_class(a->log_view, "log");
    a->log_buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(a->log_view));
    gtk_text_buffer_create_tag(a->log_buf, "time", "foreground", "#8a8f98", NULL);
    gtk_text_buffer_create_tag(a->log_buf, "bold", "weight", PANGO_WEIGHT_BOLD, NULL);
    gtk_text_buffer_create_tag(a->log_buf, "input", "foreground", "#d9822b", NULL);
    gtk_text_buffer_create_tag(a->log_buf, "signal", "foreground", "#2b7bd9", NULL);
    gtk_text_buffer_create_tag(a->log_buf, "draw", "foreground", "#9b59b6", NULL);
    gtk_text_buffer_create_tag(a->log_buf, "info", "foreground", "#3a9a5b", NULL);
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(a->log_buf, &end);
    a->log_end = gtk_text_buffer_create_mark(a->log_buf, "end", &end, FALSE);

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), a->log_view);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_widget_add_css_class(scroll, "log-frame");
    gtk_box_append(GTK_BOX(col), scroll);
    return col;
}

static const char *CSS =
    ".section { padding: 12px; border-radius: 10px; background: alpha(currentColor, 0.04); }\n"
    ".section-title { font-size: 15pt; font-weight: bold; }\n"
    ".hint { opacity: 0.7; }\n"
    ".canvas { border: 1px solid alpha(currentColor, 0.25); }\n"
    ".log { font-size: 11pt; }\n"
    ".log-frame { border: 1px solid alpha(currentColor, 0.2); border-radius: 6px; }\n"
    ".legend-tag { font-weight: bold; font-family: monospace; }\n"
    ".tag-input { color: #d9822b; }\n"
    ".tag-signal { color: #2b7bd9; }\n"
    ".tag-draw { color: #9b59b6; }\n";

static void on_activate(GtkApplication *gapp, App *a)
{
    GtkCssProvider *css = gtk_css_provider_new();
#if GTK_CHECK_VERSION(4, 12, 0)
    gtk_css_provider_load_from_string(css, CSS);
#else
    gtk_css_provider_load_from_data(css, CSS, -1);
#endif
    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
        GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);

    a->window = gtk_application_window_new(gapp);
    gtk_window_set_title(GTK_WINDOW(a->window), "GTK 이벤트 실험실");
    gtk_window_set_default_size(GTK_WINDOW(a->window), 1200, 760);

    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_set_start_child(GTK_PANED(paned), build_playground(a));
    gtk_paned_set_end_child(GTK_PANED(paned), build_log_panel(a));
    gtk_paned_set_position(GTK_PANED(paned), 500);
    gtk_window_set_child(GTK_WINDOW(a->window), paned);

    /* 창 전체에 컨트롤러를 "캡처 단계"로 붙인다.
     * 이벤트는 창 → 부모 → ... → 실제 위젯 순으로 내려가므로(capture) 여기서 먼저 엿볼 수 있다 */
    GtkEventController *keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key_pressed), a);
    g_signal_connect(keys, "key-released", G_CALLBACK(on_key_released), a);
    gtk_widget_add_controller(a->window, keys);

    GtkGesture *click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), 0);   /* 0 = 모든 버튼 */
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(click), GTK_PHASE_CAPTURE);
    g_signal_connect(click, "pressed", G_CALLBACK(on_click_pressed), a);
    g_signal_connect(click, "released", G_CALLBACK(on_click_released), a);
    gtk_widget_add_controller(a->window, GTK_EVENT_CONTROLLER(click));

    GtkEventController *motion = gtk_event_controller_motion_new();
    gtk_event_controller_set_propagation_phase(motion, GTK_PHASE_CAPTURE);
    g_signal_connect(motion, "motion", G_CALLBACK(on_motion), a);
    gtk_widget_add_controller(a->window, motion);

    a->t0 = g_get_monotonic_time();
    GdkDisplay *dpy = gdk_display_get_default();
    app_log(a, K_INFO, "디스플레이 연결: %s  (GDK 백엔드: %s)", gdk_display_get_name(dpy),
            G_OBJECT_TYPE_NAME(dpy));
    app_log(a, K_INFO, "GTK %d.%d.%d — 왼쪽 위젯들을 조작해 보세요",
            gtk_get_major_version(), gtk_get_minor_version(), gtk_get_micro_version());

    gtk_window_present(GTK_WINDOW(a->window));
}

int main(int argc, char **argv)
{
    App a = {0};
    a.points = g_array_new(FALSE, FALSE, sizeof(graphene_point_t));

    /* GtkApplication 이 디스플레이 연결, 메인 루프(이벤트 루프)를 대신 돌려준다.
     * xlib-hello 의 for(;;) { XNextEvent(); } 가 g_application_run() 안에 숨어 있는 셈 */
    GtkApplication *gapp = gtk_application_new("dev.example.GtkLab", (GApplicationFlags)0);
    g_signal_connect(gapp, "activate", G_CALLBACK(on_activate), &a);
    int status = g_application_run(G_APPLICATION(gapp), argc, argv);

    g_object_unref(gapp);
    g_array_free(a.points, TRUE);
    return status;
}
