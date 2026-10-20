// noWheelZoom
// Blocks mouse wheel scrolling while the target application (default: Warcraft III)
// is the foreground window, and maps the two side mouse buttons to F1 and F2 there
// (upper button -> hero 1, lower button -> hero 2).
// Also swallows the left and right Windows keys there, so a stray Win press cannot
// minimise the game. Middle button, dragging and all other apps are not affected.
// Build: build.bat (MinGW-w64) or build-msvc.bat (MSVC). See README.md.
#define _WIN32_WINNT 0x0A00
#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE

#include "windows.h"
#include "shellapi.h"
#include "wchar.h"

#ifdef _MSC_VER
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")
#endif

#ifndef WM_MOUSEHWHEEL
#define WM_MOUSEHWHEEL 0x020E
#endif

#ifndef WM_XBUTTONDOWN
#define WM_XBUTTONDOWN 0x020B
#endif

#ifndef WM_XBUTTONUP
#define WM_XBUTTONUP 0x020C
#endif

#ifndef WM_XBUTTONDBLCLK
#define WM_XBUTTONDBLCLK 0x020D
#endif

#ifndef XBUTTON1
#define XBUTTON1 0x0001
#endif

#ifndef XBUTTON2
#define XBUTTON2 0x0002
#endif

#ifndef PROCESS_NAME_NATIVE
#define PROCESS_NAME_NATIVE 0x00000001
#endif

#define APP_NAME       L"noWheelZoom"
#define WND_CLASS      L"NoWheelZoomWndClass"
#define MUTEX_NAME     L"NoWheelZoomSingleton"
#define DEFAULT_TARGET L"warcraft iii.exe"

#define PATH_BUF_LEN   4096
#define TARGET_LEN     260

#define WM_TRAY_CB     (WM_APP + 1)
#define WM_ALREADY     (WM_APP + 2)

#define IDM_PAUSE      1001
#define IDM_SIDE       1002
#define IDM_WIN        1003
#define IDM_EXIT       1004

// Virtual key codes, spelled out to avoid relying on winuser.h names
// that some older SDK headers still miss.
#define KEY_F1         0x70
#define KEY_F2         0x71
#define KEY_LWIN       0x5B
#define KEY_RWIN       0x5C

// XBUTTON1 is the rear (lower) side button on most mice, XBUTTON2 the front
// (upper) one. Hero 1 goes on the upper button, hero 2 on the lower.
// Swap these two lines if your mouse is wired the other way round.
#define VK_UPPER_BUTTON  KEY_F1
#define VK_LOWER_BUTTON  KEY_F2

// Marker written into every injected event, handy when debugging hooks.
#define INJECT_TAG     ((ULONG_PTR)0x4E575A31)   // 'NWZ1'

static HHOOK  g_hook = NULL;
static HHOOK  g_kbdHook = NULL;
static HWND   g_wnd = NULL;
static HICON  g_icon = NULL;
static HANDLE g_mutex = NULL;
static BOOL   g_paused = FALSE;
static BOOL   g_sideKeys = TRUE;
static BOOL   g_blockWin = TRUE;
static BOOL   g_tray = FALSE;

// Which side button is currently held down by us. Needed so that the release
// always happens exactly once, even if focus changed in between.
static BOOL   g_upperHeld = FALSE;
static BOOL   g_lowerHeld = FALSE;

// TRUE while we are swallowing a Windows key that we already swallowed on the
// way down, so the matching release is swallowed too.
static BOOL   g_winDown = FALSE;

static WCHAR  g_target[TARGET_LEN] = DEFAULT_TARGET;
static WCHAR  g_path[PATH_BUF_LEN];
static NOTIFYICONDATAW g_nid;

// Case-insensitive ASCII compare, folds both sides with bit 0x20.
static BOOL EqFold(const WCHAR *a, const WCHAR *b)
{
    for (; *a != 0 && *b != 0; a++, b++) {
        if ((WCHAR)(*a | 0x20) != (WCHAR)(*b | 0x20))
            return FALSE;
    }
    return (*a == 0 && *b == 0);
}

// TRUE if s starts with prefix, case-insensitive ASCII.
static BOOL StartsWithFold(const WCHAR *s, const WCHAR *prefix)
{
    for (; *prefix != 0; s++, prefix++) {
        if (*s == 0)
            return FALSE;
        if ((WCHAR)(*s | 0x20) != (WCHAR)(*prefix | 0x20))
            return FALSE;
    }
    return TRUE;
}

// TRUE if path ends with the file name, case-insensitive.
// A path separator must precede the name, so "notwarcraft iii.exe" does not match.
static BOOL PathEndsWithName(const WCHAR *path, const WCHAR *name)
{
    const WCHAR *end;
    const WCHAR *start;
    const WCHAR *p;

    end = path;
    while (*end != 0)
        end++;

    start = end;
    for (p = name; *p != 0; p++) {
        if (start == path)
            return FALSE;
        start--;
    }

    if (start != path && start[-1] != (WCHAR)0x5C && start[-1] != (WCHAR)0x2F)
        return FALSE;

    for (p = name; *p != 0; p++, start++) {
        if ((WCHAR)(*start | 0x20) != (WCHAR)(*p | 0x20))
            return FALSE;
    }
    return TRUE;
}

// TRUE if the process with this pid is the target application.
static BOOL ProcessIsTarget(DWORD pid)
{
    HANDLE h;
    DWORD size;
    BOOL hit = FALSE;

    h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (h == NULL)
        return FALSE;

    size = PATH_BUF_LEN;
    if (QueryFullProcessImageNameW(h, 0, g_path, &size) != 0) {
        if (size != 0)
            hit = PathEndsWithName(g_path, g_target);
    } else {
        // Path longer than MAX_PATH: retry in native format, it has no such limit.
        size = PATH_BUF_LEN;
        if (QueryFullProcessImageNameW(h, PROCESS_NAME_NATIVE, g_path, &size) != 0 && size != 0)
            hit = PathEndsWithName(g_path, g_target);
    }

    CloseHandle(h);
    return hit;
}

// TRUE if the target application owns the foreground window.
static BOOL TargetIsForeground(void)
{
    HWND hwnd;
    DWORD pid = 0;

    hwnd = GetForegroundWindow();
    if (hwnd == NULL)
        return FALSE;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0)
        return FALSE;
    return ProcessIsTarget(pid);
}

// Inject one key transition. Called from the hook, so it must stay cheap:
// a single SendInput of a single event, no allocation, no waiting.
static void SendVirtualKey(WORD vk, BOOL down)
{
    INPUT in;

    ZeroMemory(&in, sizeof(in));
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    in.ki.wScan = 0;
    in.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
    in.ki.time = 0;
    in.ki.dwExtraInfo = INJECT_TAG;
    SendInput(1, &in, sizeof(in));
}

// Drop every key we may still be holding. Called on pause, on exit and on
// the side-keys toggle, otherwise F1/F2 would stay stuck down.
static void ReleaseAllKeys(void)
{
    if (g_upperHeld) {
        SendVirtualKey((WORD)VK_UPPER_BUTTON, FALSE);
        g_upperHeld = FALSE;
    }
    if (g_lowerHeld) {
        SendVirtualKey((WORD)VK_LOWER_BUTTON, FALSE);
        g_lowerHeld = FALSE;
    }
}

// Low level mouse hook. No allocations and no blocking here on purpose.
static LRESULT CALLBACK MouseProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    UINT msg;
    MSLLHOOKSTRUCT *ms;
    WORD button;
    BOOL fg;

    if (nCode != HC_ACTION)
        return CallNextHookEx(g_hook, nCode, wParam, lParam);

    if (g_paused)
        return CallNextHookEx(g_hook, nCode, wParam, lParam);

    msg = (UINT)wParam;

    // Wheel messages: swallow, both vertical and horizontal (tilt).
    if (msg == WM_MOUSEWHEEL || msg == WM_MOUSEHWHEEL) {
        if (TargetIsForeground())
            return 1;   // the game never sees it
        return CallNextHookEx(g_hook, nCode, wParam, lParam);
    }

    if (g_sideKeys == FALSE)
        return CallNextHookEx(g_hook, nCode, wParam, lParam);

    if (msg != WM_XBUTTONDOWN && msg != WM_XBUTTONUP && msg != WM_XBUTTONDBLCLK)
        return CallNextHookEx(g_hook, nCode, wParam, lParam);

    ms = (MSLLHOOKSTRUCT *)lParam;
    button = HIWORD(ms->mouseData);
    fg = TargetIsForeground();

    // XBUTTON1 is the lower/rear button on most mice.
    if (button == XBUTTON1) {
        if (msg == WM_XBUTTONUP) {
            // Release even if focus already left the game, so the key cannot
            // stay stuck down; the event itself is not swallowed in that case.
            if (g_lowerHeld) {
                SendVirtualKey((WORD)VK_LOWER_BUTTON, FALSE);
                g_lowerHeld = FALSE;
            }
        } else if (fg && g_lowerHeld == FALSE) {
            // Down and double-click both land here; the guard stops a second
            // key-down on a double click.
            SendVirtualKey((WORD)VK_LOWER_BUTTON, TRUE);
            g_lowerHeld = TRUE;
        }
        if (fg)
            return 1;
        return CallNextHookEx(g_hook, nCode, wParam, lParam);
    }

    // XBUTTON2 is the upper/front button on most mice.
    if (button == XBUTTON2) {
        if (msg == WM_XBUTTONUP) {
            if (g_upperHeld) {
                SendVirtualKey((WORD)VK_UPPER_BUTTON, FALSE);
                g_upperHeld = FALSE;
            }
        } else if (fg && g_upperHeld == FALSE) {
            SendVirtualKey((WORD)VK_UPPER_BUTTON, TRUE);
            g_upperHeld = TRUE;
        }
        if (fg)
            return 1;
        return CallNextHookEx(g_hook, nCode, wParam, lParam);
    }

    return CallNextHookEx(g_hook, nCode, wParam, lParam);
}

// Low level keyboard hook. Only the two Windows keys are considered; every
// other key passes through untouched, so hotkeys and typing are unaffected.
static LRESULT CALLBACK KeyboardProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    KBDLLHOOKSTRUCT *ks;

    if (nCode != HC_ACTION)
        return CallNextHookEx(g_kbdHook, nCode, wParam, lParam);

    if (g_paused || g_blockWin == FALSE)
        return CallNextHookEx(g_kbdHook, nCode, wParam, lParam);

    ks = (KBDLLHOOKSTRUCT *)lParam;
    if (ks->vkCode != KEY_LWIN && ks->vkCode != KEY_RWIN)
        return CallNextHookEx(g_kbdHook, nCode, wParam, lParam);

    if (wParam == WM_KEYUP || wParam == WM_SYSKEYUP) {
        // Always swallow the release of a press we swallowed, even if focus
        // already moved away: a lone Windows key release is what makes the
        // shell pop the Start menu up on top of the game.
        if (g_winDown) {
            g_winDown = FALSE;
            return 1;
        }
        return CallNextHookEx(g_kbdHook, nCode, wParam, lParam);
    }

    // Key down and auto-repeat. Swallowing the down also disables every
    // Win+key shell shortcut, because the system never sees the modifier.
    if (TargetIsForeground()) {
        g_winDown = TRUE;
        return 1;
    }
    return CallNextHookEx(g_kbdHook, nCode, wParam, lParam);
}

static void TrayAdd(void)
{
    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_wnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY_CB;
    g_nid.hIcon = g_icon;
    lstrcpynW(g_nid.szTip, APP_NAME, 64);
    g_tray = (Shell_NotifyIconW(NIM_ADD, &g_nid) != FALSE);
}

static void TraySetTip(const WCHAR *tip)
{
    if (g_tray == FALSE)
        return;
    g_nid.uFlags = NIF_TIP;
    lstrcpynW(g_nid.szTip, tip, 64);
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

static void TrayBalloon(const WCHAR *title, const WCHAR *text)
{
    if (g_tray == FALSE)
        return;
    g_nid.uFlags = NIF_INFO;
    g_nid.dwInfoFlags = NIIF_INFO;
    lstrcpynW(g_nid.szInfoTitle, title, 64);
    lstrcpynW(g_nid.szInfo, text, 256);
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

static void TrayRemove(void)
{
    if (g_tray) {
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        g_tray = FALSE;
    }
}

static void ShowTrayMenu(void)
{
    HMENU menu;
    POINT pt;

    menu = CreatePopupMenu();
    if (menu == NULL)
        return;
    AppendMenuW(menu, MF_STRING | (g_paused ? MF_CHECKED : 0), IDM_PAUSE, L"Pause");
    AppendMenuW(menu, MF_STRING | (g_sideKeys ? MF_CHECKED : 0), IDM_SIDE,
                L"Side buttons -> F1 / F2");
    AppendMenuW(menu, MF_STRING | (g_blockWin ? MF_CHECKED : 0), IDM_WIN,
                L"Block Win key");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"Exit");

    GetCursorPos(&pt);
    SetForegroundWindow(g_wnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_wnd, NULL);
    PostMessageW(g_wnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    (void)hwnd;

    switch (msg) {
    case WM_TRAY_CB:
        if (lParam == WM_RBUTTONUP || lParam == WM_LBUTTONUP)
            ShowTrayMenu();
        return 0;
    case WM_ALREADY:
        TrayBalloon(APP_NAME, L"Already running. See the tray icon.");
        return 0;
    case WM_COMMAND:
        if (LOWORD(wParam) == IDM_PAUSE) {
            g_paused = (g_paused == FALSE);
            if (g_paused)
                ReleaseAllKeys();
            TraySetTip(g_paused ? L"noWheelZoom - paused" : L"noWheelZoom - active");
        } else if (LOWORD(wParam) == IDM_SIDE) {
            g_sideKeys = (g_sideKeys == FALSE);
            if (g_sideKeys == FALSE)
                ReleaseAllKeys();
        } else if (LOWORD(wParam) == IDM_WIN) {
            g_blockWin = (g_blockWin == FALSE);
            // Forgetting the held state keeps the next Win release in sync.
            if (g_blockWin == FALSE)
                g_winDown = FALSE;
        } else if (LOWORD(wParam) == IDM_EXIT) {
            ReleaseAllKeys();
            DestroyWindow(g_wnd);
        }
        return 0;
    case WM_DESTROY:
        ReleaseAllKeys();
        TrayRemove();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void ShowError(const WCHAR *what, DWORD code)
{
    WCHAR text[256];

    wsprintfW(text, L"%s failed, code %lu", what, (unsigned long)code);
    MessageBoxW(NULL, text, APP_NAME,
                MB_OK | MB_ICONERROR | MB_SYSTEMMODAL | MB_SETFOREGROUND | MB_TOPMOST);
}

// Optional command line: -target name.exe  or  -target=name.exe
static void ParseCommandLine(void)
{
    LPWSTR *argv;
    int argc = 0;
    int i;
    WCHAR *value = NULL;
    WCHAR *p;
    WCHAR *last;

    argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv == NULL)
        return;

    for (i = 1; i != argc; i++) {
        if (EqFold(argv[i], L"-target") && (i + 1) != argc) {
            value = argv[i + 1];
            i++;
        } else if (StartsWithFold(argv[i], L"-target=")) {
            value = argv[i] + 8;
        }
    }

    if (value != NULL && *value != 0) {
        // Keep only the file name, drop any directory part.
        last = value;
        for (p = value; *p != 0; p++) {
            if (*p == (WCHAR)0x5C || *p == (WCHAR)0x2F)
                last = p + 1;
        }
        if (*last != 0)
            lstrcpynW(g_target, last, TARGET_LEN);
    }

    LocalFree(argv);
}

int APIENTRY WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmdLine, int nShowCmd)
{
    WNDCLASSW wc;
    MSG msg;
    HWND other;
    DWORD err;
    int r;

    (void)hPrev;
    (void)lpCmdLine;
    (void)nShowCmd;

    ParseCommandLine();

    // A second copy just pings the running one and exits.
    other = FindWindowW(WND_CLASS, NULL);
    if (other != NULL) {
        PostMessageW(other, WM_ALREADY, 0, 0);
        return 0;
    }

    SetLastError(ERROR_SUCCESS);
    g_mutex = CreateMutexW(NULL, FALSE, MUTEX_NAME);
    if (g_mutex != NULL && GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(g_mutex);
        g_mutex = NULL;
        return 0;
    }

    // Own icon from version.rc if the resource exists, otherwise the stock Windows icon.
    g_icon = LoadIconW(hInst, MAKEINTRESOURCEW(1));
    if (g_icon == NULL)
        g_icon = LoadIconW(NULL, IDI_APPLICATION);

    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = WND_CLASS;
    wc.hIcon = g_icon;
    RegisterClassW(&wc);

    // Hidden window, it only receives tray menu messages.
    g_wnd = CreateWindowExW(0, WND_CLASS, APP_NAME, 0, 0, 0, 0, 0, NULL, NULL, hInst, NULL);
    if (g_wnd == NULL) {
        ShowError(L"CreateWindowExW", GetLastError());
        return 1;
    }

    TrayAdd();
    if (g_tray == FALSE)
        MessageBoxW(NULL, L"Tray icon was not added. Stop the program with Task Manager.",
                    APP_NAME, MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);

    g_hook = SetWindowsHookExW(WH_MOUSE_LL, MouseProc, NULL, 0);
    if (g_hook == NULL) {
        err = GetLastError();
        TrayRemove();
        DestroyWindow(g_wnd);
        ShowError(L"SetWindowsHookExW", err);
        return 1;
    }

    // Second hook for the Windows keys. Both must live on this thread, the one
    // that owns the message loop, or the low level callbacks never fire.
    g_kbdHook = SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardProc, NULL, 0);
    if (g_kbdHook == NULL) {
        err = GetLastError();
        UnhookWindowsHookEx(g_hook);
        g_hook = NULL;
        TrayRemove();
        DestroyWindow(g_wnd);
        ShowError(L"SetWindowsHookExW (keyboard)", err);
        return 1;
    }

    for (;;) {
        r = GetMessageW(&msg, NULL, 0, 0);
        if (r == 0 || r == -1)
            break;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_kbdHook != NULL)
        UnhookWindowsHookEx(g_kbdHook);
    if (g_hook != NULL)
        UnhookWindowsHookEx(g_hook);
    if (g_mutex != NULL)
        CloseHandle(g_mutex);
    return 0;
}
