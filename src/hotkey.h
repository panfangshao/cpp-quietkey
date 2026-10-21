// 全局热键：注册、注销，以及 "Alt+Q" 这类字符串与按键码的互转。
//
// 走系统的 RegisterHotKey，不用低级键盘钩子，理由：
//   * 钩子是 keylogger 的教科书特征，未签名的自编译程序容易被杀软拦；
//   * 钩子回调跑在本进程，受 LowLevelHooksTimeout 约束，在回调里做窗口查找
//     会拖慢**整个系统**的键盘响应；
//   * 钩子能看到你敲的每一个字符，RegisterHotKey 只能看到注册的那一个组合键。
// 附带好处：系统会独占消费注册过的组合键，不会再漏给正在打字的窗口。
#pragma once

#include <windows.h>

#include <string>

struct HotkeySpec {
    UINT modifiers = 0;  // MOD_ALT / MOD_CONTROL / MOD_SHIFT / MOD_WIN 的组合
    UINT vk = 0;         // 虚拟键码
};

/// 解析 "Alt+Q"、"Control+Alt+F9"、"Ctrl+Shift+P" 这类写法。
/// 分隔符可以是 + 或 -，大小写不敏感；解析失败返回 false 且不改动 out。
bool ParseHotkey(const std::wstring& text, HotkeySpec* out);

/// 反向格式化成 "Alt+Q" 这种规范写法，用于界面显示和写回配置。
std::wstring FormatHotkey(const HotkeySpec& spec);

/// 热键的注册状态。同一时刻只挂一个组合键，换键时先注销旧的。
class HotkeyRegistration {
public:
    ~HotkeyRegistration();

    /// 注册（或换成）新的组合键。
    /// 失败通常是组合键已被别的程序占用——这是 RegisterHotKey 的固有行为，
    /// 不是 bug，必须把错误暴露给用户，**不要静默吞掉**。
    /// 失败时原来的热键保持注册，不会落得一个都没有。
    bool Apply(HWND hwnd, const HotkeySpec& spec);

    void Clear();

    bool active() const { return active_; }
    const HotkeySpec& current() const { return current_; }

    /// WM_HOTKEY 消息里的 id 是不是我们注册的。换键时新旧两个 id 轮换，所以有两个。
    static bool IsOurId(WPARAM id) { return id == kIdA || id == kIdB; }

private:
    static constexpr int kIdA = 1;
    static constexpr int kIdB = 2;

    HWND hwnd_ = nullptr;
    int id_ = kIdA;
    HotkeySpec current_{};
    bool active_ = false;
};
