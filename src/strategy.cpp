#include "strategy.h"

#include <windows.h>

#include <cstdio>

#include "log.h"

namespace {

double MillisSince(LARGE_INTEGER start) {
    LARGE_INTEGER freq{}, now{};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    if (freq.QuadPart == 0) {
        return 0.0;
    }
    return static_cast<double>(now.QuadPart - start.QuadPart) * 1000.0 /
           static_cast<double>(freq.QuadPart);
}

void Push(ActionReport* r, const wchar_t* layer, const Outcome& out) {
    r->trace.push_back(TraceEntry{layer, out.Label(), out.detail});
}

}  // namespace

std::wstring ActionReport::Summary() const {
    wchar_t buf[512];
    if (winner.empty()) {
        _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"未生效 · %s", detail.c_str());
    } else {
        _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"%s · %s · %.1fms",
                     winner.c_str(), detail.c_str(), elapsedMs);
    }
    return buf;
}

ActionReport TogglePlayPause(const Config& cfg) {
    LARGE_INTEGER start{};
    QueryPerformanceCounter(&start);

    ActionReport report;

    ResolvedTarget target;
    const bool hasTarget = ResolveTarget(cfg.target, &target);
    if (hasTarget) {
        // 把实际命中的窗口写进 trace：匹配规则写错时，这一行是最快的线索。
        report.trace.push_back(
            TraceEntry{L"目标窗口", L"命中", target.exe + L" · 「" + target.title + L"」"});
    }

    // 第 1 层：GSMTC。它不依赖窗口句柄，所以即使窗口没解析出来也值得一试。
    bool gsmtcRan = false;
    bool sawSessions = false;
    Outcome gsmtcOut;
    if (cfg.strategies.gsmtc) {
        gsmtcRan = true;
        std::wstring activity;
        const Outcome out = GsmtcToggle(cfg.target, hasTarget ? &target : nullptr,
                                        cfg.followRecentSession, &activity, &sawSessions);
        if (!activity.empty()) {
            // 排查"为什么控的是它"时，这一行是最直接的证据。
            LogF(L"会话活跃度: %s", activity.c_str());
        }
        Push(&report, L"GSMTC 媒体会话", out);
        gsmtcOut = out;
        if (out.kind == OutcomeKind::Applied) {
            report.ok = true;
            report.winner = L"GSMTC 媒体会话";
            report.detail = out.detail;
            report.elapsedMs = MillisSince(start);
            return report;
        }
    }

    // 后面三层都需要一个具体的 HWND。
    if (!hasTarget) {
        if (!cfg.target.Empty()) {
            report.detail = L"没有窗口匹配规则 [" + cfg.target.Describe() + L"]";
        } else if (!gsmtcRan) {
            report.detail = L"第 1 层（媒体会话）已关闭，又没有设置目标窗口，没有任何一层可用";
        } else if (gsmtcOut.kind == OutcomeKind::Failed) {
            // 会话找到了，是它拒绝了请求——原因就在第 1 层的 detail 里，别用别的话盖掉。
            report.detail = gsmtcOut.detail;
        } else if (!sawSessions) {
            // 最常见也最容易误导人的一种失败：播放器根本没注册媒体会话
            // （标签页被浏览器丢弃、视频被静音、或者压根没在播）。
            // 这时说"尚未设置目标窗口"是答非所问，会让人以为程序坏了。
            report.detail =
                L"系统里没有任何正在播放的媒体：播放器没有注册媒体会话。"
                L"刷新一下播放页面或重新点播放通常就能恢复；"
                L"如果这个播放器从来不出现在「诊断 → 刷新媒体会话」列表里，"
                L"就得改用第 2/3 层并设置目标窗口。";
        } else {
            report.detail = L"尚未设置目标窗口（有媒体会话但没匹配上）";
        }
        report.elapsedMs = MillisSince(start);
        return report;
    }

    if (cfg.strategies.appcommand) {
        const Outcome out = AppCommandPlayPause(target);
        Push(&report, L"WM_APPCOMMAND", out);
        if (out.kind == OutcomeKind::Applied) {
            report.ok = true;
            report.winner = L"WM_APPCOMMAND";
            report.detail = out.detail;
            report.elapsedMs = MillisSince(start);
            return report;
        }
    }

    if (cfg.strategies.postmessage) {
        const Outcome out = PostSpace(target);
        Push(&report, L"PostMessage 按键", out);
        if (out.kind == OutcomeKind::Applied) {
            report.ok = true;
            report.winner = L"PostMessage 按键";
            report.detail = out.detail;
            report.elapsedMs = MillisSince(start);
            return report;
        }
    }

    if (cfg.strategies.focusSendInput) {
        const Outcome out = FocusAndSendSpace(target, cfg.focusSettleMs);
        Push(&report, L"抢焦点 SendInput", out);
        if (out.kind == OutcomeKind::Applied) {
            report.ok = true;
            report.winner = L"抢焦点 SendInput";
            report.detail = out.detail;
            report.intrusive = true;  // 只有这一层会打断用户，界面要标黄警告
            report.elapsedMs = MillisSince(start);
            return report;
        }
    }

    report.detail = L"所有已启用的策略都未生效";
    report.elapsedMs = MillisSince(start);
    return report;
}
