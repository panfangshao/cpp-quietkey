// 托盘图标与右键菜单。
//
// 两个必须处理、否则会表现成"偶发失灵"的细节：
//
//  1. **资源管理器重启后图标会消失。** explorer.exe 崩溃或重启时，托盘区被重建，
//     我们的图标不会自动回来。系统会广播 "TaskbarCreated" 消息，收到就重新加一次。
//  2. **弹菜单前必须 SetForegroundWindow。** 这是 Win32 的老坑：不做的话，
//     菜单弹出来之后点别处不会消失，会一直赖在屏幕上。
#pragma once

#include <windows.h>
#include <shellapi.h>  // NOTIFYICONDATAW

class Tray {
public:
    ~Tray();

    /// 创建托盘图标。`callbackMsg` 是鼠标事件回传给 hwnd 的自定义消息号。
    bool Create(HWND hwnd, UINT callbackMsg, HICON icon, const wchar_t* tooltip);
    void Destroy();

    /// 重新添加图标：资源管理器重启后，或启动时 Create 失败后的重试。
    /// 已经在托盘里就直接返回 true。
    bool Readd();

    /// 在鼠标位置弹出右键菜单。菜单项 ID 见 resource.h。
    void ShowContextMenu(HWND hwnd, POINT pt);

    /// 气泡通知。用于把"热键触发了什么"这类结果告诉用户。
    void Balloon(const wchar_t* title, const wchar_t* text);

    /// 更新鼠标悬停提示。程序常年只有一个图标，把「当前热键 + 触发次数」
    /// 放进提示里，用户悬停一下就知道它还活着、热键是哪个。
    void SetTooltip(const wchar_t* tooltip);

    /// 系统广播的 "TaskbarCreated" 消息号，主窗口过程要拿它做比较。
    static UINT TaskbarCreatedMessage();

private:
    NOTIFYICONDATAW data_{};
    bool added_ = false;
};
