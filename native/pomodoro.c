// 포모도로 타이머 - 순수 Win32 C 버전 (C 런타임 없이 빌드)
// C# 버전과 기능/디자인 동일. 최적화 포인트:
// - 할 일이 없으면 타이머가 하나도 돌지 않음 (마우스 hover는 이벤트로 감지)
// - 실행 중엔 표시되는 초가 바뀌는 순간에만 깨어나서 다시 그림
// - 투명도 애니메이션 타이머는 애니메이션 중에만 동작
// - 이모지는 exe에 내장한 48px 이미지를 시작할 때 한 번만 축소해 둠
#define WIN32_LEAN_AND_MEAN
#define UNICODE
#include <windows.h>
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

#define IDLE_ALPHA 64   // 25% 불투명 = 75% 투명
#define T_TICK  1
#define T_FADE  2
#define T_BLINK 3
#define T_TOP   4

#define LONG_EVERY 4   // 공부 4회마다 긴 휴식 (Cirillo 원래 방식, Pomofocus 등 기본값)

enum { FOCUS, BREAK, LONG_BREAK };
enum { CMD_25 = 1, CMD_50, CMD_LONG, CMD_SWITCH, CMD_RESET, CMD_HOME, CMD_EXIT };

static HWND hwnd;
static int dpi, W, H, pad, emoSize, topY;
static int focusMin = 25, breakMin = 5, longMin = 15, phase = FOCUS;
static int longOn = 1, doneCount;   // doneCount: 이번 세트에서 끝까지 마친 공부 횟수
static int running, finished, blinkOn;
static LONGLONG remainMs;
static ULONGLONG endTick;
static int alpha = IDLE_ALPHA, hover, menuOpen, mouseDown, dragging;
static POINT downPos, downLoc;
static HFONT fontSmall, fontTime;
static HDC memDC;
static DWORD *pix;
static DWORD emoji[3][EMOJI_SRC * EMOJI_SRC];   // 0=불, 1=커피, 2=일시정지 (화면 크기로 축소된 것)
static WCHAR iniPath[MAX_PATH];

static int S(int v) { return (v * dpi + 48) / 96; }
static int Abs(int v) { return v < 0 ? -v : v; }
static int PhaseMin(void) { return phase == FOCUS ? focusMin : phase == BREAK ? breakMin : longMin; }
static LONGLONG PhaseMs(void) { return (LONGLONG)PhaseMin() * 60000; }

static WCHAR *PutInt(WCHAR *p, int v)
{
    WCHAR tmp[12]; int n = 0;
    if (v < 0) { *p++ = L'-'; v = -v; }
    do { tmp[n++] = (WCHAR)(L'0' + v % 10); v /= 10; } while (v);
    while (n) *p++ = tmp[--n];
    return p;
}

static WCHAR *PutStr(WCHAR *p, const WCHAR *s) { while (*s) *p++ = *s++; return p; }

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

static void Render(void)
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

// ---------- 설정 저장 ----------

static void SaveSettings(void)
{
    RECT r; GetWindowRect(hwnd, &r);
    char buf[64]; WCHAR w[64], *p = w;
    p = PutStr(p, L"mode="); p = PutInt(p, focusMin);
    p = PutStr(p, L"\r\nlong="); p = PutInt(p, longOn);
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
            s++; int neg = *s == '-', v = 0, ok = 0; if (neg) s++;
            while (*s >= '0' && *s <= '9') { v = v * 10 + *s++ - '0'; ok = 1; }
            if (neg) v = -v;
            if (ok && key == 'm' && v == 50) { focusMin = 50; breakMin = 10; longMin = 30; }
            if (ok && key == 'l') longOn = v != 0;
            if (ok && key == 'x') { pt->x = v; hasX = 1; }
            if (ok && key == 'y') { pt->y = v; hasY = 1; }
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

static void ShowMenu(void)
{
    HMENU m = CreatePopupMenu();
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
        return 0;
    case WM_MOUSEMOVE:
        if (!hover) { hover = 1; TRACKMOUSEEVENT t = { sizeof t, TME_LEAVE, h, 0 }; TrackMouseEvent(&t); UpdateFade(); }
        if (mouseDown) {
            POINT p; GetCursorPos(&p);
            int dx = p.x - downPos.x, dy = p.y - downPos.y;
            if (!dragging && (Abs(dx) > S(4) || Abs(dy) > S(4))) dragging = 1;
            if (dragging) SetWindowPos(h, NULL, downLoc.x + dx, downLoc.y + dy, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return 0;
    case WM_MOUSELEAVE:
        hover = 0; UpdateFade();
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

    // 백버퍼: 픽셀에 직접 접근할 수 있는 DIB 섹션
    BITMAPINFO bi = { { sizeof(BITMAPINFOHEADER), W, -H, 1, 32, BI_RGB } };
    memDC = CreateCompatibleDC(sdc);
    SelectObject(memDC, CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, (void **)&pix, NULL, 0));
    SetBkMode(memDC, TRANSPARENT);
    ReleaseDC(NULL, sdc);

    fontSmall = CreateFontW(-S(11), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Malgun Gothic");
    fontTime = CreateFontW(-S(26), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Segoe UI");

    int n = (int)GetEnvironmentVariableW(L"APPDATA", iniPath, MAX_PATH - 32);
    lstrcpyW(iniPath + n, L"\\Pomodoro\\settings.ini");

    POINT pos;
    if (!LoadSettings(&pos)) pos = DefaultLocation();
    RECT wr = { pos.x, pos.y, pos.x + W, pos.y + H };
    if (!MonitorFromRect(&wr, MONITOR_DEFAULTTONULL)) pos = DefaultLocation();
    remainMs = PhaseMs();

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
