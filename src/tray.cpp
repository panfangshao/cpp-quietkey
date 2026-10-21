#include "tray.h"

#include "log.h"
#include "resource.h"

#include <shellapi.h>

#include <cwchar>

UINT Tray::TaskbarCreatedMessage() {
    // 只注册一次，之后每次返回同一个消息号。
    static const UINT msg = RegisterWindowMessageW(L"TaskbarCreated");
    return msg;
}

Tray::~Tray() {
    Destroy();
}

bool Tray::Create(HWND hwnd, UINT callbackMsg, HICON icon, const wchar_t* tooltip) {
    Destroy();

    data_ = NOTIFYICONDATAW{};
    data_.cbSize = sizeof(data_);
    data_.hWnd = hwnd;
    data_.uID = 1;
    data_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    data_.uCallbackMessage = callbackMsg;
    data_.hIcon = icon;
    wcsncpy_s(data_.szTip, tooltip, _TRUNCATE);

    if (!Shell_NotifyIconW(NIM_ADD, &data_)) {
        LogF(L"托盘图标创建失败（错误码 %lu）", GetLastError());
        return false;
    }
    added_ = true;

    // 版本 4：鼠标事件的坐标直接放在 wParam 里，不用再自己 GetCursorPos，
    // 也能正确处理多显示器和高 DPI。
    data_.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &data_);
    return true;
}

void Tray::Destroy() {
    if (added_) {
        Shell_NotifyIconW(NIM_DELETE, &data_);
        added_ = false;
    }
}

bool Tray::Readd() {
    if (data_.hWnd == nullptr) {
        return false;
    }
    // TaskbarCreated 意味着旧图标已随旧托盘区一起消失，无论 added_ 是什么都要重新加。
    added_ = false;
    if (!Shell_NotifyIconW(NIM_ADD, &data_)) {
        return false;
    }
    added_ = true;
    data_.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &data_);
    LogF(L"托盘图标已添加");
    return true;
}

void Tray::ShowContextMenu(HWND hwnd, POINT pt) {
    HMENU menu = CreatePopupMenu();
    if (!menu) {
        return;
    }
    AppendMenuW(menu, MF_STRING, IDM_SETTINGS, L"打开设置(&S)");
    AppendMenuW(menu, MF_STRING, IDM_TOGGLE, L"立即播放/暂停(&P)");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_QUIT, L"退出(&X)");

    // 不抢前台的话，菜单弹出后点别处不会消失——Win32 的经典坑。
    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, hwnd, nullptr);
    // 同一个坑的另一半：发个空消息让菜单正常收尾。
    PostMessageW(hwnd, WM_NULL, 0, 0);

    DestroyMenu(menu);
}

void Tray::SetTooltip(const wchar_t* tooltip) {
    // 图标暂时不在（启动时添加失败、资源管理器重启中）也先记下，Readd 时带上。
    wcsncpy_s(data_.szTip, tooltip, _TRUNCATE);
    if (!added_) {
        return;
    }
    NOTIFYICONDATAW n = data_;
    n.uFlags = NIF_TIP | NIF_SHOWTIP;
    Shell_NotifyIconW(NIM_MODIFY, &n);
}

void Tray::Balloon(const wchar_t* title, const wchar_t* text) {
    if (!added_) {
        return;
    }
    NOTIFYICONDATAW n = data_;
    n.uFlags = NIF_INFO;
    n.dwInfoFlags = NIIF_NONE;
    wcsncpy_s(n.szInfoTitle, title, _TRUNCATE);
    wcsncpy_s(n.szInfo, text, _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &n);
}
