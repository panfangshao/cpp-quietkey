// Win32 窗口相关的封装：枚举、按规则解析目标、抢前台、发按键。
#pragma once

#include <windows.h>

#include <string>
#include <vector>

#include "config.h"

struct WindowInfo {
    HWND hwnd = nullptr;
    DWORD pid = 0;
    std::wstring title;
    std::wstring className;
    std::wstring exe;   // 进程可执行文件名，如 "chrome.exe"
};

struct ResolvedTarget {
    HWND hwnd = nullptr;
    std::wstring title;
    std::wstring exe;
    std::wstring className;
};

/// Chromium 系窗口（Chrome / Edge / Electron 应用……）。它们的键盘输入走 renderer 管线，
/// 投递给顶层 HWND 的合成键盘消息会被直接丢掉。
bool IsChromiumWindowClass(const std::wstring& className);

/// 列出可见的顶层窗口，供「窗口拾取器」展示。
///
/// 过滤掉 owner 窗口、WS_EX_TOOLWINDOW、以及 **DWM cloaked** 窗口。
/// cloaked 检查是必需的：去掉之后列表里会塞满看不见的 UWP 幽灵窗口。
std::vector<WindowInfo> ListWindows();

/// 按匹配规则实时解析出当前的目标窗口。
///
/// 多个命中时取**标题最长**的——浏览器真正在播视频的标签页，
/// 标题总比「新标签页」长。
bool ResolveTarget(const TargetSpec& spec, ResolvedTarget* out);

HWND GetForegroundHwnd();
std::wstring GetWindowTitleText(HWND hwnd);

/// 把窗口抢到前台。
///
/// 后台进程裸调 SetForegroundWindow 会被 Windows 的前台锁定机制拒绝，
/// 必须先 AttachThreadInput 把自己"挂"到当前前台线程的输入队列上。
bool ForceForeground(HWND hwnd);

/// 等用户松开所有修饰键（Ctrl/Alt/Shift/Win），最多等 `timeoutMs`。返回是否已全部松开。
///
/// WM_HOTKEY 在组合键**按下**时就到达，那一刻用户的手还按着修饰键。
/// 这时 SendInput 一个空格，目标收到的是 Alt+Space（弹系统菜单）、
/// Ctrl+Space（切输入法）之类，而不是空格。
bool WaitModifiersReleased(unsigned timeoutMs);

/// 用 SendInput 发一个按键（按下 + 抬起）。作用于当前前台窗口。
/// 若此刻仍有修饰键按着，会先合成它们的抬起，保证目标收到的是"裸"按键。
bool SendKeyToForeground(UINT vk);
