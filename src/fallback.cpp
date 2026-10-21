// 第 4 层：抢焦点 → SendInput → 立刻还原。**唯一会打断用户的一层。**
//
// 常见脚本就是这么干的，但这里把它做对了：
//
//   * 不用「三段 sleep(0.5) 硬等」那套。切换后只等一个可配置的极短间隔
//     （默认 30ms），够窗口管理器完成焦点转移即可。这个等待去不掉——
//     SetForegroundWindow 返回时前台切换还没走完，立刻 SendInput 会打到旧窗口上，
//     这是 Win32 的固有行为，和用什么语言、什么按键库都无关。
//   * 发完按键**立刻**把前台还原，而且还原的是「触发热键那一刻的前台窗口」，
//     不是配置里的目标窗口——你在哪个窗口按的热键，焦点就回到哪里。
//
// 即便如此屏幕仍会闪一下，所以默认关闭，只在前三层都对某个播放器无效时才开。

#include "log.h"
#include "strategy.h"

namespace {

std::wstring Truncate(const std::wstring& s, size_t maxChars) {
    if (s.size() <= maxChars) {
        return s;
    }
    return s.substr(0, maxChars) + L"…";
}

}  // namespace

Outcome FocusAndSendSpace(const ResolvedTarget& target, unsigned settleMs) {
    // 先等用户松开热键里的修饰键：否则目标收到的是 Alt+Space（系统菜单）之类。
    // 放在切前台之前等，不会延长屏幕"闪"的时间。等不到的话 SendKeyToForeground
    // 会自己合成抬起。
    WaitModifiersReleased(500);

    // 先记下"我是从哪儿按的热键"，这才是待会要还原的窗口。
    const HWND origin = GetForegroundHwnd();

    if (origin == target.hwnd) {
        // 目标已经在前台，直接发按键，不需要来回切。
        return SendKeyToForeground(VK_SPACE)
                   ? Outcome::Applied(target.exe + L" 已在前台，直接发送空格")
                   : Outcome::Failed(L"SendInput 失败");
    }

    // ForceForeground 会把最小化的窗口还原出来；记下原状，用完再缩回去，
    // 否则视频窗口从此摊在屏幕上。
    const bool wasMinimized = IsIconic(target.hwnd) != FALSE;
    if (!ForceForeground(target.hwnd)) {
        if (wasMinimized) {
            ShowWindow(target.hwnd, SW_SHOWMINNOACTIVE);
        }
        return Outcome::Failed(L"无法把 " + target.exe + L" 切到前台");
    }
    const unsigned settle = settleMs > 500 ? 500 : settleMs;
    Sleep(settle);

    const bool sent = SendKeyToForeground(VK_SPACE);

    // 无论按键是否发成功，都必须把焦点还回去，否则用户会卡在视频窗口里。
    Sleep(settle);
    if (wasMinimized) {
        ShowWindow(target.hwnd, SW_SHOWMINNOACTIVE);
    }
    bool restored = false;
    if (origin && IsWindow(origin)) {
        restored = ForceForeground(origin);
    }

    if (!sent) {
        return Outcome::Failed(L"SendInput 失败");
    }
    std::wstring back = L"但焦点还原失败";
    if (restored) {
        const std::wstring title = GetWindowTitleText(origin);
        back = title.empty() ? L"已还原焦点" : (L"焦点已还给「" + Truncate(title, 24) + L"」");
    }
    return Outcome::Applied(L"切到 " + target.exe + L" 发送空格，" + back);
}
