#include "settings.h"

#include <commctrl.h>

#include <string>
#include <vector>

#include "hotkey.h"
#include "log.h"
#include "resource.h"
#include "winutil.h"

namespace {

HWND g_dlg = nullptr;
Config* g_cfg = nullptr;
void (*g_onApply)(const Config&) = nullptr;
ActionReport g_lastReport;
bool g_hasReport = false;
// 界面上选中的目标窗口（草稿）。和其他设置一样等「保存并应用」才写回配置并落盘——
// 以前是选完立即改内存里的配置却不落盘，关窗后看着生效、重启就丢。
TargetSpec g_draftTarget;
std::wstring g_defaultHotkeyHint;  // 资源里写的热键说明，含 Win 键提示切换时要还原

// ---- 热键控件与 MOD_* 的互转 ----
// msctls_hotkey32 用的是 HOTKEYF_*，和 RegisterHotKey 的 MOD_* 不是一套值。
// 另外这个控件**不支持 Win 键**，需要 Win 组合的话直接改 config.ini。

WORD SpecToHotkeyCtrl(const HotkeySpec& spec) {
    BYTE flags = 0;
    if (spec.modifiers & MOD_SHIFT) flags |= HOTKEYF_SHIFT;
    if (spec.modifiers & MOD_CONTROL) flags |= HOTKEYF_CONTROL;
    if (spec.modifiers & MOD_ALT) flags |= HOTKEYF_ALT;
    return static_cast<WORD>(MAKEWORD(spec.vk, flags));
}

HotkeySpec HotkeyCtrlToSpec(WORD raw) {
    HotkeySpec spec;
    spec.vk = LOBYTE(raw);
    const BYTE flags = HIBYTE(raw);
    if (flags & HOTKEYF_SHIFT) spec.modifiers |= MOD_SHIFT;
    if (flags & HOTKEYF_CONTROL) spec.modifiers |= MOD_CONTROL;
    if (flags & HOTKEYF_ALT) spec.modifiers |= MOD_ALT;
    return spec;
}

void SetText(HWND dlg, int id, const std::wstring& text) {
    SetDlgItemTextW(dlg, id, text.c_str());
}

std::wstring GetText(HWND dlg, int id) {
    wchar_t buf[512] = {};
    GetDlgItemTextW(dlg, id, buf, _countof(buf));
    return buf;
}

void Check(HWND dlg, int id, bool on) {
    CheckDlgButton(dlg, id, on ? BST_CHECKED : BST_UNCHECKED);
}

bool IsChecked(HWND dlg, int id) {
    return IsDlgButtonChecked(dlg, id) == BST_CHECKED;
}

// ---------------------------------------------------------------------------
// 窗口拾取器
// ---------------------------------------------------------------------------

struct PickerState {
    std::vector<WindowInfo> rows;     // 当前列表框里显示的
    TargetSpec picked;
    bool matchTitle = false;
};

std::wstring ToLowerCopy(std::wstring s) {
    for (wchar_t& c : s) {
        c = static_cast<wchar_t>(towlower(c));
    }
    return s;
}

void FillPickerList(HWND dlg, PickerState* st) {
    const std::wstring filter = ToLowerCopy(GetText(dlg, IDC_PICKER_FILTER));
    const std::vector<WindowInfo> all = ListWindows();

    st->rows.clear();
    HWND list = GetDlgItem(dlg, IDC_PICKER_LIST);
    SendMessageW(list, LB_RESETCONTENT, 0, 0);

    for (const WindowInfo& w : all) {
        if (!filter.empty() && ToLowerCopy(w.title).find(filter) == std::wstring::npos &&
            ToLowerCopy(w.exe).find(filter) == std::wstring::npos) {
            continue;
        }
        const std::wstring line = w.exe + L"\t" + w.title;
        SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(line.c_str()));
        st->rows.push_back(w);
    }

    // 列表框的制表位（对话框单位的一半），让进程名和标题对齐成两列。
    const int tabs[] = {110};
    SendMessageW(list, LB_SETTABSTOPS, 1, reinterpret_cast<LPARAM>(tabs));
}

bool CommitPickerSelection(HWND dlg, PickerState* st) {
    const LRESULT sel = SendDlgItemMessageW(dlg, IDC_PICKER_LIST, LB_GETCURSEL, 0, 0);
    if (sel == LB_ERR || sel < 0 || static_cast<size_t>(sel) >= st->rows.size()) {
        return false;
    }
    const WindowInfo& w = st->rows[static_cast<size_t>(sel)];
    st->picked.exe = w.exe;
    st->picked.className = w.className;
    // 标题默认不参与匹配：视频标题会随播放进度变，拿它匹配很脆。
    st->picked.titleContains = IsChecked(dlg, IDC_PICKER_MATCH_TITLE) ? w.title : std::wstring();
    return true;
}

INT_PTR CALLBACK PickerProc(HWND dlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* st = reinterpret_cast<PickerState*>(GetWindowLongPtrW(dlg, GWLP_USERDATA));

    switch (msg) {
        case WM_INITDIALOG: {
            SetWindowLongPtrW(dlg, GWLP_USERDATA, static_cast<LONG_PTR>(lParam));
            st = reinterpret_cast<PickerState*>(lParam);
            FillPickerList(dlg, st);
            return TRUE;
        }
        case WM_COMMAND: {
            const int id = LOWORD(wParam);
            const int code = HIWORD(wParam);
            if (id == IDC_PICKER_REFRESH) {
                FillPickerList(dlg, st);
                return TRUE;
            }
            if (id == IDC_PICKER_FILTER && code == EN_CHANGE) {
                FillPickerList(dlg, st);
                return TRUE;
            }
            if (id == IDC_PICKER_LIST && code == LBN_DBLCLK) {
                if (CommitPickerSelection(dlg, st)) {
                    EndDialog(dlg, IDOK);
                }
                return TRUE;
            }
            if (id == IDOK) {
                if (!CommitPickerSelection(dlg, st)) {
                    MessageBoxW(dlg, L"请先在列表里选中一个窗口。", L"quietkey",
                                MB_OK | MB_ICONINFORMATION);
                    return TRUE;
                }
                EndDialog(dlg, IDOK);
                return TRUE;
            }
            if (id == IDCANCEL) {
                EndDialog(dlg, IDCANCEL);
                return TRUE;
            }
            break;
        }
        default:
            break;
    }
    return FALSE;
}

// ---------------------------------------------------------------------------
// 设置对话框
// ---------------------------------------------------------------------------

void LoadIntoControls(HWND dlg) {
    HotkeySpec spec;
    const bool parsed = ParseHotkey(g_cfg->hotkey, &spec);
    SendDlgItemMessageW(dlg, IDC_HOTKEY, HKM_SETHOTKEY, parsed ? SpecToHotkeyCtrl(spec) : 0, 0);
    if (parsed && (spec.modifiers & MOD_WIN)) {
        // 控件显示不出 Win 键，至少说清楚：不动它就会原样保留。
        SetText(dlg, IDC_HOTKEY_HINT,
                L"当前热键是 " + FormatHotkey(spec) +
                    L"（控件显示不了 Win 键）。不改动这里，保存时会原样保留。");
    } else {
        SetText(dlg, IDC_HOTKEY_HINT, g_defaultHotkeyHint);
    }
    g_draftTarget = g_cfg->target;
    SetText(dlg, IDC_TARGET_TEXT, g_draftTarget.Describe());
    Check(dlg, IDC_S_GSMTC, g_cfg->strategies.gsmtc);
    Check(dlg, IDC_S_FOLLOW, g_cfg->followRecentSession);
    Check(dlg, IDC_S_APPCOMMAND, g_cfg->strategies.appcommand);
    Check(dlg, IDC_S_POSTMESSAGE, g_cfg->strategies.postmessage);
    Check(dlg, IDC_S_FALLBACK, g_cfg->strategies.focusSendInput);
    SetText(dlg, IDC_SETTLE, std::to_wstring(g_cfg->focusSettleMs));
    // 自启状态的真相在注册表，每次打开界面都现读，不缓存也不写进配置文件。
    Check(dlg, IDC_AUTOSTART, IsAutoStartEnabled());
}

/// 开机自启是立即生效的系统设置，不跟着「保存并应用」走，
/// 点一下就写注册表——否则用户勾了不保存就走，行为会和看到的不一致。
void OnAutoStartToggled(HWND dlg) {
    const bool want = IsChecked(dlg, IDC_AUTOSTART);
    if (SetAutoStart(want)) {
        return;
    }
    MessageBoxW(dlg,
                L"写入开机启动项失败。\n\n"
                L"通常是被安全软件拦了，可以到「任务管理器 → 启动应用」里手动添加。",
                L"quietkey", MB_OK | MB_ICONWARNING);
    Check(dlg, IDC_AUTOSTART, IsAutoStartEnabled());  // 状态回退到真实情况
}

/// 把界面上的内容收进一份草稿。返回 false 表示有非法输入（已提示用户）。
bool CollectFromControls(HWND dlg, Config* draft) {
    *draft = *g_cfg;

    const WORD raw = static_cast<WORD>(SendDlgItemMessageW(dlg, IDC_HOTKEY, HKM_GETHOTKEY, 0, 0));
    draft->target = g_draftTarget;

    // 配置里手写的 Win 组合：控件里只显示得出去掉 Win 之后的部分。
    // 用户没动控件就保留原值，否则改任何别的设置一保存，Win 就被悄悄丢掉了。
    HotkeySpec orig;
    if (ParseHotkey(g_cfg->hotkey, &orig) && (orig.modifiers & MOD_WIN) &&
        raw == SpecToHotkeyCtrl(orig)) {
        draft->hotkey = g_cfg->hotkey;
    } else {
        const HotkeySpec spec = HotkeyCtrlToSpec(raw);
        if (spec.vk == 0 || spec.modifiers == 0) {
            MessageBoxW(dlg,
                        L"热键必须带至少一个修饰键（Ctrl / Alt / Shift）。\n"
                        L"不带修饰键会把那个键从整个系统里抢走。",
                        L"quietkey", MB_OK | MB_ICONWARNING);
            return false;
        }
        draft->hotkey = FormatHotkey(spec);
    }

    draft->strategies.gsmtc = IsChecked(dlg, IDC_S_GSMTC);
    draft->followRecentSession = IsChecked(dlg, IDC_S_FOLLOW);
    draft->strategies.appcommand = IsChecked(dlg, IDC_S_APPCOMMAND);
    draft->strategies.postmessage = IsChecked(dlg, IDC_S_POSTMESSAGE);
    draft->strategies.focusSendInput = IsChecked(dlg, IDC_S_FALLBACK);

    const std::wstring settle = GetText(dlg, IDC_SETTLE);
    draft->focusSettleMs = settle.empty() ? 0 : static_cast<unsigned>(_wtoi(settle.c_str()));
    if (draft->focusSettleMs > 500) {
        draft->focusSettleMs = 500;
    }
    return true;
}

void ShowSessions(HWND dlg) {
    HWND list = GetDlgItem(dlg, IDC_SESSIONS);
    SendMessageW(list, LB_RESETCONTENT, 0, 0);

    const std::vector<SessionInfo> sessions = ListMediaSessions();
    if (sessions.empty()) {
        // 空列表是重要证据：播放器没注册媒体会话的话，第 1 层根本够不着它。
        SendMessageW(list, LB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(
                         L"系统当前没有任何媒体会话（播放器只有在播放带声音的媒体时才会注册）"));
        return;
    }
    for (const SessionInfo& s : sessions) {
        std::wstring line = s.playing ? L"▶ " : L"⏸ ";
        line += s.recent ? L"★ " : L"   ";
        line += s.aumid;
        if (!s.hasTimeline) {
            line += L"  · 不上报时间线";
        } else if (s.recent) {
            line += L"  · 最近碰过";
        } else {
            wchar_t buf[48];
            _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"  · %.1f 秒前碰过", s.behindSecs);
            line += buf;
        }
        SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(line.c_str()));
    }
}

void ShowLastReport(HWND dlg) {
    if (!g_hasReport) {
        SetText(dlg, IDC_LAST_SUMMARY, L"还没有触发过");
        SetText(dlg, IDC_TRACE, L"");
        return;
    }
    std::wstring summary = g_lastReport.Summary();
    if (g_lastReport.intrusive) {
        summary += L"    ⚠ 走到了兜底层，这次切换了焦点";
    }
    SetText(dlg, IDC_LAST_SUMMARY, summary);

    // trace 是主要排查手段：每一层试了什么、结果如何，逐行列出。
    std::wstring text;
    for (const TraceEntry& e : g_lastReport.trace) {
        text += e.layer + L"  [" + e.label + L"]  " + e.detail + L"\r\n";
    }
    SetText(dlg, IDC_TRACE, text);
}

void DoPickTarget(HWND dlg) {
    PickerState st;
    const INT_PTR r = DialogBoxParamW(
        reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(dlg, GWLP_HINSTANCE)),
        MAKEINTRESOURCEW(IDD_PICKER), dlg, PickerProc, reinterpret_cast<LPARAM>(&st));
    if (r != IDOK) {
        return;
    }
    // 先进草稿：「立即测试一次」马上能用上它，「保存并应用」才真正写回并落盘。
    g_draftTarget = st.picked;
    SetText(dlg, IDC_TARGET_TEXT, g_draftTarget.Describe() + L"（未保存）");
}

void DoTestOnce(HWND dlg) {
    // 用界面上的草稿测，方便边调边试，不必先保存。
    Config draft;
    if (!CollectFromControls(dlg, &draft)) {
        return;
    }
    const ActionReport report = TogglePlayPause(draft);
    SettingsRefreshLastReport(report);
}

void DoApply(HWND dlg) {
    Config draft;
    if (!CollectFromControls(dlg, &draft)) {
        return;
    }
    *g_cfg = draft;
    if (g_onApply) {
        g_onApply(draft);
    }
    // onApply 可能改回了配置（例如新热键挂不上、退回旧的），界面要跟着显示真实状态。
    LoadIntoControls(dlg);
    SettingsRefreshStatus();
}

INT_PTR CALLBACK SettingsProc(HWND dlg, UINT msg, WPARAM wParam, LPARAM) {
    switch (msg) {
        case WM_INITDIALOG: {
            g_dlg = dlg;
            g_defaultHotkeyHint = GetText(dlg, IDC_HOTKEY_HINT);
            // 有了任务栏按钮（见 app.rc 的 WS_EX_APPWINDOW），就得给它一个图标。
            const HINSTANCE inst =
                reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(dlg, GWLP_HINSTANCE));
            HICON small = nullptr;
            HICON big = nullptr;
            LoadIconMetric(inst, MAKEINTRESOURCEW(IDI_APP), LIM_SMALL, &small);
            LoadIconMetric(inst, MAKEINTRESOURCEW(IDI_APP), LIM_LARGE, &big);
            SendMessageW(dlg, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(small));
            SendMessageW(dlg, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(big));
            LoadIntoControls(dlg);
            ShowLastReport(dlg);
            SetText(dlg, IDC_STATUS, AppStatusLine());
            return TRUE;
        }

        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case IDOK:              DoApply(dlg); return TRUE;
                case IDC_HIDE:
                case IDCANCEL:          ShowWindow(dlg, SW_HIDE); return TRUE;
                case IDC_TARGET_PICK:   DoPickTarget(dlg); return TRUE;
                case IDC_TARGET_CLEAR:
                    g_draftTarget = TargetSpec{};
                    SetText(dlg, IDC_TARGET_TEXT, g_draftTarget.Describe() + L"（未保存）");
                    return TRUE;
                case IDC_TEST_ONCE:     DoTestOnce(dlg); return TRUE;
                case IDC_REFRESH_SESSIONS: ShowSessions(dlg); return TRUE;
                case IDC_AUTOSTART:     OnAutoStartToggled(dlg); return TRUE;
                default: break;
            }
            break;

        case WM_CLOSE:
            // 关窗不退出程序，只是缩回托盘——真正退出只走托盘菜单的「退出」。
            ShowWindow(dlg, SW_HIDE);
            return TRUE;

        case WM_DESTROY:
            g_dlg = nullptr;
            return TRUE;

        default:
            break;
    }
    return FALSE;
}

}  // namespace

void ShowSettingsDialog(HINSTANCE instance, HWND owner, Config* cfg,
                        void (*onApply)(const Config&)) {
    g_cfg = cfg;
    g_onApply = onApply;

    if (!g_dlg) {
        // 非模态：对话框开着的时候热键、托盘菜单都要照常工作，
        // 用 DialogBox 那种模态循环会把整个程序卡在里面。
        g_dlg = CreateDialogParamW(instance, MAKEINTRESOURCEW(IDD_SETTINGS), owner,
                                   SettingsProc, 0);
        if (!g_dlg) {
            LogF(L"创建设置对话框失败（错误码 %lu）", GetLastError());
            return;
        }
    } else if (!IsWindowVisible(g_dlg)) {
        // 关窗只是隐藏，WM_INITDIALOG 不会再来。重新打开时要按当前配置重填：
        // 否则显示的是上次没保存的草稿，自启勾选框也不是注册表里的真实状态。
        LoadIntoControls(g_dlg);
        ShowLastReport(g_dlg);
        SetText(g_dlg, IDC_STATUS, AppStatusLine());
    }
    if (IsIconic(g_dlg)) {
        ShowWindow(g_dlg, SW_RESTORE);
    }
    ShowWindow(g_dlg, SW_SHOW);
    SetForegroundWindow(g_dlg);
}

bool SettingsHandleMessage(MSG* msg) {
    return g_dlg && IsDialogMessageW(g_dlg, msg);
}

void SettingsRefreshLastReport(const ActionReport& report) {
    g_lastReport = report;
    g_hasReport = true;
    if (g_dlg) {
        ShowLastReport(g_dlg);
        SetText(g_dlg, IDC_STATUS, AppStatusLine());
    }
}

void SettingsRefreshStatus() {
    if (g_dlg) {
        SetText(g_dlg, IDC_STATUS, AppStatusLine());
    }
}
