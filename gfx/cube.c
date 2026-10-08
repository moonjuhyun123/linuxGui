/*
 * cube : GPU 없이 CPU 로만 3D 정육면체를 그리는 "미니 렌더링 엔진"
 *
 * 그래픽스 엔진(그리고 GPU)이 매 프레임 하는 일을 C 로 하나하나 직접 한다.
 * GTK 는 창과 체크박스를 띄우는 데만 쓰고, 정육면체 그림은 전부 픽셀 배열(uint32_t[])에
 * 우리가 직접 숫자를 써서 만든다. 다 그린 배열을 마지막에 통째로 화면에 붙인다.
 *
 * 렌더링 파이프라인 (render_frame 이 위에서 아래로 그대로 따라간다)
 *
 *   ① 정점(vertex)    : 정육면체 = 꼭짓점 8개 + 삼각형 12개 (모든 3D 모델은 삼각형 덩어리)
 *   ② 모델 변환       : 회전·이동 행렬을 곱해 "물체 기준 좌표" → "세상 좌표"
 *   ③ 투영            : 3D 좌표 → 2D 화면 좌표. 원근 투영은 x, y 를 거리(w)로 나눈다
 *   ④ 뒷면 제거       : 화면에서 시계방향으로 보이는 삼각형 = 뒤를 보고 있음 → 버림
 *   ⑤ 래스터화        : 삼각형이 덮는 픽셀을 찾아낸다 (edge function)
 *   ⑥ 깊이 테스트     : 픽셀마다 "지금까지 그린 것보다 가까운가?" 를 z-buffer 로 판정
 *   ⑦ 셰이딩(색칠)    : 조명 계산 + 색 보간으로 픽셀 색 결정
 *
 * 빌드: make cube     실행: ./cube
 */
#include <gtk/gtk.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

/* ================================================================ 수학 */

typedef struct { float x, y, z; } Vec3;
typedef struct { float x, y, z, w; } Vec4;
typedef struct { float m[4][4]; } Mat4;   /* m[행][열] */

static Vec3 v3(float x, float y, float z) { return (Vec3){ x, y, z }; }
static Vec3 v3_sub(Vec3 a, Vec3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static float v3_dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static Vec3 v3_cross(Vec3 a, Vec3 b)
{
    return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
static Vec3 v3_norm(Vec3 a)
{
    float l = sqrtf(v3_dot(a, a));
    return l > 0 ? v3(a.x / l, a.y / l, a.z / l) : a;
}

static Mat4 m4_identity(void)
{
    Mat4 r = {0};
    for (int i = 0; i < 4; i++) r.m[i][i] = 1;
    return r;
}

static Mat4 m4_mul(Mat4 a, Mat4 b)
{
    Mat4 r = {0};
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            for (int k = 0; k < 4; k++)
                r.m[i][j] += a.m[i][k] * b.m[k][j];
    return r;
}

static Vec4 m4_apply(Mat4 a, Vec3 v)   /* (x, y, z, 1) 을 행렬에 곱함 */
{
    Vec4 r;
    r.x = a.m[0][0] * v.x + a.m[0][1] * v.y + a.m[0][2] * v.z + a.m[0][3];
    r.y = a.m[1][0] * v.x + a.m[1][1] * v.y + a.m[1][2] * v.z + a.m[1][3];
    r.z = a.m[2][0] * v.x + a.m[2][1] * v.y + a.m[2][2] * v.z + a.m[2][3];
    r.w = a.m[3][0] * v.x + a.m[3][1] * v.y + a.m[3][2] * v.z + a.m[3][3];
    return r;
}

static Mat4 m4_translate(float x, float y, float z)
{
    Mat4 r = m4_identity();
    r.m[0][3] = x; r.m[1][3] = y; r.m[2][3] = z;
    return r;
}

static Mat4 m4_scale(float s)
{
    Mat4 r = m4_identity();
    r.m[0][0] = r.m[1][1] = r.m[2][2] = s;
    return r;
}

static Mat4 m4_rot_x(float a)
{
    Mat4 r = m4_identity();
    r.m[1][1] = cosf(a); r.m[1][2] = -sinf(a);
    r.m[2][1] = sinf(a); r.m[2][2] = cosf(a);
    return r;
}

static Mat4 m4_rot_y(float a)
{
    Mat4 r = m4_identity();
    r.m[0][0] = cosf(a);  r.m[0][2] = sinf(a);
    r.m[2][0] = -sinf(a); r.m[2][2] = cosf(a);
    return r;
}

/* 원근 투영: 멀리 있는 것일수록 작게. 결과의 w 에 "카메라로부터의 거리"가 들어간다 */
static Mat4 m4_perspective(float fov_y, float aspect, float n, float f)
{
    float t = 1.0f / tanf(fov_y / 2);
    Mat4 r = {0};
    r.m[0][0] = t / aspect;
    r.m[1][1] = t;
    r.m[2][2] = (f + n) / (n - f);
    r.m[2][3] = 2 * f * n / (n - f);
    r.m[3][2] = -1;   /* w = -z : 나중에 x/w, y/w 로 나누는 게 "원근" 의 정체 */
    return r;
}

/* 직교 투영: 거리와 상관없이 같은 크기 (설계 도면, 2D 게임) */
static Mat4 m4_ortho(float half_h, float aspect, float n, float f)
{
    Mat4 r = m4_identity();
    r.m[0][0] = 1 / (half_h * aspect);
    r.m[1][1] = 1 / half_h;
    r.m[2][2] = -2 / (f - n);
    r.m[2][3] = -(f + n) / (f - n);
    return r;
}

/* ================================================================ 모델 데이터 */

/* ① 정육면체 꼭짓점 8개. 디자이너가 Blender 에서 만든 .glb 파일도 결국 이런 숫자 목록이다 */
static const Vec3 CUBE_VERTS[8] = {
    { -1, -1, -1 }, { 1, -1, -1 }, { 1, 1, -1 }, { -1, 1, -1 },
    { -1, -1,  1 }, { 1, -1,  1 }, { 1, 1,  1 }, { -1, 1,  1 },
};

/* 면 6개 × 삼각형 2개. 바깥에서 볼 때 반시계방향(CCW) 순서로 적는 게 약속 → 뒷면 판정에 쓰임 */
static const int CUBE_TRIS[12][3] = {
    { 4, 5, 6 }, { 4, 6, 7 },   /* 앞 (+z) */
    { 1, 0, 3 }, { 1, 3, 2 },   /* 뒤 (-z) */
    { 5, 1, 2 }, { 5, 2, 6 },   /* 오른쪽 (+x) */
    { 0, 4, 7 }, { 0, 7, 3 },   /* 왼쪽 (-x) */
    { 7, 6, 2 }, { 7, 2, 3 },   /* 위 (+y) */
    { 0, 1, 5 }, { 0, 5, 4 },   /* 아래 (-y) */
};

static const Vec3 FACE_COLORS[6] = {
    { 0.93f, 0.33f, 0.31f }, { 0.25f, 0.62f, 0.95f }, { 0.36f, 0.78f, 0.42f },
    { 0.98f, 0.76f, 0.18f }, { 0.67f, 0.45f, 0.92f }, { 0.96f, 0.55f, 0.22f },
};

/* ================================================================ 앱 상태 */

enum { MODE_POINTS, MODE_WIRE, MODE_FILL, MODE_FILL_WIRE };
enum { COLOR_FACE, COLOR_VERTEX, COLOR_DEPTH };

typedef struct {
    /* 프레임 버퍼: 화면 그 자체. 픽셀 하나 = 0x00RRGGBB */
    uint32_t *color;
    float *depth;          /* z-buffer: 픽셀마다 "지금까지 그린 것 중 가장 가까운 거리" */
    int w, h;

    /* 설정 (오른쪽 패널) */
    GtkWidget *perspective, *cull, *zbuffer, *lighting, *auto_rotate, *second_cube, *slowmo;
    GtkWidget *mode_dd, *color_dd, *pixel_scale;
    GtkWidget *stats;
    GtkWidget *area;

    float angle_x, angle_y, orbit;
    double drag_ax, drag_ay;
    gint64 last_frame_us;

    /* 슬로모션: 이번 프레임에 쓸 수 있는 픽셀 수 제한 → 그려지는 순서가 보인다 */
    long pixel_budget;
    long full_pixels;      /* 제한 없이 그렸을 때의 픽셀 수 (속도 맞추기용) */
    int hold_frames;

    /* 통계 */
    int st_verts, st_tris, st_culled, st_raster;
    long st_pixels, st_ztest_fail;
    double st_ms;
} App;

static gboolean on(GtkWidget *check) { return gtk_check_button_get_active(GTK_CHECK_BUTTON(check)); }

static uint32_t rgb(Vec3 c)
{
    int r = (int)(fminf(fmaxf(c.x, 0), 1) * 255 + 0.5f);
    int g = (int)(fminf(fmaxf(c.y, 0), 1) * 255 + 0.5f);
    int b = (int)(fminf(fmaxf(c.z, 0), 1) * 255 + 0.5f);
    return (uint32_t)(r << 16 | g << 8 | b);
}

/* ================================================================ 래스터화 */

/* 화면에 투영된 꼭짓점 하나 */
typedef struct {
    float x, y;       /* 화면 픽셀 좌표 */
    float z;          /* 깊이 (-1 가까움 ~ 1 멀리) */
    float inv_w;      /* 1/w — 원근 보정 보간용 */
    Vec3 color;       /* 정점 색 */
} ScreenVert;

/* edge function: 점 p 가 a→b 선분의 왼쪽(+)인지 오른쪽(-)인지.
 * 세 변 모두에 대해 같은 쪽이면 삼각형 안. GPU 래스터라이저도 본질적으로 이걸 한다 */
static float edge(float ax, float ay, float bx, float by, float px, float py)
{
    return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

/* ⑤ 래스터화 + ⑥ 깊이 테스트 + ⑦ 셰이딩 */
static void raster_triangle(App *a, const ScreenVert *v0, const ScreenVert *v1, const ScreenVert *v2,
                            Vec3 face_color, float light, int color_mode, gboolean ztest)
{
    float area = edge(v0->x, v0->y, v1->x, v1->y, v2->x, v2->y);
    if (fabsf(area) < 1e-6f) return;

    /* 삼각형을 감싸는 사각형 안의 픽셀만 검사 */
    int minx = (int)floorf(fminf(v0->x, fminf(v1->x, v2->x)));
    int maxx = (int)ceilf(fmaxf(v0->x, fmaxf(v1->x, v2->x)));
    int miny = (int)floorf(fminf(v0->y, fminf(v1->y, v2->y)));
    int maxy = (int)ceilf(fmaxf(v0->y, fmaxf(v1->y, v2->y)));
    if (minx < 0) minx = 0;
    if (miny < 0) miny = 0;
    if (maxx > a->w - 1) maxx = a->w - 1;
    if (maxy > a->h - 1) maxy = a->h - 1;

    float sign = area < 0 ? -1.0f : 1.0f;   /* 뒷면 제거를 끄면 뒤집힌 삼각형도 그려야 하므로 */
    float inv_area = 1.0f / fabsf(area);

    for (int y = miny; y <= maxy; y++) {
        for (int x = minx; x <= maxx; x++) {
            float px = x + 0.5f, py = y + 0.5f;   /* 픽셀 중심 */
            float w0 = edge(v1->x, v1->y, v2->x, v2->y, px, py) * sign;
            float w1 = edge(v2->x, v2->y, v0->x, v0->y, px, py) * sign;
            float w2 = edge(v0->x, v0->y, v1->x, v1->y, px, py) * sign;
            if (w0 < 0 || w1 < 0 || w2 < 0) continue;   /* 삼각형 밖 */

            /* 무게중심 좌표(barycentric): 이 픽셀이 세 꼭짓점에 얼마나 가까운지 (합 = 1) */
            float b0 = w0 * inv_area, b1 = w1 * inv_area, b2 = w2 * inv_area;
            float z = b0 * v0->z + b1 * v1->z + b2 * v2->z;

            size_t i = (size_t)y * a->w + x;
            if (ztest && z >= a->depth[i]) {   /* ⑥ 이미 더 가까운 게 그려져 있음 → 버림 */
                a->st_ztest_fail++;
                continue;
            }
            if (a->pixel_budget >= 0 && a->st_pixels >= a->pixel_budget) return;

            Vec3 c;
            if (color_mode == COLOR_VERTEX) {
                /* 원근 보정 보간: 화면에서 선형으로 섞으면 3D 에선 휘어 보이므로 1/w 로 보정 */
                float q0 = b0 * v0->inv_w, q1 = b1 * v1->inv_w, q2 = b2 * v2->inv_w;
                float qs = q0 + q1 + q2;
                c = v3((q0 * v0->color.x + q1 * v1->color.x + q2 * v2->color.x) / qs,
                       (q0 * v0->color.y + q1 * v1->color.y + q2 * v2->color.y) / qs,
                       (q0 * v0->color.z + q1 * v1->color.z + q2 * v2->color.z) / qs);
            } else {
                c = face_color;
            }
            c = v3(c.x * light, c.y * light, c.z * light);

            a->color[i] = rgb(c);
            a->depth[i] = z;
            a->st_pixels++;
        }
    }
}

/* 브레젠험 직선: 정수 덧셈만으로 선 긋기 (와이어프레임용).
 * ztest 면 선도 깊이 버퍼와 비교해서, 면 뒤에 숨은 선은 그리지 않는다 */
static void draw_line(App *a, const ScreenVert *p, const ScreenVert *q, uint32_t col, gboolean ztest)
{
    int x0 = (int)p->x, y0 = (int)p->y, x1 = (int)q->x, y1 = (int)q->y;
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    int steps = dx > -dy ? dx : -dy;
    for (int k = 0; k <= steps; k++) {
        if (x0 >= 0 && x0 < a->w && y0 >= 0 && y0 < a->h) {
            size_t i = (size_t)y0 * a->w + x0;
            float t = steps ? (float)k / steps : 0;
            float z = p->z + (q->z - p->z) * t;
            if (!ztest || z <= a->depth[i] + 6e-4f) a->color[i] = col;
        }
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static void draw_dot(App *a, int cx, int cy, int r, uint32_t col)
{
    for (int y = cy - r; y <= cy + r; y++)
        for (int x = cx - r; x <= cx + r; x++)
            if (x >= 0 && x < a->w && y >= 0 && y < a->h)
                a->color[(size_t)y * a->w + x] = col;
}

/* ================================================================ 한 프레임 렌더링 */

static void draw_cube(App *a, Mat4 model, Mat4 proj, Vec3 tint)
{
    int mode = (int)gtk_drop_down_get_selected(GTK_DROP_DOWN(a->mode_dd));
    int color_mode = (int)gtk_drop_down_get_selected(GTK_DROP_DOWN(a->color_dd));

    /* ② 모델 변환 + ③ 투영 : 꼭짓점 8개를 화면 좌표로 */
    ScreenVert sv[8];
    Vec3 world[8];
    gboolean behind = FALSE;
    for (int i = 0; i < 8; i++) {
        Vec4 wv = m4_apply(model, CUBE_VERTS[i]);
        world[i] = v3(wv.x, wv.y, wv.z);
        Vec4 clip = m4_apply(proj, world[i]);
        if (clip.w < 0.1f) behind = TRUE;   /* 카메라 뒤 (진짜 엔진은 여기서 삼각형을 잘라낸다=clipping) */

        /* 원근 나눗셈: x/w, y/w → 멀리 있을수록(w 큼) 화면 중심으로 모인다 */
        float ndc_x = clip.x / clip.w, ndc_y = clip.y / clip.w, ndc_z = clip.z / clip.w;
        /* 뷰포트 변환: -1..1 → 0..너비 픽셀. y 는 화면에서 아래로 증가하므로 뒤집는다 */
        sv[i].x = (ndc_x + 1) * 0.5f * a->w;
        sv[i].y = (1 - ndc_y) * 0.5f * a->h;
        sv[i].z = ndc_z;
        sv[i].inv_w = 1.0f / clip.w;
        Vec3 c = v3((CUBE_VERTS[i].x + 1) / 2, (CUBE_VERTS[i].y + 1) / 2, (CUBE_VERTS[i].z + 1) / 2);
        sv[i].color = v3(c.x * tint.x, c.y * tint.y, c.z * tint.z);
        a->st_verts++;
    }
    if (behind) return;

    Vec3 light_dir = v3_norm(v3(-0.4f, 0.6f, 0.7f));
    gboolean visible[12];

    for (int t = 0; t < 12; t++) {
        const int *idx = CUBE_TRIS[t];
        const ScreenVert *p0 = &sv[idx[0]], *p1 = &sv[idx[1]], *p2 = &sv[idx[2]];
        a->st_tris++;

        /* ④ 뒷면 제거: 화면에서 감긴 방향(넓이의 부호)으로 앞/뒤를 판정.
         * 화면 y 가 뒤집혀 있으므로 앞면(CCW)은 여기서 음수가 된다 */
        float area = edge(p0->x, p0->y, p1->x, p1->y, p2->x, p2->y);
        visible[t] = area < 0;
        if (on(a->cull) && !visible[t]) {
            a->st_culled++;
            continue;
        }
        visible[t] = TRUE;
        if (mode == MODE_POINTS || mode == MODE_WIRE) continue;

        /* ⑦ 조명 (램버트): 면이 빛을 정면으로 볼수록 밝다 = 법선·빛방향 내적 */
        Vec3 n = v3_norm(v3_cross(v3_sub(world[idx[1]], world[idx[0]]),
                                  v3_sub(world[idx[2]], world[idx[0]])));
        float light = 1.0f;
        if (on(a->lighting)) light = 0.35f + 0.65f * fmaxf(0, v3_dot(n, light_dir));

        Vec3 fc = FACE_COLORS[t / 2];
        fc = v3(fc.x * tint.x, fc.y * tint.y, fc.z * tint.z);
        raster_triangle(a, p0, p1, p2, fc, light, color_mode, on(a->zbuffer));
        a->st_raster++;
    }

    /* 와이어프레임: 삼각형 테두리. 사각형 면도 사실은 삼각형 2개라는 게 보인다 */
    if (mode == MODE_WIRE || mode == MODE_FILL_WIRE) {
        uint32_t col = mode == MODE_WIRE ? 0xE6EDF3 : 0x111111;
        for (int t = 0; t < 12; t++) {
            if (!visible[t]) continue;
            for (int e = 0; e < 3; e++) {
                const ScreenVert *p = &sv[CUBE_TRIS[t][e]], *q = &sv[CUBE_TRIS[t][(e + 1) % 3]];
                draw_line(a, p, q, col, mode == MODE_FILL_WIRE && on(a->zbuffer));
            }
        }
    }
    if (mode == MODE_POINTS) {
        int r = a->w > 300 ? 3 : 1;
        for (int i = 0; i < 8; i++) draw_dot(a, (int)sv[i].x, (int)sv[i].y, r, 0xFFD54F);
    }
}

static void render_frame(App *a)
{
    gint64 t0 = g_get_monotonic_time();
    a->st_verts = a->st_tris = a->st_culled = a->st_raster = 0;
    a->st_pixels = a->st_ztest_fail = 0;

    /* 화면 지우기: 색 버퍼는 배경색, 깊이 버퍼는 "무한히 멀다" 로 */
    size_t n = (size_t)a->w * a->h;
    for (size_t i = 0; i < n; i++) {
        a->color[i] = 0x1B1F27;
        a->depth[i] = INFINITY;
    }

    float aspect = (float)a->w / a->h;
    Mat4 proj = on(a->perspective) ? m4_perspective(1.0f, aspect, 0.1f, 50.0f)
                                   : m4_ortho(2.4f, aspect, 0.1f, 50.0f);

    /* 카메라는 원점에서 -z 방향을 본다. 물체를 z=-5 로 밀어 카메라 앞에 둔다 */
    Mat4 model = m4_mul(m4_translate(0, 0, -6.0f),
                        m4_mul(m4_rot_y(a->angle_y), m4_mul(m4_rot_x(a->angle_x), m4_scale(1.15f))));
    draw_cube(a, model, proj, v3(1, 1, 1));

    if (on(a->second_cube)) {
        /* 큰 정육면체를 관통하며 도는 작은 정육면체 → 깊이 버퍼가 왜 필요한지 보여줌 */
        Mat4 m2 = m4_mul(m4_translate(1.6f * cosf(a->orbit), 0.4f * sinf(a->orbit * 2), -6.0f + 1.6f * sinf(a->orbit)),
                         m4_mul(m4_rot_x(a->orbit * 1.7f), m4_mul(m4_rot_y(a->orbit * 2.3f), m4_scale(0.55f))));
        draw_cube(a, m2, proj, v3(0.95f, 0.95f, 0.95f));
    }

    /* 깊이 버퍼 시각화: 이번 프레임의 가장 가까운 값 ~ 가장 먼 값을 흰색 ~ 어두운 회색으로 */
    if (gtk_drop_down_get_selected(GTK_DROP_DOWN(a->color_dd)) == COLOR_DEPTH) {
        float zmin = INFINITY, zmax = -INFINITY;
        for (size_t i = 0; i < n; i++) {
            if (isinf(a->depth[i])) continue;
            zmin = fminf(zmin, a->depth[i]);
            zmax = fmaxf(zmax, a->depth[i]);
        }
        float range = zmax > zmin ? zmax - zmin : 1;
        for (size_t i = 0; i < n; i++) {
            if (isinf(a->depth[i])) { a->color[i] = 0; continue; }
            float d = 1.0f - 0.8f * (a->depth[i] - zmin) / range;
            a->color[i] = rgb(v3(d, d, d));
        }
    }

    a->st_ms = (g_get_monotonic_time() - t0) / 1000.0;
}

/* ================================================================ GTK 연결 */

static void update_stats(App *a)
{
    char *s = g_strdup_printf(
        "해상도        %d × %d  (%'d 픽셀)\n"
        "정점 변환      %d 개\n"
        "삼각형         %d 개\n"
        "  └ 뒷면 제거   %d 개\n"
        "  └ 래스터화    %d 개\n"
        "픽셀 칠함      %'ld\n"
        "깊이 테스트 탈락 %'ld\n"
        "CPU 렌더 시간  %.2f ms",
        a->w, a->h, a->w * a->h, a->st_verts, a->st_tris, a->st_culled, a->st_raster,
        a->st_pixels, a->st_ztest_fail, a->st_ms);
    gtk_label_set_text(GTK_LABEL(a->stats), s);
    g_free(s);
}

static void ensure_buffers(App *a, int w, int h)
{
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (w == a->w && h == a->h) return;
    g_free(a->color);
    g_free(a->depth);
    a->w = w;
    a->h = h;
    a->color = g_new0(uint32_t, (size_t)w * h);
    a->depth = g_new0(float, (size_t)w * h);
}

static void draw_func(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer data)
{
    App *a = data;
    (void)area;
    int scale = (int)gtk_range_get_value(GTK_RANGE(a->pixel_scale));
    ensure_buffers(a, width / scale, height / scale);

    render_frame(a);

    /* 우리가 칠한 픽셀 배열을 그대로 이미지로 감싸서 화면에 붙인다.
     * 확대할 때 NEAREST 필터 → 픽셀 하나하나가 네모로 보인다 */
    cairo_surface_t *img = cairo_image_surface_create_for_data(
        (unsigned char *)a->color, CAIRO_FORMAT_RGB24, a->w, a->h, a->w * 4);
    cairo_scale(cr, (double)width / a->w, (double)height / a->h);
    cairo_set_source_surface(cr, img, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
    cairo_paint(cr);
    cairo_surface_destroy(img);
    /* 주의: 여기(그리기 중)서 라벨 글자를 바꾸면 레이아웃이 꼬인다 → 통계는 on_tick 에서 갱신 */
}

/* 게임 루프의 한 박자: 화면 주사율(보통 60Hz)마다 GTK 가 불러준다 */
static gboolean on_tick(GtkWidget *w, GdkFrameClock *clock, gpointer data)
{
    App *a = data;
    gint64 now = gdk_frame_clock_get_frame_time(clock);
    float dt = a->last_frame_us ? (now - a->last_frame_us) / 1e6f : 0;
    if (dt > 0.1f) dt = 0.1f;
    a->last_frame_us = now;

    if (on(a->slowmo)) {
        /* 회전을 멈추고, 매 프레임 칠할 수 있는 픽셀 수를 조금씩 늘린다 */
        long total = (long)a->w * a->h;
        long base = a->full_pixels > 0 ? a->full_pixels / 100 : total / 300;   /* 약 100 프레임에 걸쳐 */
        long step = base > 0 ? base : 1;
        if (a->pixel_budget < 0) a->pixel_budget = 0;
        if (a->st_pixels < a->pixel_budget) {          /* 다 그렸음 → 잠깐 멈췄다가 처음부터 */
            a->full_pixels = a->st_pixels;
            if (++a->hold_frames > 60) { a->pixel_budget = 0; a->hold_frames = 0; }
        } else {
            a->pixel_budget += step;
        }
    } else {
        a->pixel_budget = -1;
        if (on(a->auto_rotate)) {
            a->angle_y += 0.8f * dt;
            a->angle_x += 0.5f * dt;
        }
        if (on(a->second_cube)) a->orbit += 0.9f * dt;
    }

    update_stats(a);   /* 직전 프레임의 통계 */
    gtk_widget_queue_draw(w);
    return G_SOURCE_CONTINUE;
}

static void on_drag_begin(GtkGestureDrag *g, double x, double y, App *a)
{
    (void)g; (void)x; (void)y;
    a->drag_ax = a->angle_x;
    a->drag_ay = a->angle_y;
}

static void on_drag_update(GtkGestureDrag *g, double dx, double dy, App *a)
{
    (void)g;
    a->angle_y = (float)(a->drag_ay + dx * 0.01);
    a->angle_x = (float)(a->drag_ax + dy * 0.01);
}

/* ---------------------------------------------------------------- 오른쪽 패널 */

static GtkWidget *stage(GtkWidget *box, const char *title, const char *desc)
{
    GtkWidget *t = gtk_label_new(title);
    gtk_widget_add_css_class(t, "stage-title");
    gtk_label_set_xalign(GTK_LABEL(t), 0);
    gtk_box_append(GTK_BOX(box), t);
    if (desc) {
        GtkWidget *d = gtk_label_new(desc);
        gtk_widget_add_css_class(d, "hint");
        gtk_label_set_xalign(GTK_LABEL(d), 0);
        gtk_label_set_wrap(GTK_LABEL(d), TRUE);
        gtk_label_set_max_width_chars(GTK_LABEL(d), 40);
        gtk_box_append(GTK_BOX(box), d);
    }
    return t;
}

static GtkWidget *check(GtkWidget *box, const char *label, gboolean active)
{
    GtkWidget *c = gtk_check_button_new_with_label(label);
    gtk_check_button_set_active(GTK_CHECK_BUTTON(c), active);
    gtk_box_append(GTK_BOX(box), c);
    return c;
}

static GtkWidget *build_panel(App *a)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_start(box, 16);
    gtk_widget_set_margin_end(box, 16);
    gtk_widget_set_margin_top(box, 12);
    gtk_widget_set_margin_bottom(box, 16);

    GtkWidget *title = gtk_label_new("렌더링 파이프라인");
    gtk_widget_add_css_class(title, "panel-title");
    gtk_label_set_xalign(GTK_LABEL(title), 0);
    gtk_box_append(GTK_BOX(box), title);
    GtkWidget *flow = gtk_label_new("정점 → 변환 → 투영 → 뒷면 제거 → 래스터화 → 깊이 테스트 → 색칠");
    gtk_widget_add_css_class(flow, "hint");
    gtk_label_set_wrap(GTK_LABEL(flow), TRUE);
    gtk_label_set_xalign(GTK_LABEL(flow), 0);
    gtk_box_append(GTK_BOX(box), flow);

    stage(box, "① 그리기 방식", "모든 3D 모델은 꼭짓점과 삼각형으로 되어 있습니다. 점 → 선 → 면 순서로 바꿔 보세요.");
    a->mode_dd = gtk_drop_down_new_from_strings((const char *const[]){
        "점만 (정점 8개)", "와이어프레임 (삼각형 테두리)", "면 채우기", "면 + 테두리", NULL });
    gtk_drop_down_set_selected(GTK_DROP_DOWN(a->mode_dd), MODE_FILL);
    gtk_box_append(GTK_BOX(box), a->mode_dd);

    stage(box, "② 변환", "회전 행렬을 매 프레임 꼭짓점에 곱합니다. 화면을 드래그해도 돌아갑니다.");
    a->auto_rotate = check(box, "자동 회전", TRUE);
    a->second_cube = check(box, "작은 정육면체 추가 (큰 것을 관통)", FALSE);

    stage(box, "③ 투영", "끄면 직교 투영: 먼 면도 같은 크기로 보여 입체감이 사라집니다.");
    a->perspective = check(box, "원근 투영 (x, y 를 거리로 나누기)", TRUE);

    stage(box, "④ 뒷면 제거", "뒤를 향한 삼각형은 어차피 안 보이니 버립니다. 통계에서 절반이 버려지는 걸 확인하세요.");
    a->cull = check(box, "뒷면 제거 (back-face culling)", TRUE);

    stage(box, "⑤ 래스터화", "삼각형이 덮는 픽셀을 하나씩 찾아 칠합니다. 픽셀을 키우면 계단 모양이 보입니다.");
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(row), gtk_label_new("픽셀 크기"));
    a->pixel_scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 1, 16, 1);
    gtk_range_set_value(GTK_RANGE(a->pixel_scale), 2);
    gtk_scale_set_draw_value(GTK_SCALE(a->pixel_scale), TRUE);
    gtk_widget_set_hexpand(a->pixel_scale, TRUE);
    gtk_box_append(GTK_BOX(row), a->pixel_scale);
    gtk_box_append(GTK_BOX(box), row);
    a->slowmo = check(box, "슬로모션: 칠하는 순서 보기", FALSE);

    stage(box, "⑥ 깊이 테스트", "뒷면 제거와 깊이 버퍼를 둘 다 끄면, 늦게 그린 뒷면이 앞면을 덮어버립니다.");
    a->zbuffer = check(box, "깊이 버퍼 (z-buffer)", TRUE);

    stage(box, "⑦ 셰이딩 (색칠)", NULL);
    a->lighting = check(box, "조명 (빛을 정면으로 받을수록 밝게)", TRUE);
    a->color_dd = gtk_drop_down_new_from_strings((const char *const[]){
        "면마다 단색", "꼭짓점 색을 섞기 (보간)", "깊이 버퍼를 흑백으로 보기", NULL });
    gtk_box_append(GTK_BOX(box), a->color_dd);

    GtkWidget *st = gtk_label_new("이번 프레임 통계");
    gtk_widget_add_css_class(st, "stage-title");
    gtk_label_set_xalign(GTK_LABEL(st), 0);
    gtk_widget_set_margin_top(st, 10);
    gtk_box_append(GTK_BOX(box), st);
    a->stats = gtk_label_new("");
    gtk_widget_add_css_class(a->stats, "stats");
    gtk_label_set_xalign(GTK_LABEL(a->stats), 0);
    gtk_label_set_width_chars(GTK_LABEL(a->stats), 36);
    gtk_box_append(GTK_BOX(box), a->stats);

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), box);
    gtk_widget_set_size_request(scroll, 380, -1);
    gtk_widget_set_hexpand(scroll, FALSE);   /* 안쪽 슬라이더의 hexpand 가 위로 번져 패널이 넓어지는 걸 막음 */
    return scroll;
}

static const char *CSS =
    ".panel-title { font-size: 16pt; font-weight: bold; }\n"
    ".stage-title { font-size: 12pt; font-weight: bold; margin-top: 8px; }\n"
    ".hint { opacity: 0.7; font-size: 9.5pt; }\n"
    ".stats { font-family: monospace; font-size: 10.5pt; padding: 8px;"
    "  border-radius: 6px; background: alpha(currentColor, 0.06); }\n";

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

    GtkWidget *win = gtk_application_window_new(gapp);
    gtk_window_set_title(GTK_WINDOW(win), "소프트웨어 렌더러 — 정육면체");
    gtk_window_set_default_size(GTK_WINDOW(win), 1200, 780);

    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    a->area = gtk_drawing_area_new();
    gtk_widget_set_hexpand(a->area, TRUE);
    gtk_widget_set_vexpand(a->area, TRUE);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(a->area), draw_func, a, NULL);

    GtkGesture *drag = gtk_gesture_drag_new();
    g_signal_connect(drag, "drag-begin", G_CALLBACK(on_drag_begin), a);
    g_signal_connect(drag, "drag-update", G_CALLBACK(on_drag_update), a);
    gtk_widget_add_controller(a->area, GTK_EVENT_CONTROLLER(drag));

    gtk_box_append(GTK_BOX(hbox), a->area);
    gtk_box_append(GTK_BOX(hbox), build_panel(a));
    gtk_window_set_child(GTK_WINDOW(win), hbox);

    a->angle_x = 0.5f;
    a->angle_y = 0.6f;
    a->pixel_budget = -1;
    /* 매 프레임 호출 = 게임 루프. GUI 앱의 "이벤트 올 때까지 잠들기" 와 다른 점 */
    gtk_widget_add_tick_callback(a->area, on_tick, a, NULL);

    gtk_window_present(GTK_WINDOW(win));
}

int main(int argc, char **argv)
{
    App a = {0};
    GtkApplication *gapp = gtk_application_new("dev.example.SoftCube", (GApplicationFlags)0);
    g_signal_connect(gapp, "activate", G_CALLBACK(on_activate), &a);
    int status = g_application_run(G_APPLICATION(gapp), argc, argv);
    g_object_unref(gapp);
    g_free(a.color);
    g_free(a.depth);
    return status;
}
