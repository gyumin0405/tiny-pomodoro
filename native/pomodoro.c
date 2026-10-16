// 포모도로 타이머 - 순수 Win32 C 버전 (C 런타임 없이 빌드)
// C# 버전과 기능/디자인 동일. 최적화 포인트:
// - 할 일이 없으면 타이머가 하나도 돌지 않음 (마우스 hover는 이벤트로 감지)
// - 실행 중엔 표시되는 초가 바뀌는 순간에만 깨어나서 다시 그림
// - 투명도 애니메이션 타이머는 애니메이션 중에만 동작
// - 이모지는 exe에 내장한 48px 이미지를 시작할 때 한 번만 축소해 둠
// 오른쪽 클릭 → 화면 전환으로 "인터넷 차단" 화면: 지금부터 N시간 동안 크롬/엣지의 웹페이지를
// 브라우저 정책으로 막음. 해제는 SYSTEM 예약 작업이 하므로 앱을 꺼도, 재부팅해도 제때 풀림
#define WIN32_LEAN_AND_MEAN
#define UNICODE
#include <windows.h>
#include <shellapi.h>
#include <dwmapi.h>
#include "emoji_data.h"

// -nostdlib 이라 컴파일러가 생성하는 memset/memcpy 호출을 직접 제공
void *memset(void *d, int c, size_t n) { unsigned char *p = d; while (n--) *p++ = (unsigned char)c; return d; }
void *memcpy(void *d, const void *s, size_t n) { unsigned char *p = d; const unsigned char *q = s; while (n--) *p++ = *q++; return d; }

#define RGBX(r, g, b) ((COLORREF)((r) | ((g) << 8) | ((b) << 16)))
#define BG_COLOR    RGBX(30, 30, 32)
#define BG_FLASH    RGBX(70, 70, 76)
#define FOCUS_COLOR RGBX(235, 94, 85)
#define BREAK_COLOR RGBX(76, 194, 110)
#define TEXT_COLOR  RGBX(240, 240, 240)
#define DIM_TEXT    RGBX(150, 150, 155)
#define BAR_BG      RGBX(55, 55, 60)
#define BLOCK_COLOR RGBX(98, 150, 235)

#define IDLE_ALPHA 64   // 25% 불투명 = 75% 투명
#define T_TICK  1
#define T_FADE  2
#define T_BLINK 3
#define T_TOP   4
#define T_BLOCK 5   // 차단 남은 시간
#define T_PROC  6   // 관리자 권한 PowerShell이 끝났는지 확인 (적용 중에만)

#define LONG_EVERY 4   // 공부 4회마다 긴 휴식 (Cirillo 원래 방식, Pomofocus 등 기본값)

#define BLOCK_STEP 30          // 차단 시간 조절 단위 (분)
#define BLOCK_MAX  (12 * 60)   // 최대 차단 시간 (분)

enum { FOCUS, BREAK, LONG_BREAK };
enum { CMD_25 = 1, CMD_50, CMD_LONG, CMD_SWITCH, CMD_RESET, CMD_HOME, CMD_EXIT, CMD_VIEW, CMD_UNBLOCK };
enum { VIEW_TIMER, VIEW_BLOCK };
enum { Z_NONE, Z_MINUS, Z_PLUS, Z_START };   // 차단 화면에서 마우스가 가리키는 곳

static HWND hwnd;
static int dpi, W, H, pad, emoSize, topY;
static int focusMin = 25, breakMin = 5, longMin = 15, phase = FOCUS;
static int longOn = 1, doneCount;   // doneCount: 이번 세트에서 끝까지 마친 공부 횟수
static int running, finished, blinkOn;
static LONGLONG remainMs;
static ULONGLONG endTick;
static int alpha = IDLE_ALPHA, hover, menuOpen, mouseDown, dragging;
static POINT downPos, downLoc;
static HFONT fontSmall, fontTime, fontMid;
static int ascSmall, ascTime, ascMid;   // 글꼴별 기준선 위치 (큰 숫자 옆에 작은 글씨를 나란히 쓸 때)
static HDC memDC;
static DWORD *pix;
static DWORD emoji[4][EMOJI_SRC * EMOJI_SRC];   // 0=불, 1=커피, 2=일시정지, 3=금지 (화면 크기로 축소된 것)
static WCHAR iniPath[MAX_PATH], errLogPath[MAX_PATH];   // 설정 파일, 차단 실패 로그
static int view = VIEW_TIMER, zone;
static int blockMins = 120;
static LONGLONG blockStart, blockEnd;   // 차단 시작/끝 (1601년부터 초, 벽시계 기준이라 재시작해도 유지)
static HANDLE applyProc;                // 실행 중인 관리자 권한 PowerShell
static int applyBlock;                  // 1=차단 적용 중, 0=해제 적용 중

static int S(int v) { return (v * dpi + 48) / 96; }
static int Abs(int v) { return v < 0 ? -v : v; }
static int PhaseMin(void) { return phase == FOCUS ? focusMin : phase == BREAK ? breakMin : longMin; }
static LONGLONG PhaseMs(void) { return (LONGLONG)PhaseMin() * 60000; }

static WCHAR *PutInt(WCHAR *p, LONGLONG v)
{
    WCHAR tmp[24]; int n = 0;
    if (v < 0) { *p++ = L'-'; v = -v; }
    do { tmp[n++] = (WCHAR)(L'0' + v % 10); v /= 10; } while (v);
    while (n) *p++ = tmp[--n];
    return p;
}

static WCHAR *PutStr(WCHAR *p, const WCHAR *s) { while (*s) *p++ = *s++; return p; }
// PowerShell 작은따옴표 문자열 안에 넣을 때 (' 는 '' 로)
static WCHAR *PutQuoted(WCHAR *p, const WCHAR *s) { for (; *s; s++) { if (*s == L'\'') *p++ = L'\''; *p++ = *s; } return p; }

// ---------- 이모지: 48px 원본을 박스 필터로 n px로 축소 ----------

static void ScaleEmoji(const unsigned int *src, DWORD *dst, int n)
{
    const int s = EMOJI_SRC;
    for (int dy = 0; dy < n; dy++)
        for (int dx = 0; dx < n; dx++) {
            unsigned acc[4] = { 0, 0, 0, 0 };
            int y0 = dy * s / n, y1 = ((dy + 1) * s + n - 1) / n;
            int x0 = dx * s / n, x1 = ((dx + 1) * s + n - 1) / n;
            for (int sy = y0; sy < y1; sy++) {
                int wy = (sy * n + n < dy * s + s ? sy * n + n : dy * s + s) - (sy * n > dy * s ? sy * n : dy * s);
                for (int sx = x0; sx < x1; sx++) {
                    int wx = (sx * n + n < dx * s + s ? sx * n + n : dx * s + s) - (sx * n > dx * s ? sx * n : dx * s);
                    unsigned c = src[sy * s + sx], w = (unsigned)(wx * wy);
                    acc[0] += (c & 255) * w; acc[1] += (c >> 8 & 255) * w;
                    acc[2] += (c >> 16 & 255) * w; acc[3] += (c >> 24) * w;
                }
            }
            unsigned t = (unsigned)(s * s), h = t / 2;
            dst[dy * n + dx] = (acc[0] + h) / t | (acc[1] + h) / t << 8 | (acc[2] + h) / t << 16 | (acc[3] + h) / t << 24;
        }
}

static void BlendEmoji(int which, int x0, int y0)
{
    const DWORD *src = emoji[which];
    for (int y = 0; y < emoSize; y++) {
        if (y0 + y < 0 || y0 + y >= H) continue;
        for (int x = 0; x < emoSize; x++) {
            DWORD s = src[y * emoSize + x], a = s >> 24;
            if (!a || x0 + x < 0 || x0 + x >= W) continue;
            DWORD *d = &pix[(y0 + y) * W + x0 + x];
            if (a == 255) { *d = s; continue; }
            DWORD inv = 255 - a, dv = *d;
            DWORD r = (s >> 16 & 255) + ((dv >> 16 & 255) * inv + 127) / 255;
            DWORD g = (s >> 8 & 255) + ((dv >> 8 & 255) * inv + 127) / 255;
            DWORD b = (s & 255) + ((dv & 255) * inv + 127) / 255;
            *d = r << 16 | g << 8 | b;
        }
    }
}

// ---------- 그리기 ----------

static void Fill(int x, int y, int w, int h, COLORREF c)
{
    RECT r = { x, y, x + w, y + h };
    SetDCBrushColor(memDC, c);
    FillRect(memDC, &r, (HBRUSH)GetStockObject(DC_BRUSH));
}

static int TextW(HFONT f, const WCHAR *s, int n)
{
    SIZE sz; SelectObject(memDC, f); GetTextExtentPoint32W(memDC, s, n, &sz); return sz.cx;
}

static void Text(HFONT f, COLORREF c, int x, int y, const WCHAR *s, int n)
{
    SelectObject(memDC, f); SetTextColor(memDC, c); TextOutW(memDC, x, y, s, n);
}

static void RenderTimer(void)
{
    COLORREF accent = phase == FOCUS ? FOCUS_COLOR : BREAK_COLOR;
    int gp = S(11) / 6;              // GDI+ DrawString의 좌우 여백을 흉내 내서 C# 버전과 같은 위치에
    int right = W - pad, y = topY - S(1);
    int pausedOrIdle = !running && !finished;

    Fill(0, 0, W, H, finished && blinkOn ? BG_FLASH : BG_COLOR);

    // 상단: 🔥 공부 25분 2/4
    WCHAR buf[16], *p = PutStr(buf, phase == FOCUS ? L"공부 " : phase == BREAK ? L"휴식 " : L"긴 휴식 ");
    if (finished) p = PutStr(p, L"끝!");
    else { p = PutInt(p, PhaseMin()); *p++ = L'분'; }
    int lx = pad + emoSize + S(2) + gp;
    Text(fontSmall, accent, lx, y, buf, (int)(p - buf));
    if (longOn && phase == FOCUS) {   // 이번 세트에서 몇 번째 공부인지
        int lw = TextW(fontSmall, buf, (int)(p - buf));
        WCHAR c[8], *q = PutInt(c, finished ? doneCount : doneCount + 1);
        *q++ = L'/'; q = PutInt(q, LONG_EVERY);
        Text(fontSmall, DIM_TEXT, lx + lw + S(4), y, c, (int)(q - c));
    }

    if (finished) {
        int aw = TextW(fontSmall, L"→", 1);
        Text(fontSmall, TEXT_COLOR, right - emoSize - aw + S(2), y, L"→", 1);
    } else if (pausedOrIdle && remainMs == PhaseMs()) {
        int aw = TextW(fontSmall, L"▶", 1);
        Text(fontSmall, DIM_TEXT, right - aw - gp + S(2), y, L"▶", 1);
    }

    // 가운데: 남은 시간
    int sec = (int)((remainMs + 999) / 1000);
    WCHAR t[5] = { (WCHAR)(L'0' + sec / 600), (WCHAR)(L'0' + sec / 60 % 10), L':',
                   (WCHAR)(L'0' + sec % 60 / 10), (WCHAR)(L'0' + sec % 10) };
    int tw = TextW(fontTime, t, 5);
    Text(fontTime, pausedOrIdle ? DIM_TEXT : TEXT_COLOR, (W - tw) / 2, S(21), t, 5);

    // 하단: 진행 바
    int barH = S(3);
    LONGLONG total = PhaseMs();
    Fill(0, H - barH, W, barH, BAR_BG);
    Fill(0, H - barH, (int)((total - remainMs) * W / total), barH, accent);

    // 이모지는 GDI가 아니라 픽셀 버퍼에 직접 알파 블렌딩
    GdiFlush();
    BlendEmoji(phase == FOCUS ? 0 : 1, pad, y);
    if (finished) BlendEmoji(phase == FOCUS ? 1 : 0, right - emoSize, y);
    else if (pausedOrIdle && remainMs != PhaseMs()) BlendEmoji(2, right - emoSize, y);
}

// ---------- 인터넷 차단 화면 ----------

static LONGLONG NowMs(void)
{
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    return (LONGLONG)(((ULONGLONG)ft.dwHighDateTime << 32 | ft.dwLowDateTime) / 10000);
}

static LONGLONG BlockLeftMs(void)
{
    LONGLONG r = blockEnd * 1000 - NowMs();
    return blockEnd && r > 0 ? r : 0;
}

static void RenderBlock(void)
{
    int gp = S(11) / 6, right = W - pad, y = topY - S(1);
    LONGLONG left = BlockLeftMs();
    int lx = pad + emoSize + S(2) + gp, mid = S(21);

    Fill(0, 0, W, H, BG_COLOR);

    // 상단: 🚫 인터넷 차단 / 차단 중  ……  15:30까지
    const WCHAR *label = applyProc ? (applyBlock ? L"차단 적용 중…" : L"해제 중…") : left ? L"차단 중" : L"인터넷 차단";
    Text(fontSmall, BLOCK_COLOR, lx, y, label, lstrlenW(label));

    if (left && !applyProc) {
        // 끝나는 시각 (현지 시간)
        LONGLONG e = blockEnd * 10000000; FILETIME ft = { (DWORD)e, (DWORD)(e >> 32) }, lt; SYSTEMTIME st;
        FileTimeToLocalFileTime(&ft, &lt); FileTimeToSystemTime(&lt, &st);
        WCHAR b[12] = { (WCHAR)(L'0' + st.wHour / 10), (WCHAR)(L'0' + st.wHour % 10), L':',
                        (WCHAR)(L'0' + st.wMinute / 10), (WCHAR)(L'0' + st.wMinute % 10), L'까', L'지' };
        int bw = TextW(fontSmall, b, 7);
        Text(fontSmall, DIM_TEXT, right - bw - gp + S(2), y, b, 7);

        // 가운데: 남은 시간 H:MM:SS
        int sec = (int)((left + 999) / 1000), hh = sec / 3600;
        WCHAR t[12], *p = PutInt(t, hh);
        *p++ = L':'; *p++ = (WCHAR)(L'0' + sec / 600 % 6); *p++ = (WCHAR)(L'0' + sec / 60 % 10);
        *p++ = L':'; *p++ = (WCHAR)(L'0' + sec % 60 / 10); *p++ = (WCHAR)(L'0' + sec % 10);
        int n = (int)(p - t), tw = TextW(fontTime, t, n);
        Text(fontTime, TEXT_COLOR, (W - tw) / 2, mid, t, n);
    } else {
        if (!applyProc) {
            int aw = TextW(fontSmall, L"▶", 1);
            Text(fontSmall, zone == Z_START ? TEXT_COLOR : DIM_TEXT, right - aw - gp + S(2), y, L"▶", 1);
        }
        // 가운데:  −   1.5시간   +   ("시간"은 작은 글씨로 기준선 맞춤)
        // 30분씩 바꿀 때 글자가 좌우로 흔들리지 않게 자리를 고정: 정수는 오른쪽 끝을 맞추고,
        // 그 뒤 ".5" 자리는 항상 비워 둠 (Segoe UI 숫자는 폭이 같아서 1→2도 제자리)
        WCHAR num[4]; int nn = (int)(PutInt(num, blockMins / 60) - num);
        int iw = TextW(fontTime, L"0", 1) * 3 / 2, hw = TextW(fontTime, L".5", 2);   // 숫자 1.5자리 기준 가운데 (−/+ 와 양쪽 여백이 비슷하게)
        int uw = TextW(fontSmall, L"시간", 2), gap = S(2);
        int ir = (W - iw - hw - gap - uw) / 2 + iw;   // 정수 부분의 오른쪽 끝
        COLORREF c = applyProc ? DIM_TEXT : zone == Z_START ? TEXT_COLOR : DIM_TEXT;
        Text(fontTime, c, ir - TextW(fontTime, num, nn), mid, num, nn);
        if (blockMins % 60) Text(fontTime, c, ir, mid, L".5", 2);
        Text(fontSmall, c, ir + hw + gap, mid + ascTime - ascSmall, L"시간", 2);
        if (!applyProc) {
            // −/+ 의 가운데를 숫자 높이의 가운데에 맞춤 (Segoe UI: 숫자 높이 ≈ 0.7em, 연산자 가운데 ≈ 0.33em)
            int my = mid + ascTime - S(26) * 35 / 100 + S(18) * 33 / 100 - ascMid;
            Text(fontMid, zone == Z_MINUS ? TEXT_COLOR : blockMins > BLOCK_STEP ? DIM_TEXT : BAR_BG, pad + S(6), my, L"−", 1);
            int pw = TextW(fontMid, L"+", 1);
            Text(fontMid, zone == Z_PLUS ? TEXT_COLOR : blockMins < BLOCK_MAX ? DIM_TEXT : BAR_BG, right - pw - S(6), my, L"+", 1);
        }
    }

    // 하단: 진행 바
    int barH = S(3);
    Fill(0, H - barH, W, barH, BAR_BG);
    if (left && blockEnd > blockStart) {
        LONGLONG total = (blockEnd - blockStart) * 1000;
        Fill(0, H - barH, (int)((total - left) * W / total), barH, BLOCK_COLOR);
    }

    GdiFlush();
    BlendEmoji(3, pad, y);
}

static void Render(void)
{
    if (view == VIEW_BLOCK) RenderBlock(); else RenderTimer();
}

// ---------- 설정 저장 ----------

static void SaveSettings(void)
{
    RECT r; GetWindowRect(hwnd, &r);
    char buf[192]; WCHAR w[192], *p = w;
    p = PutStr(p, L"mode="); p = PutInt(p, focusMin);
    p = PutStr(p, L"\r\nlong="); p = PutInt(p, longOn);
    p = PutStr(p, L"\r\nview="); p = PutInt(p, view);
    p = PutStr(p, L"\r\ndur="); p = PutInt(p, blockMins);
    p = PutStr(p, L"\r\nstart="); p = PutInt(p, blockStart);
    p = PutStr(p, L"\r\nblockend="); p = PutInt(p, blockEnd);
    p = PutStr(p, L"\r\nx="); p = PutInt(p, r.left);
    p = PutStr(p, L"\r\ny="); p = PutInt(p, r.top);
    p = PutStr(p, L"\r\n");
    int n = (int)(p - w);
    for (int i = 0; i < n; i++) buf[i] = (char)w[i];
    WCHAR dir[MAX_PATH]; int len = lstrlenW(iniPath);
    lstrcpyW(dir, iniPath); while (len && dir[len] != L'\\') len--; dir[len] = 0;
    CreateDirectoryW(dir, NULL);
    HANDLE f = CreateFileW(iniPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD wr; WriteFile(f, buf, (DWORD)n, &wr, NULL); CloseHandle(f);
}

static int LoadSettings(POINT *pt)
{
    char buf[256]; DWORD rd = 0;
    HANDLE f = CreateFileW(iniPath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    ReadFile(f, buf, sizeof buf - 1, &rd, NULL); CloseHandle(f);
    buf[rd] = 0;
    int hasX = 0, hasY = 0;
    for (char *s = buf; *s; ) {
        char key = *s; while (*s && *s != '=' && *s != '\n') s++;
        if (*s == '=') {
            s++; int neg = *s == '-', ok = 0; LONGLONG v = 0; if (neg) s++;
            while (*s >= '0' && *s <= '9') { v = v * 10 + *s++ - '0'; ok = 1; }
            if (neg) v = -v;
            if (ok && key == 'm' && v == 50) { focusMin = 50; breakMin = 10; longMin = 30; }
            if (ok && key == 'l') longOn = v != 0;
            if (ok && key == 'v') view = v == VIEW_BLOCK ? VIEW_BLOCK : VIEW_TIMER;
            if (ok && key == 'h' && v >= 1 && v <= BLOCK_MAX / 60) blockMins = (int)v * 60;   // 예전 설정 (시간 단위)
            if (ok && key == 'd' && v >= BLOCK_STEP && v <= BLOCK_MAX && v % BLOCK_STEP == 0) blockMins = (int)v;
            if (ok && key == 's') blockStart = v;
            if (ok && key == 'b') blockEnd = v;
            if (ok && key == 'x') { pt->x = (int)v; hasX = 1; }
            if (ok && key == 'y') { pt->y = (int)v; hasY = 1; }
        }
        while (*s && *s != '\n') s++;
        if (*s) s++;
    }
    return hasX && hasY;
}

static POINT DefaultLocation(void)
{
    RECT wa; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    POINT p = { wa.left + S(12), wa.bottom - H - S(12) };
    return p;
}

// ---------- 상태 전환 ----------

static void Redraw(void) { InvalidateRect(hwnd, NULL, FALSE); }
static void SetView(int v);

static void UpdateFade(void)
{
    int target = (hover || finished || menuOpen || dragging) ? 255 : IDLE_ALPHA;
    if (target != alpha) SetTimer(hwnd, T_FADE, 30, NULL);
}

// WS_EX_TOPMOST여도 일부 앱(Zed 등)이 앞으로 오면 그 아래로 밀릴 때가 있음 → 다시 맨 위로
static void BringToTop(void)
{
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

// 활성 창이 바뀔 때만 호출됨 (폴링 없음). 상대 앱이 늦게 z-order를 바꾸는 경우를 위해 잠시 뒤 한 번 더
static void CALLBACK OnForeground(HWINEVENTHOOK hk, DWORD ev, HWND w, LONG obj, LONG child, DWORD tid, DWORD t)
{
    if (w == hwnd || menuOpen) return;
    BringToTop();
    SetTimer(hwnd, T_TOP, 300, NULL);
}

static void ScheduleTick(void)
{
    // 표시되는 초(올림)가 바뀌는 순간에 맞춰 한 번만 깨어남
    SetTimer(hwnd, T_TICK, (UINT)((remainMs - 1) % 1000) + 2, NULL);
}

static void ResetPhase(void)
{
    running = finished = 0;
    remainMs = PhaseMs();
    KillTimer(hwnd, T_TICK); KillTimer(hwnd, T_BLINK);
    UpdateFade(); Redraw();
}

static void Start(void)
{
    endTick = GetTickCount64() + (ULONGLONG)remainMs;
    running = 1; ScheduleTick(); Redraw();
}

static void Pause(void)
{
    LONGLONG r = (LONGLONG)(endTick - GetTickCount64());
    remainMs = r < 0 ? 0 : r;
    running = 0; KillTimer(hwnd, T_TICK); Redraw();
}

static void SwitchPhase(int autoStart)
{
    if (phase == FOCUS) phase = longOn && doneCount >= LONG_EVERY ? LONG_BREAK : BREAK;
    else {
        if (phase == LONG_BREAK) doneCount = 0;   // 긴 휴식이 지나면 새 세트
        phase = FOCUS;
    }
    ResetPhase();
    if (autoStart) Start();
}

static void SetMode(int f, int b, int l)
{
    focusMin = f; breakMin = b; longMin = l;
    ResetPhase(); SaveSettings();
}

static void ToggleLong(void)
{
    longOn = !longOn; doneCount = 0;
    if (!longOn && phase == LONG_BREAK) { phase = BREAK; ResetPhase(); }
    Redraw(); SaveSettings();
}

static void OnTick(void)
{
    LONGLONG r = (LONGLONG)(endTick - GetTickCount64());
    if (r <= 0) {
        remainMs = 0; running = 0; finished = 1; blinkOn = 1;
        if (longOn && phase == FOCUS && doneCount < LONG_EVERY) doneCount++;
        KillTimer(hwnd, T_TICK);
        SetTimer(hwnd, T_BLINK, 600, NULL);
        MessageBeep(MB_ICONEXCLAMATION);
        if (view == VIEW_BLOCK) SetView(VIEW_TIMER);   // 차단 화면을 보고 있어도 끝난 걸 알 수 있게
        UpdateFade();
    } else {
        remainMs = r; ScheduleTick();
    }
    Redraw();
}

static void OnFade(void)
{
    int target = (hover || finished || menuOpen || dragging) ? 255 : IDLE_ALPHA;
    int d = (target - alpha) / 4;
    if (!d) d = target > alpha ? 1 : -1;
    alpha += d;
    if (Abs(target - alpha) < 3) alpha = target;
    SetLayeredWindowAttributes(hwnd, 0, (BYTE)alpha, LWA_ALPHA);
    if (alpha == target) KillTimer(hwnd, T_FADE);
}

static void CheckHover(void)
{
    POINT p; RECT r; GetCursorPos(&p); GetWindowRect(hwnd, &r);
    hover = PtInRect(&r, p);
    if (hover) { TRACKMOUSEEVENT t = { sizeof t, TME_LEAVE, hwnd, 0 }; TrackMouseEvent(&t); }
    UpdateFade();
}

// ---------- 인터넷 차단 (브라우저 정책) ----------
// 크롬/엣지의 관리 정책(URLBlocklist = *)으로 웹페이지를 막음. 브라우저 안에서만 적용되므로
// 다른 앱(AI 앱, Claude Code 등)은 그대로 인터넷을 씀. 이미 열린 탭은 그대로 보이고,
// 내 PC 파일(file://, PDF 뷰어)은 허용 목록에 있어서 브라우저를 PDF 리더로 계속 쓸 수 있음.
// 엣지는 Microsoft 계정으로 로그인한 프로필엔 URLBlocklist를 적용하지 않으므로(MS 문서), 그런 프로필에도
// 적용되는 ProxySettings로 엣지의 프록시를 닫힌 포트로 돌려서 웹 요청이 모두 실패하게 함 (로컬 파일은 영향 없음).
// 정책은 HKLM 레지스트리라 관리자 권한이 필요해서 UAC 확인 후 숨은 PowerShell로 처리하고,
// 해제는 끝나는 시각에 SYSTEM 예약 작업이 직접 함 (앱이 꺼져 있거나 그 사이 재부팅해도 해제됨)

#define BLOCK_TASK L"PomodoroUnblock"
// 이 앱이 정책을 썼다는 표시. 이게 있을 때만 정책을 지움 (다른 곳에서 설정한 정책은 건드리지 않음)
#define BLOCK_MARK_KEY L"SOFTWARE\\TinyPomodoro"
#define BLOCK_MARK L"HKLM:\\" BLOCK_MARK_KEY
#define BLOCK_BASES L"'HKLM:\\SOFTWARE\\Policies\\Google\\Chrome','HKLM:\\SOFTWARE\\Policies\\Microsoft\\Edge'"
#define EDGE_KEY L"'HKLM:\\SOFTWARE\\Policies\\Microsoft\\Edge'"

// 해제 스크립트: 이 앱이 쓴 정책과, 그 때문에 생긴 빈 정책 키를 지움 (빈 키가 남으면 '조직에서 관리' 표시가
// 남을 수 있음). 예전 버전(방화벽 방식)이 남긴 규칙도 정리. 본문 끝에 exit가 없어야 차단 실패 시
// 되돌리기용으로 불러도 스크립트가 거기서 끝나지 않음
#define UNBLOCK_BODY \
    L"$m='" BLOCK_MARK L"';" \
    L"if(Test-Path $m){Remove-ItemProperty " EDGE_KEY L" -Name ProxySettings -EA 0;" \
    L"foreach($b in " BLOCK_BASES L"){" \
    L"Remove-Item \"$b\\URLBlocklist\",\"$b\\URLAllowlist\" -Recurse -EA 0;" \
    L"$k=Get-Item $b -EA 0;if($k -and !$k.ValueCount -and !$k.SubKeyCount){Remove-Item $b -EA 0}};" \
    L"Remove-Item $m -EA 0};" \
    L"Remove-NetFirewallRule -Group 'PomodoroBlock' -EA 0;" \
    L"Unregister-ScheduledTask -TaskName '" BLOCK_TASK L"' -Confirm:$false -EA 0"
static const WCHAR UNBLOCK_BODY_W[] = UNBLOCK_BODY;
static const WCHAR UNBLOCK_PS[] = UNBLOCK_BODY L";exit 0";   // 지금 차단 해제

static WCHAR *PutB64(WCHAR *p, const BYTE *d, int n)
{
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (int i = 0; i < n; i += 3) {
        unsigned v = (unsigned)d[i] << 16 | (i + 1 < n ? (unsigned)d[i + 1] << 8 : 0) | (i + 2 < n ? d[i + 2] : 0);
        *p++ = (WCHAR)t[v >> 18]; *p++ = (WCHAR)t[v >> 12 & 63];
        *p++ = i + 1 < n ? (WCHAR)t[v >> 6 & 63] : L'=';
        *p++ = i + 2 < n ? (WCHAR)t[v & 63] : L'=';
    }
    return p;
}

// 스크립트를 관리자 권한 PowerShell로 숨겨서 실행. UAC에서 취소하면 0
static int RunElevated(const WCHAR *script)
{
    // 정적 버퍼는 exe 크기를 키우므로 (CRT 없이 빌드해서 .bss가 없음) 쓸 때만 힙에서
    int n = lstrlenW(script);
    WCHAR *args = HeapAlloc(GetProcessHeap(), 0, (SIZE_T)(n * 8 / 3 + 256) * sizeof(WCHAR));
    if (!args) return 0;
    WCHAR *p = PutStr(args, L"-NoProfile -NonInteractive -ExecutionPolicy Bypass -WindowStyle Hidden -EncodedCommand ");
    p = PutB64(p, (const BYTE *)script, n * 2);   // -EncodedCommand는 UTF-16LE의 base64
    *p = 0;
    SHELLEXECUTEINFOW se = { sizeof se };
    se.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    se.hwnd = hwnd; se.lpVerb = L"runas"; se.lpFile = L"powershell.exe";
    se.lpParameters = args; se.nShow = SW_HIDE;
    BOOL ok = ShellExecuteExW(&se);
    DWORD err = GetLastError();
    HeapFree(GetProcessHeap(), 0, args);
    if (!ok || !se.hProcess) {
        if (err != ERROR_CANCELLED)
            MessageBoxW(hwnd, L"관리자 권한으로 PowerShell을 실행하지 못했습니다.", L"인터넷 차단", MB_ICONERROR | MB_TOPMOST);
        return 0;
    }
    applyProc = se.hProcess;
    SetTimer(hwnd, T_PROC, 250, NULL);
    zone = Z_NONE; Redraw();
    return 1;
}

static void ScheduleBlockTick(void)
{
    LONGLONG left = BlockLeftMs();
    if (!left) { KillTimer(hwnd, T_BLOCK); return; }
    // 차단 화면이 보일 땐 초가 바뀔 때마다, 안 보일 땐 끝나는 순간에만 깨어남
    SetTimer(hwnd, T_BLOCK, view == VIEW_BLOCK ? (UINT)((left - 1) % 1000) + 2 : (UINT)left + 50, NULL);
}

// 차단 표시 키가 실제로 있는지 (읽기는 관리자 권한 없이 가능). 없으면 이미 풀린 것
static int BlockMarkerExists(void)
{
    HKEY k;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, BLOCK_MARK_KEY, 0, KEY_READ | KEY_WOW64_64KEY, &k) != ERROR_SUCCESS) return 0;
    RegCloseKey(k);
    return 1;
}

// 끝났거나, 다른 경로(수동 해제 등)로 이미 풀렸으면 화면도 풀린 상태로. 해제 자체는 예약 작업/스크립트가 함
static void CheckBlockState(void)
{
    if (blockEnd && (!BlockLeftMs() || !BlockMarkerExists())) { blockStart = blockEnd = 0; SaveSettings(); }
}

static void OnBlockTick(void)
{
    CheckBlockState();
    ScheduleBlockTick();
    if (view == VIEW_BLOCK) Redraw();
}

static void StartBlock(void)
{
    WCHAR msg[200], *m = PutStr(msg, L"지금부터 ");
    if (blockMins >= 60) { m = PutInt(m, blockMins / 60); m = PutStr(m, L"시간"); if (blockMins % 60) *m++ = L' '; }
    if (blockMins % 60) { m = PutInt(m, blockMins % 60); *m++ = L'분'; }
    m = PutStr(m, L" 동안 크롬·엣지에서 웹페이지를 막습니다.\n"
                  L"열려 있는 탭은 그대로 두고, 내 PC의 파일(PDF 등)은 계속 열 수 있습니다.\n"
                  L"다른 앱의 인터넷은 영향 없습니다.\n\n"
                  L"계속할까요? (관리자 권한 확인 창이 뜹니다)");
    *m = 0;
    if (MessageBoxW(hwnd, msg, L"인터넷 차단", MB_OKCANCEL | MB_ICONQUESTION | MB_TOPMOST) != IDOK) return;

    DeleteFileW(errLogPath);

    // 해제 스크립트는 예약 작업과 실패 시 되돌리기에 같이 쓰도록 base64로 넣어 둠
    int un = lstrlenW(UNBLOCK_BODY_W);
    WCHAR *ps = HeapAlloc(GetProcessHeap(), 0, (SIZE_T)(un * 8 / 3 + 3072 + 2 * MAX_PATH) * sizeof(WCHAR));
    if (!ps) return;
    WCHAR *p = PutStr(ps, L"$e='");
    p = PutB64(p, (const BYTE *)UNBLOCK_BODY_W, un * 2);
    p = PutStr(p, L"';$ErrorActionPreference='Stop';try{"
        L"$n='" BLOCK_TASK L"';$m='" BLOCK_MARK L"';$bs=" BLOCK_BASES L";"
        // 다른 곳에서 이미 설정한 차단/허용 목록이 있으면 건드리지 않음
        L"if(!(Test-Path $m)){foreach($b in $bs){if((Test-Path \"$b\\URLBlocklist\") -or (Test-Path \"$b\\URLAllowlist\")){exit 2}};"
        L"if($null -ne (Get-ItemProperty " EDGE_KEY L" -Name ProxySettings -EA 0)){exit 2}};"
        L"New-Item $m -Force|Out-Null;"
        L"foreach($b in $bs){"
        L"Remove-Item \"$b\\URLBlocklist\",\"$b\\URLAllowlist\" -Recurse -EA 0;"
        L"$k=New-Item \"$b\\URLBlocklist\" -Force;New-ItemProperty $k.PSPath -Name '1' -Value '*'|Out-Null;"
        L"$k=New-Item \"$b\\URLAllowlist\" -Force;$i=1;"
        // 내 PC 파일, 브라우저 설정 화면, 내장 PDF 뷰어(확장)는 허용
        L"foreach($u in 'file://*','chrome://*','edge://*','chrome-extension://*','extension://*','about:blank'){"
        L"New-ItemProperty $k.PSPath -Name \"$i\" -Value $u|Out-Null;$i++}};"
        // 엣지: Microsoft 계정 프로필에도 먹는 프록시 정책으로 웹 요청을 닫힌 포트(9)로 보냄
        L"Set-ItemProperty " EDGE_KEY L" -Name ProxySettings -Type String"
        L" -Value '{\"ProxyMode\":\"fixed_servers\",\"ProxyServer\":\"127.0.0.1:9\"}';"
        L"$a=New-ScheduledTaskAction -Execute 'powershell.exe' -Argument \"-NoProfile -WindowStyle Hidden -EncodedCommand $e\";"
        L"$t=New-ScheduledTaskTrigger -Once -At (Get-Date).AddMinutes(");
    p = PutInt(p, blockMins);
    p = PutStr(p, L");"
        L"$s=New-ScheduledTaskSettingsSet -StartWhenAvailable -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries;"
        L"Register-ScheduledTask -TaskName $n -Action $a -Trigger $t -Settings $s -User 'SYSTEM' -RunLevel Highest -Force|Out-Null;"
        L"exit 0"
        // 실패: 원인을 로그로 남기고(앱이 보여 줌), 되돌리기는 오류가 나도 끝까지 가도록 Continue로
        L"}catch{$ErrorActionPreference='Continue';"
        L"try{[IO.File]::WriteAllText('");
    p = PutQuoted(p, errLogPath);
    p = PutStr(p, L"',(($_|Out-String)+$_.InvocationInfo.PositionMessage))}catch{};"
        L"& ([scriptblock]::Create([Text.Encoding]::Unicode.GetString([Convert]::FromBase64String($e))));exit 1}");
    *p = 0;
    applyBlock = 1;
    RunElevated(ps);
    HeapFree(GetProcessHeap(), 0, ps);
}

static void StopBlock(void)
{
    if (MessageBoxW(hwnd, L"인터넷 차단을 지금 해제할까요?", L"인터넷 차단",
                    MB_OKCANCEL | MB_ICONQUESTION | MB_DEFBUTTON2 | MB_TOPMOST) != IDOK) return;
    applyBlock = 0;
    RunElevated(UNBLOCK_PS);
}

// 관리자 권한 PowerShell이 끝나면 결과 반영
static void OnProc(void)
{
    if (WaitForSingleObject(applyProc, 0) == WAIT_TIMEOUT) return;
    DWORD code = 1; GetExitCodeProcess(applyProc, &code);
    CloseHandle(applyProc); applyProc = NULL;
    KillTimer(hwnd, T_PROC);
    if (code == 0) {
        if (applyBlock) { blockStart = NowMs() / 1000; blockEnd = blockStart + (LONGLONG)blockMins * 60; }
        else blockStart = blockEnd = 0;
        SaveSettings();
    } else if (!applyBlock || code == 2) {
        MessageBoxW(hwnd, !applyBlock ? L"차단을 해제하지 못했습니다."
                          : L"크롬·엣지에 이미 다른 URL 차단 정책이 설정돼 있어서 건드리지 않았습니다.",
                    L"인터넷 차단", MB_ICONERROR | MB_TOPMOST);
    } else {
        // 스크립트가 남긴 오류 로그(UTF-8)를 앞부분만 보여 줌
        WCHAR *msg = HeapAlloc(GetProcessHeap(), 0, 2048 * sizeof(WCHAR));
        if (msg) {
            WCHAR *q = PutStr(msg, L"브라우저 차단 정책을 설정하지 못했습니다. (종료 코드 ");
            q = PutInt(q, (LONGLONG)code); *q++ = L')';
            char raw[1536]; DWORD rd = 0;
            HANDLE f = CreateFileW(errLogPath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
            if (f != INVALID_HANDLE_VALUE) { ReadFile(f, raw, sizeof raw, &rd, NULL); CloseHandle(f); }
            if (rd) {
                q = PutStr(q, L"\n\n");
                q += MultiByteToWideChar(CP_UTF8, 0, raw, (int)rd, q, (int)(msg + 2000 - q));
                q = PutStr(q, L"\n\n(전체 로그: %APPDATA%\\Pomodoro\\block_error.log)");
            }
            *q = 0;
            MessageBoxW(hwnd, msg, L"인터넷 차단", MB_ICONERROR | MB_TOPMOST);
            HeapFree(GetProcessHeap(), 0, msg);
        }
    }
    ScheduleBlockTick();
    Redraw();
}

static void SetView(int v)
{
    view = v; zone = Z_NONE;
    if (!applyProc) CheckBlockState();
    ScheduleBlockTick(); Redraw(); SaveSettings();
}

static void AddMins(int d)
{
    int v = blockMins + d;
    if (v < BLOCK_STEP || v > BLOCK_MAX || BlockLeftMs() || applyProc) return;
    blockMins = v; Redraw(); SaveSettings();
}

// 차단 화면에서 마우스 위치 → 영역 (차단 중이거나 적용 중이면 누를 곳 없음)
static int ZoneAt(int x, int y)
{
    if (view != VIEW_BLOCK || applyProc || BlockLeftMs() || x < 0) return Z_NONE;
    if (y >= S(20) && x < S(36)) return Z_MINUS;
    if (y >= S(20) && x >= W - S(36)) return Z_PLUS;
    return Z_START;
}

static void UpdateZone(int x, int y)
{
    int z = ZoneAt(x, y);
    if (z != zone) { zone = z; Redraw(); }
}

static void BlockClick(int x, int y)
{
    int z = ZoneAt(x, y);
    if (z == Z_MINUS) AddMins(-BLOCK_STEP);
    else if (z == Z_PLUS) AddMins(BLOCK_STEP);
    else if (z == Z_START) StartBlock();
}

static void ShowMenu(void)
{
    HMENU m = CreatePopupMenu();
    if (view == VIEW_BLOCK) {
        AppendMenuW(m, MF_STRING, CMD_VIEW, L"화면 전환 (포모도로)");
        AppendMenuW(m, MF_SEPARATOR, 0, NULL);
        AppendMenuW(m, MF_STRING | (applyProc ? MF_GRAYED : 0), CMD_UNBLOCK, L"지금 차단 해제");
        AppendMenuW(m, MF_STRING, CMD_HOME, L"위치 초기화 (왼쪽 아래)");
        AppendMenuW(m, MF_SEPARATOR, 0, NULL);
        AppendMenuW(m, MF_STRING, CMD_EXIT, L"종료");
    } else {
    AppendMenuW(m, MF_STRING, CMD_VIEW, BlockLeftMs() ? L"화면 전환 (인터넷 차단 중)" : L"화면 전환 (인터넷 차단)");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING | (focusMin == 25 ? MF_CHECKED : 0), CMD_25, L"25분 공부 / 5분 휴식");
    AppendMenuW(m, MF_STRING | (focusMin == 50 ? MF_CHECKED : 0), CMD_50, L"50분 공부 / 10분 휴식");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING | (longOn ? MF_CHECKED : 0), CMD_LONG,
                longMin == 30 ? L"4회마다 긴 휴식 (30분)" : L"4회마다 긴 휴식 (15분)");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, CMD_SWITCH, L"공부 ↔ 휴식 전환");
    AppendMenuW(m, MF_STRING, CMD_RESET, L"리셋");
    AppendMenuW(m, MF_STRING, CMD_HOME, L"위치 초기화 (왼쪽 아래)");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, CMD_EXIT, L"종료");
    }

    POINT p; GetCursorPos(&p);
    menuOpen = 1; UpdateFade();
    SetForegroundWindow(hwnd);   // 메뉴 밖을 클릭했을 때 메뉴가 닫히도록
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, p.x, p.y, 0, hwnd, NULL);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);
    menuOpen = 0;

    switch (cmd) {
    case CMD_25: SetMode(25, 5, 15); break;
    case CMD_50: SetMode(50, 10, 30); break;
    case CMD_LONG: ToggleLong(); break;
    case CMD_SWITCH: SwitchPhase(0); break;
    case CMD_RESET: ResetPhase(); break;
    case CMD_HOME: { POINT d = DefaultLocation(); SetWindowPos(hwnd, NULL, d.x, d.y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE); SaveSettings(); } break;
    case CMD_VIEW: SetView(view == VIEW_BLOCK ? VIEW_TIMER : VIEW_BLOCK); break;
    case CMD_UNBLOCK: StopBlock(); break;
    case CMD_EXIT: DestroyWindow(hwnd); return;
    }
    CheckHover();
    // 메뉴가 끌어온 메모리(테마, 메뉴 비트맵 등)를 돌려줌
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
}

// ---------- 윈도우 프로시저 ----------

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        Render();
        BitBlt(dc, 0, 0, W, H, memDC, 0, 0, SRCCOPY);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_TIMER:
        if (wp == T_TICK) OnTick();
        else if (wp == T_FADE) OnFade();
        else if (wp == T_BLINK) { blinkOn = !blinkOn; Redraw(); }
        else if (wp == T_TOP) { KillTimer(h, T_TOP); BringToTop(); }
        else if (wp == T_BLOCK) OnBlockTick();
        else if (wp == T_PROC) OnProc();
        return 0;
    case WM_MOUSEMOVE:
        if (!hover) { hover = 1; TRACKMOUSEEVENT t = { sizeof t, TME_LEAVE, h, 0 }; TrackMouseEvent(&t); UpdateFade(); }
        if (mouseDown) {
            POINT p; GetCursorPos(&p);
            int dx = p.x - downPos.x, dy = p.y - downPos.y;
            if (!dragging && (Abs(dx) > S(4) || Abs(dy) > S(4))) dragging = 1;
            if (dragging) SetWindowPos(h, NULL, downLoc.x + dx, downLoc.y + dy, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        UpdateZone(dragging ? -1 : (short)LOWORD(lp), (short)HIWORD(lp));
        return 0;
    case WM_MOUSELEAVE:
        hover = 0; UpdateFade(); UpdateZone(-1, 0);
        return 0;
    case WM_LBUTTONDOWN: {
        RECT r; GetWindowRect(h, &r);
        mouseDown = 1; dragging = 0;
        GetCursorPos(&downPos); downLoc.x = r.left; downLoc.y = r.top;
        SetCapture(h);
        return 0;
    }
    case WM_LBUTTONUP:
        if (mouseDown) {
            int wasDrag = dragging;
            mouseDown = dragging = 0;
            ReleaseCapture();
            if (wasDrag) SaveSettings();
            else if (view == VIEW_BLOCK) BlockClick((short)LOWORD(lp), (short)HIWORD(lp));
            else if (finished) SwitchPhase(1);   // 끝난 뒤 클릭 → 다음 단계로 전환하고 바로 시작
            else if (running) Pause();
            else Start();
            CheckHover();
        }
        return 0;
    case WM_CAPTURECHANGED:
        mouseDown = dragging = 0;
        return 0;
    case WM_RBUTTONUP:
        ShowMenu();
        return 0;
    case WM_MOUSEWHEEL:   // 차단 화면에서 휠로 시간 조절
        if (view == VIEW_BLOCK) AddMins((short)HIWORD(wp) > 0 ? BLOCK_STEP : -BLOCK_STEP);
        return 0;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_DESTROY:
        SaveSettings();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// ---------- 시작 ----------

static int Main(void)
{
    CreateMutexW(NULL, TRUE, L"PomodoroTimer_SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;   // 이미 실행 중

    SetProcessDPIAware();
    HDC sdc = GetDC(NULL); dpi = GetDeviceCaps(sdc, LOGPIXELSX);

    W = S(145); H = S(67); pad = S(9); emoSize = S(14); topY = S(7);
    if (emoSize > EMOJI_SRC) emoSize = EMOJI_SRC;
    ScaleEmoji(EMOJI_FIRE, emoji[0], emoSize);
    ScaleEmoji(EMOJI_COFFEE, emoji[1], emoSize);
    ScaleEmoji(EMOJI_PAUSE, emoji[2], emoSize);
    ScaleEmoji(EMOJI_BLOCK, emoji[3], emoSize);

    // 백버퍼: 픽셀에 직접 접근할 수 있는 DIB 섹션
    BITMAPINFO bi = { { sizeof(BITMAPINFOHEADER), W, -H, 1, 32, BI_RGB } };
    memDC = CreateCompatibleDC(sdc);
    SelectObject(memDC, CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, (void **)&pix, NULL, 0));
    SetBkMode(memDC, TRANSPARENT);
    ReleaseDC(NULL, sdc);

    fontSmall = CreateFontW(-S(11), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Malgun Gothic");
    fontTime = CreateFontW(-S(26), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Segoe UI");
    fontMid = CreateFontW(-S(18), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Segoe UI");
    TEXTMETRICW tm;
    SelectObject(memDC, fontSmall); GetTextMetricsW(memDC, &tm); ascSmall = tm.tmAscent;
    SelectObject(memDC, fontTime); GetTextMetricsW(memDC, &tm); ascTime = tm.tmAscent;
    SelectObject(memDC, fontMid); GetTextMetricsW(memDC, &tm); ascMid = tm.tmAscent;

    int n = (int)GetEnvironmentVariableW(L"APPDATA", iniPath, MAX_PATH - 32);
    lstrcpyW(iniPath + n, L"\\Pomodoro\\settings.ini");
    lstrcpyW(errLogPath, iniPath); lstrcpyW(errLogPath + n, L"\\Pomodoro\\block_error.log");

    POINT pos;
    if (!LoadSettings(&pos)) pos = DefaultLocation();
    RECT wr = { pos.x, pos.y, pos.x + W, pos.y + H };
    if (!MonitorFromRect(&wr, MONITOR_DEFAULTTONULL)) pos = DefaultLocation();
    remainMs = PhaseMs();
    if (!BlockLeftMs() || !BlockMarkerExists()) blockStart = blockEnd = 0;   // 꺼져 있던 사이 끝났거나 풀린 차단

    HINSTANCE inst = GetModuleHandleW(NULL);
    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.lpszClassName = L"PomodoroTimer";
    RegisterClassW(&wc);

    hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST | WS_EX_LAYERED,
                           wc.lpszClassName, L"Pomodoro", WS_POPUP,
                           pos.x, pos.y, W, H, NULL, NULL, inst, NULL);
    SetLayeredWindowAttributes(hwnd, 0, (BYTE)alpha, LWA_ALPHA);
    int round = 2;   // DWMWCP_ROUND (Windows 11 둥근 모서리)
    DwmSetWindowAttribute(hwnd, 33, &round, sizeof round);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    ScheduleBlockTick();
    UpdateWindow(hwnd);
    SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, NULL, OnForeground, 0, 0,
                    WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    SetWinEventHook(EVENT_SYSTEM_MINIMIZEEND, EVENT_SYSTEM_MINIMIZEEND, NULL, OnForeground, 0, 0,
                    WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

    // 시작할 때 쓰고 끝난 메모리(DLL 초기화 등)를 돌려줌
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) DispatchMessageW(&msg);
    return 0;
}

void start(void) { ExitProcess((UINT)Main()); }
