// 第 2、3 层：往目标窗口投递消息，全程不碰前台焦点。

#include "log.h"
#include "strategy.h"

namespace {

/// 目标窗口当前的焦点子窗口。
///
/// 键盘消息要投给真正有焦点的那个子窗口，投给顶层窗口往往被忽略。
/// 要读别的线程的焦点必须先 AttachThreadInput——GetFocus 只认本线程。
HWND FocusedChildOf(HWND top) {
    const DWORD targetThread = GetWindowThreadProcessId(top, nullptr);
    const DWORD myThread = GetCurrentThreadId();
    HWND focus = nullptr;
    if (targetThread != 0 && targetThread != myThread &&
        AttachThreadInput(myThread, targetThread, TRUE)) {
        focus = GetFocus();
        AttachThreadInput(myThread, targetThread, FALSE);
    }
    return focus ? focus : top;
}

}  // namespace

Outcome AppCommandPlayPause(const ResolvedTarget& target) {
    // **用 SendMessageTimeoutW 而不是 PostMessageW，是刻意的。**
    // 返回值是区分「对方接住了」和「对方无视了」的唯一依据：处理 WM_APPCOMMAND
    // 的程序返回 TRUE，走 DefWindowProc 的返回 0。换成 PostMessageW 会让这一层
    // 永远报成功，把第 3、4 层永久饿死。
    const LPARAM lparam = MAKELPARAM(0, FAPPCOMMAND_KEY | APPCOMMAND_MEDIA_PLAY_PAUSE);
    DWORD_PTR result = 0;
    const LRESULT sent = SendMessageTimeoutW(target.hwnd, WM_APPCOMMAND,
                                             reinterpret_cast<WPARAM>(target.hwnd), lparam,
                                             SMTO_ABORTIFHUNG | SMTO_NORMAL, 120, &result);
    if (sent == 0) {
        return Outcome::Skip(target.exe + L" 未响应 WM_APPCOMMAND（超时或无响应）");
    }
    if (result == 0) {
        return Outcome::Skip(target.exe + L" 未处理 WM_APPCOMMAND");
    }
    return Outcome::Applied(L"WM_APPCOMMAND 已被 " + target.exe + L" 处理");
}

Outcome PostSpace(const ResolvedTarget& target) {
    // Chromium 系根本不读顶层 HWND 队列里的合成键盘消息。这一层又"无法确认"、
    // 只会乐观报成功，对它们放行的话既没效果，又把第 4 层和失败提示一起饿死。
    if (IsChromiumWindowClass(target.className)) {
        return Outcome::Skip(target.exe + L" 是 Chromium 系窗口，投递的键盘消息会被丢弃");
    }
    const HWND focus = FocusedChildOf(target.hwnd);
    const UINT scan = MapVirtualKeyW(VK_SPACE, MAPVK_VK_TO_VSC);
    const LPARAM down = static_cast<LPARAM>(1 | (scan << 16));
    const LPARAM up = static_cast<LPARAM>(1 | (scan << 16) | (1u << 30) | (1u << 31));

    if (!PostMessageW(focus, WM_KEYDOWN, VK_SPACE, down)) {
        return Outcome::Failed(L"PostMessage 失败（错误码 " + std::to_wstring(GetLastError()) + L"）");
    }
    PostMessageW(focus, WM_KEYUP, VK_SPACE, up);

    // **这一层无法确认任何事。** PostMessage 是异步的，只表示"塞进队列了"，
    // 对方收没收、认不认都不知道。所以乐观返回成功，但在 detail 里说清楚。
    return Outcome::Applied(L"空格已投递到 " + target.exe + L" 的窗口（无法确认是否响应）");
}
