// quietkey —— 一个全局热键切换后台视频的播放/暂停，不切窗口、不抢焦点。
//
// 进程结构刻意简单：**只有一个线程**，一个隐藏窗口 + 一条消息循环。
// 界面是原生对话框、动作是消息循环里的一次同步调用，不存在"界面框架的重绘节奏"
// 这种约束。策略链里每一步阻塞调用都有上限（媒体会话单步 ≤1.5s 超时、
// SendMessageTimeoutW 120ms），期间界面短暂无响应可以接受——何况平时窗口根本不显示。

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>  // CommandLineToArgvW
#include <windowsx.h>  // GET_X_LPARAM / GET_Y_LPARAM

#include <winrt/base.h>

#include <string>

#include "config.h"
#include "hotkey.h"
#include "log.h"
#include "resource.h"
#include "settings.h"
#include "strategy.h"
#include "tray.h"

namespace {

// 托盘鼠标事件回传用的自定义消息号。
constexpr UINT WM_TRAY_CALLBACK = WM_APP + 1;

// 托盘图标添加失败后的重试定时器。开机自启时资源管理器常常还没就绪，
// Shell_NotifyIcon 会失败；之后不一定还会收到 TaskbarCreated，所以自己轮询。
constexpr UINT_PTR kTrayRetryTimer = 1;
constexpr UINT kTrayRetryIntervalMs = 2000;
constexpr int kTrayRetryMax = 60;  // 最多重试两分钟，之后只靠 TaskbarCreated

constexpr wchar_t kWindowClass[] = L"QuietkeyMainWindow";
constexpr wchar_t kAppName[] = L"quietkey";
// 单实例互斥体。第二个实例注册热键必定失败，与其让用户对着一个
// "热键没反应"的程序发懵，不如直接告诉他已经在跑了。
constexpr wchar_t kSingleInstanceMutex[] = L"Local\\quietkey.singleton";

struct App {
    HINSTANCE instance = nullptr;
    HWND hwnd = nullptr;
    Tray tray;
    HotkeyRegistration hotkey;
    Config cfg;
    unsigned triggerCount = 0;
    int trayRetries = 0;
};

App g_app;

/// 按 g_app.cfg.hotkey 注册热键，返回是否成功。
/// 失败时原来已注册的热键保持不动（见 HotkeyRegistration::Apply）。
bool ApplyHotkeyFromConfig(bool notifyOnFailure) {
    HotkeySpec spec;
    if (!ParseHotkey(g_app.cfg.hotkey, &spec)) {
        LogF(L"配置里的热键 \"%s\" 无法解析", g_app.cfg.hotkey.c_str());
        if (notifyOnFailure) {
            g_app.tray.Balloon(kAppName, L"配置里的热键写法无法识别，请在设置里重新录一个。");
        }
        return false;
    }
    if (!g_app.hotkey.Apply(g_app.hwnd, spec)) {
        // 组合键被别的程序占用是 RegisterHotKey 的固有行为，不是 bug。
        // 必须把错误暴露给用户，**不要静默吞掉**。
        std::wstring msg = L"热键 " + FormatHotkey(spec) + L" 注册失败，多半已被其他程序占用。\n";
        msg += g_app.hotkey.active()
                   ? L"仍在使用原来的 " + FormatHotkey(g_app.hotkey.current()) + L"。"
                   : std::wstring(L"程序仍在托盘运行，可以在设置里换一个组合键。");
        if (notifyOnFailure) {
            g_app.tray.Balloon(kAppName, msg.c_str());
        }
        return false;
    }
    return true;
}

/// 托盘悬停提示：把「热键 + 已触发次数」摆出来，悬停一下就知道它还活着。
void RefreshTrayTooltip() {
    std::wstring tip = L"quietkey";
    if (g_app.hotkey.active()) {
        tip += L" — " + FormatHotkey(g_app.hotkey.current());
    } else {
        tip += L" — 热键未注册";
    }
    tip += L" — 已触发 " + std::to_wstring(g_app.triggerCount) + L" 次";
    g_app.tray.SetTooltip(tip.c_str());
}

/// 执行一次播放/暂停，并把结果送到界面和日志。
void TriggerToggle() {
    const ActionReport report = TogglePlayPause(g_app.cfg);
    ++g_app.triggerCount;
    LogF(L"热键触发: %s", report.Summary().c_str());
    SettingsRefreshLastReport(report);
    RefreshTrayTooltip();

    // 成功时保持安静（这个工具的全部意义就是不打扰你）；
    // 只有真的没生效才弹气泡，否则用户会以为程序坏了却没有任何线索。
    if (!report.ok) {
        // 气泡里放一句能直接照做的话，完整解释留给设置界面的「上次触发」。
        // 别把长段落塞进气泡——读不完，也盖不住重点。
        const wchar_t* hint =
            report.detail.find(L"没有注册媒体会话") != std::wstring::npos
                ? L"没检测到正在播放的媒体。到播放页面按 F5 重新播放通常就好了。"
                : L"打开设置 →「上次触发」可以看到每一层的尝试结果。";
        g_app.tray.Balloon(L"quietkey 这次没生效", hint);
    }
}

void OpenSettings() {
    ShowSettingsDialog(g_app.instance, g_app.hwnd, &g_app.cfg, [](const Config&) {
        // 「保存并应用」：先重挂热键，再落盘。
        if (!ApplyHotkeyFromConfig(true) && g_app.hotkey.active()) {
            // 新组合键挂不上、旧的还在用：配置里也写回旧的。
            // 否则落盘的是一个挂不上的热键，下次启动就一个热键都没有。
            g_app.cfg.hotkey = FormatHotkey(g_app.hotkey.current());
        }
        RefreshTrayTooltip();
        if (!g_app.cfg.Save()) {
            MessageBoxW(nullptr, L"配置写入失败，改动只在本次运行有效。", kAppName,
                        MB_OK | MB_ICONWARNING);
        }
    });
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    // 资源管理器重启后托盘区被重建，图标要自己加回去。
    if (msg == Tray::TaskbarCreatedMessage()) {
        g_app.tray.Readd();
        return 0;
    }

    switch (msg) {
        case WM_TRAY_CALLBACK: {
            // 图标用的是 NOTIFYICON_VERSION_4：事件在 lParam 低位，坐标在 wParam。
            const UINT event = LOWORD(lParam);
            if (event == WM_CONTEXTMENU) {
                POINT pt{GET_X_LPARAM(wParam), GET_Y_LPARAM(wParam)};
                g_app.tray.ShowContextMenu(hwnd, pt);
            } else if (event == WM_LBUTTONDBLCLK) {
                OpenSettings();  // 双击开设置，托盘小工具的通行约定
            }
            return 0;
        }

        case WM_HOTKEY:
            if (HotkeyRegistration::IsOurId(wParam)) {
                TriggerToggle();
            }
            return 0;

        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case IDM_SETTINGS: OpenSettings(); return 0;
                case IDM_TOGGLE:   TriggerToggle(); return 0;
                case IDM_QUIT:     DestroyWindow(hwnd); return 0;
                default: break;
            }
            break;

        case WM_TIMER:
            if (wParam == kTrayRetryTimer) {
                if (g_app.tray.Readd()) {
                    KillTimer(hwnd, kTrayRetryTimer);
                    // 启动时热键若注册失败，那会儿托盘不在、气泡发不出去。
                    // 现在图标有了，再试一次，失败就能如实告诉用户。
                    if (!g_app.hotkey.active()) {
                        ApplyHotkeyFromConfig(true);
                    }
                    RefreshTrayTooltip();
                } else if (++g_app.trayRetries >= kTrayRetryMax) {
                    KillTimer(hwnd, kTrayRetryTimer);
                    LogF(L"托盘图标多次添加失败，改为等待资源管理器的 TaskbarCreated 广播");
                }
                return 0;
            }
            break;

        case WM_DESTROY:
            KillTimer(hwnd, kTrayRetryTimer);
            g_app.hotkey.Clear();
            g_app.tray.Destroy();
            PostQuitMessage(0);
            return 0;

        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

/// 创建承载消息的窗口。
///
/// 刻意用普通隐藏窗口而不是 HWND_MESSAGE 消息窗口：后者收不到系统广播，
/// 也就收不到 "TaskbarCreated"，资源管理器重启后图标就再也回不来了。
bool CreateHiddenWindow() {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = g_app.instance;
    wc.lpszClassName = kWindowClass;
    wc.hIcon = LoadIconW(g_app.instance, MAKEINTRESOURCEW(IDI_APP));
    if (!RegisterClassExW(&wc)) {
        LogF(L"注册窗口类失败（错误码 %lu）", GetLastError());
        return false;
    }

    g_app.hwnd = CreateWindowExW(0, kWindowClass, kAppName, WS_OVERLAPPEDWINDOW,
                                 CW_USEDEFAULT, CW_USEDEFAULT, 0, 0,
                                 nullptr, nullptr, g_app.instance, nullptr);
    if (!g_app.hwnd) {
        LogF(L"创建窗口失败（错误码 %lu）", GetLastError());
        return false;
    }
    return true;  // 不 ShowWindow，程序常驻托盘
}

bool ClaimSingleInstance() {
    HANDLE m = CreateMutexW(nullptr, FALSE, kSingleInstanceMutex);
    if (m == nullptr) {
        return true;  // 拿不到互斥体不该阻断启动，放行
    }
    return GetLastError() != ERROR_ALREADY_EXISTS;
    // 故意不关闭句柄：让它随进程一起释放
}

bool HasArg(int argc, wchar_t** argv, const wchar_t* name) {
    for (int i = 1; i < argc; ++i) {
        if (_wcsicmp(argv[i], name) == 0) {
            return true;
        }
    }
    return false;
}

// ---- 命令行诊断模式 ----
// 「某个播放器为什么控不到」的第一问永远是"它到底注册会话了没有"，
// 命令行比开界面快得多。注意这两个模式都需要控制台，双击运行看不到输出。

int RunListSessions() {
    const std::vector<SessionInfo> sessions = ListMediaSessions();
    if (sessions.empty()) {
        PrintF(L"系统当前没有任何媒体会话。\n"
               L"播放器只有在播放【带声音、未静音、时长足够】的媒体时才会注册会话。\n");
        return 0;
    }
    PrintF(L"共 %zu 个媒体会话：\n", sessions.size());
    for (const SessionInfo& s : sessions) {
        std::wstring activity;
        if (!s.hasTimeline) {
            activity = L"不上报时间线";
        } else if (s.recent) {
            activity = L"最近碰过";
        } else {
            wchar_t buf[48];
            _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"%.1f 秒前碰过", s.behindSecs);
            activity = buf;
        }
        PrintF(L"  %s %s %-24s %s\n", s.playing ? L"▶ 播放中" : L"⏸ 已暂停",
               s.recent ? L"★" : L" ", s.aumid.c_str(), activity.c_str());
    }
    return 0;
}

int RunTestOnce() {
    const ActionReport report = TogglePlayPause(g_app.cfg);
    PrintF(L"结果: %s\n", report.Summary().c_str());
    for (const TraceEntry& e : report.trace) {
        PrintF(L"  %-18s %s  %s\n", e.layer.c_str(), e.label.c_str(), e.detail.c_str());
    }
    return report.ok ? 0 : 1;
}

}  // namespace

/// 顶部状态行，设置对话框会调它。
std::wstring AppStatusLine() {
    std::wstring s;
    if (g_app.hotkey.active()) {
        s = L"● 热键 " + FormatHotkey(g_app.hotkey.current()) + L" 已注册，正在后台监听";
    } else {
        s = L"● 热键未注册（多半被其他程序占用，换一个组合键试试）";
    }
    s += L"\n累计触发 " + std::to_wstring(g_app.triggerCount) + L" 次";
    return s;
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    g_app.instance = instance;

    // WinRT 的媒体会话调用要求线程处于多线程套间。
    // 本程序不用任何只支持 STA 的 shell 接口，所以直接把主线程放进 MTA 最省事。
    winrt::init_apartment(winrt::apartment_type::multi_threaded);

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    const bool wantList = argv && HasArg(argc, argv, L"--list-sessions");
    const bool wantTest = argv && HasArg(argc, argv, L"--test-once");
    const bool wantConsole = argv && HasArg(argc, argv, L"--console");
    if (argv) {
        LocalFree(argv);
    }

    g_app.cfg = Config::Load();

    if (wantList || wantTest) {
        AttachToConsole();
        SetLogToConsole(true);
        return wantList ? RunListSessions() : RunTestOnce();
    }
    if (wantConsole) {
        SetLogToConsole(true);
    }

    if (!ClaimSingleInstance()) {
        LogF(L"已经有一个实例在运行");
        MessageBoxW(nullptr, L"quietkey 已经在运行了。\n看一下托盘区。", kAppName,
                    MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    // 热键控件（msctls_hotkey32）和视觉样式版的通用控件都要显式初始化。
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES | ICC_HOTKEY_CLASS};
    InitCommonControlsEx(&icc);

    if (!CreateHiddenWindow()) {
        return 1;
    }

    // 托盘要的是当前 DPI 下的小图标尺寸。LoadIconW 固定给 32px，
    // 系统再缩到 16/20/24px，高分屏上发糊。
    HICON icon = nullptr;
    if (FAILED(LoadIconMetric(instance, MAKEINTRESOURCEW(IDI_APP), LIM_SMALL, &icon))) {
        icon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP));
    }
    if (!g_app.tray.Create(g_app.hwnd, WM_TRAY_CALLBACK, icon, L"quietkey — 后台播放/暂停")) {
        // 开机自启时资源管理器往往还没就绪，这里失败是常态，不能就此退出——
        // 热键照常工作，图标稍后由定时器 / TaskbarCreated 补上。
        LogF(L"托盘图标暂时添加失败，稍后重试");
        SetTimer(g_app.hwnd, kTrayRetryTimer, kTrayRetryIntervalMs, nullptr);
    }

    ApplyHotkeyFromConfig(true);
    RefreshTrayTooltip();

    // 第一次运行（还没有配置文件）就把设置界面摆出来，
    // 否则用户只看到一个托盘图标，不知道该干什么。
    // 同时把默认配置落盘：否则用户不点「保存」直接关窗，
    // 之后每次启动（包括开机自启）都会再弹一次。
    if (GetFileAttributesW(ConfigPath().c_str()) == INVALID_FILE_ATTRIBUTES) {
        g_app.cfg.Save();
        OpenSettings();
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        // 非模态对话框的 Tab / 回车 / 快捷键都靠它，不转交的话键盘在对话框里是废的。
        if (SettingsHandleMessage(&msg)) {
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    LogF(L"已退出");
    return 0;
}
