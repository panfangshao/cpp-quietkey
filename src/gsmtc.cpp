// 第 1 层：Windows 全局媒体传输控制（GSMTC）。
//
// 这是「一次都不切窗口」的正解。浏览器在播放视频时都会向系统注册一个媒体会话——
// 就是按音量键时弹出的那个媒体浮层背后的东西。我们直接枚举会话并调用
// TryTogglePlayPauseAsync，全程：
//
//   * 不需要焦点，不切窗口，不动前台
//   * 不需要窗口标题匹配（标题会随播放进度变，本来就不可靠）
//   * 窗口最小化、被完全遮挡都照样生效
//
// 注意：向浏览器顶层 HWND 投递合成的 WM_KEYDOWN 是**行不通**的，
// Chromium 的输入走自己的 renderer 管线会直接把它丢掉。所以对网页视频来说，
// 这一层不是「优化」，而是唯一可行的无感方案。**不要以「精简」为由删掉它。**

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>  // 遍历 IVectorView 要它
#include <winrt/Windows.Media.Control.h>

#include <algorithm>
#include <chrono>
#include <cwctype>

#include "log.h"
#include "recency.h"
#include "strategy.h"

using namespace winrt;
using namespace winrt::Windows::Media::Control;

namespace {

using Session = GlobalSystemMediaTransportControlsSession;
using SessionManager = GlobalSystemMediaTransportControlsSessionManager;
using PlaybackStatus = GlobalSystemMediaTransportControlsSessionPlaybackStatus;

/// 枚举到的一个会话。
struct Found {
    Session session{nullptr};
    std::wstring aumid;   // 小写
    bool playing = false;
    /// TimelineProperties.LastUpdatedTime，取不到时为 0。
    /// 它比播放状态更能反映"这个会话被碰过"：手动播了又停这种**净状态零变化**
    /// 的操作，状态比对完全看不见，但它会被刷新。
    int64_t touchedAt = 0;
};

std::wstring ToLower(std::wstring s) {
    for (wchar_t& c : s) {
        c = static_cast<wchar_t>(std::towlower(c));
    }
    return s;
}

/// "chrome.exe" -> "chrome"，用于和 AppUserModelId 做模糊比对。
std::wstring ExeStem(const std::wstring& exe) {
    std::wstring s = ToLower(exe);
    const size_t dot = s.rfind(L".exe");
    if (dot != std::wstring::npos && dot + 4 == s.size()) {
        s.erase(dot);
    }
    return s;
}

// ---- 带超时的等待 ----
// `.get()` 没有上限：媒体服务或播放器（比如卡住的浏览器渲染进程）不回话时，
// 整个程序——热键、托盘、设置窗口——都跟着卡死在这一个调用上。

constexpr std::chrono::milliseconds kRequestTimeout{1500};
constexpr std::chrono::milliseconds kCommandTimeout{1500};

struct AwaitTimeout {
    const wchar_t* what;
};

/// 等异步操作完成并取结果；超时抛 AwaitTimeout，出错照常抛 hresult_error。
template <typename Op>
auto Await(const Op& op, std::chrono::milliseconds limit, const wchar_t* what) {
    if (op.wait_for(limit) == Windows::Foundation::AsyncStatus::Started) {
        op.Cancel();
        throw AwaitTimeout{what};
    }
    return op.GetResults();
}

/// 从 AUMID 认不出是哪个程序的会话：空的，或者是一串十六进制哈希
/// （Firefox 用安装路径的哈希当 AUMID，如 308046b0af4a39cb）。
/// 配置了目标却一个都没对上时，这种会话仍可能正是目标，不能排除。
bool Unidentifiable(const std::wstring& aumid) {
    if (aumid.empty()) {
        return true;
    }
    if (aumid.size() < 16) {
        return false;
    }
    return std::all_of(aumid.begin(), aumid.end(), [](wchar_t c) { return std::iswxdigit(c) != 0; });
}

bool MatchesAny(const std::wstring& aumid, const std::vector<std::wstring>& wanted) {
    if (aumid.empty()) {
        return false;
    }
    for (const std::wstring& w : wanted) {
        if (w.empty()) continue;
        if (aumid.find(w) != std::wstring::npos || w.find(aumid) != std::wstring::npos) {
            return true;
        }
    }
    return false;
}

std::wstring JoinNames(const std::vector<std::wstring>& names) {
    std::wstring out;
    for (const std::wstring& n : names) {
        if (n.empty() || out.find(n) != std::wstring::npos) continue;
        if (!out.empty()) out += L"/";
        out += n;
    }
    return out;
}

std::vector<Found> Collect(const SessionManager& manager) {
    std::vector<Found> all;
    for (const Session& s : manager.GetSessions()) {
        Found f;
        f.session = s;
        try {
            f.aumid = ToLower(std::wstring(s.SourceAppUserModelId().c_str()));
        } catch (const hresult_error&) {
        }
        try {
            f.playing = s.GetPlaybackInfo().PlaybackStatus() == PlaybackStatus::Playing;
        } catch (const hresult_error&) {
        }
        try {
            f.touchedAt = s.GetTimelineProperties().LastUpdatedTime().time_since_epoch().count();
        } catch (const hresult_error&) {
        }
        all.push_back(std::move(f));
    }
    return all;
}

std::vector<SeenSession> SeenRows(const std::vector<Found>& all) {
    // **不做任何过滤**：下标要和 all 一一对应，MostRecentIndex 返回的下标
    // 才能直接拿来索引 all。
    std::vector<SeenSession> rows;
    rows.reserve(all.size());
    for (const Found& f : all) {
        rows.push_back(SeenSession{f.aumid, f.touchedAt});
    }
    return rows;
}

std::wstring JoinAumids(const std::vector<Found>& all) {
    std::wstring names;
    for (const Found& f : all) {
        if (!names.empty()) names += L", ";
        names += f.aumid.empty() ? std::wstring(L"(未知)") : f.aumid;
    }
    return names;
}

std::wstring ShortError(const hresult_error& e) {
    std::wstring msg(e.message().c_str());
    if (msg.empty()) {
        wchar_t buf[32];
        _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"HRESULT 0x%08X",
                     static_cast<unsigned>(e.code().value));
        return buf;
    }
    return msg;
}

}  // namespace

std::vector<SessionInfo> ListMediaSessions() {
    std::vector<SessionInfo> out;
    try {
        const SessionManager manager =
            Await(SessionManager::RequestAsync(), kRequestTimeout, L"获取会话管理器");
        const std::vector<Found> all = Collect(manager);
        const std::vector<SeenSession> rows = SeenRows(all);
        const int recent = MostRecentIndex(rows);

        int64_t newest = 0;
        for (const Found& f : all) {
            newest = (std::max)(newest, f.touchedAt);
        }
        for (size_t i = 0; i < all.size(); ++i) {
            SessionInfo info;
            info.aumid = all[i].aumid;
            info.playing = all[i].playing;
            info.recent = (static_cast<int>(i) == recent);
            info.hasTimeline = all[i].touchedAt > 0;
            info.behindSecs = BehindSeconds(all[i].touchedAt, newest);
            out.push_back(std::move(info));
        }
    } catch (const AwaitTimeout& t) {
        LogF(L"枚举媒体会话失败: %s超时", t.what);
    } catch (const hresult_error& e) {
        LogF(L"枚举媒体会话失败: %s", ShortError(e).c_str());
    }
    return out;
}

/// 目标会话的挑选顺序（第一个命中即用）：
///   1. 配置匹配   —— 设了目标（进程名或解析出的窗口）就只在属于它的会话里挑
///   2. 最近活跃   —— 见 recency.h，followRecent 控制
///   3. 唯一在播放 —— 只开了一个视频的日常场景
///   4. 系统当前会话（GetCurrentSession）
///   5. 唯一会话
///
/// 第 1 条不是"命中就用、不中就算了"，而是**划定候选范围**：设了目标却一个会话都
/// 没对上时，不能退去控制别的播放器（用户配的是 VLC，结果暂停了 Chrome），
/// 而是让出给第 2~4 层按窗口处理。唯一的例外是从 AUMID 认不出身份的会话
/// （见 Unidentifiable），它们可能正是目标，留在候选里。
/// 第 1 条匹配到多个（一个浏览器开着多个视频标签页）时，在它们之间继续走 2~5 条。
///
/// 第 4、5 条是「能暂停、不能恢复」的解药：视频一旦被我们暂停，
/// 正在播放的会话数就变成 0，只靠第 3 条会彻底找不回目标，
/// 于是掉到对浏览器无效的第 2/3 层，表现为再按一次没反应。
Outcome GsmtcToggle(const TargetSpec& spec, const ResolvedTarget* resolved,
                    bool followRecent, std::wstring* activity, bool* sawSessions) {
    if (sawSessions) {
        *sawSessions = false;
    }
    try {
        const SessionManager manager =
            Await(SessionManager::RequestAsync(), kRequestTimeout, L"获取会话管理器");
        const std::vector<Found> all = Collect(manager);
        if (all.empty()) {
            return Outcome::Skip(L"系统当前没有任何媒体会话");
        }
        if (sawSessions) {
            *sawSessions = true;
        }

        const std::vector<SeenSession> rows = SeenRows(all);
        if (activity) {
            *activity = DescribeActivity(rows);
        }

        // 配置里的进程名，以及实际解析到的窗口的进程名，都拿来做匹配依据。
        std::vector<std::wstring> wanted;
        if (!spec.exe.empty()) {
            wanted.push_back(ExeStem(spec.exe));
        }
        if (resolved && !resolved->exe.empty()) {
            wanted.push_back(ExeStem(resolved->exe));
        }

        // ---- 第 1 条：划定候选范围 ----
        std::vector<size_t> pool;
        bool configMatched = false;
        if (wanted.empty()) {
            for (size_t i = 0; i < all.size(); ++i) pool.push_back(i);
        } else {
            std::vector<size_t> unknown;
            for (size_t i = 0; i < all.size(); ++i) {
                if (MatchesAny(all[i].aumid, wanted)) {
                    pool.push_back(i);
                } else if (Unidentifiable(all[i].aumid)) {
                    unknown.push_back(i);
                }
            }
            if (!pool.empty()) {
                configMatched = true;
            } else if (!unknown.empty()) {
                pool = unknown;  // 认不出身份的会话可能正是目标
            } else {
                return Outcome::Skip(L"没有属于目标 " + JoinNames(wanted) + L" 的会话（现有：" +
                                     JoinAumids(all) + L"）");
            }
        }

        int idx = -1;
        std::wstring route;
        if (configMatched && pool.size() == 1) {
            idx = static_cast<int>(pool[0]);
            route = L"配置匹配";
        }

        // ---- 第 2 条：最近活跃 ----
        if (idx < 0 && followRecent) {
            // 有会话**正在播放却不上报时间线**时，时间戳比较是缺了一方的：
            // 它永远赢不了，热键会去恢复某个早就暂停了的会话，而不是暂停正在播的这个。
            // 这种情况下时间戳不可信，交给"唯一在播放"。
            bool untrackedPlaying = false;
            std::vector<SeenSession> sub;
            for (size_t i : pool) {
                untrackedPlaying = untrackedPlaying || (all[i].playing && all[i].touchedAt <= 0);
                sub.push_back(rows[i]);
            }
            if (!untrackedPlaying) {
                // 按会话逐个比时间戳，不按进程名归并——所以一个浏览器开多个视频标签页、
                // 各自注册一个会话时，也能精确挑到你最近碰过的那一个。
                const int k = MostRecentIndex(sub);
                if (k >= 0) {
                    idx = static_cast<int>(pool[static_cast<size_t>(k)]);
                    route = L"最近活跃";
                }
            }
        }

        // ---- 第 3 条：唯一在播放 ----
        if (idx < 0) {
            int found = -1;
            for (size_t i : pool) {
                if (!all[i].playing) continue;
                if (found >= 0) {
                    found = -1;  // 不止一个
                    break;
                }
                found = static_cast<int>(i);
            }
            if (found >= 0) {
                idx = found;
                route = L"唯一在播放";
            }
        }

        // ---- 第 4 条：系统当前会话 ----
        if (idx < 0) {
            Session cur{nullptr};
            try {
                cur = manager.GetCurrentSession();
            } catch (const hresult_error&) {
            }
            if (cur) {
                // 先按对象身份比：同一个浏览器的多个标签页 AUMID 完全相同，
                // 按 AUMID 比只会永远挑中第一个。
                for (size_t i : pool) {
                    if (all[i].session == cur) {
                        idx = static_cast<int>(i);
                        break;
                    }
                }
                // 身份对不上（理论上可能是不同的代理对象）时退回 AUMID，
                // 但只在候选里恰好一个同名会话时才敢用。
                if (idx < 0) {
                    std::wstring curAumid;
                    try {
                        curAumid = ToLower(std::wstring(cur.SourceAppUserModelId().c_str()));
                    } catch (const hresult_error&) {
                    }
                    int found = -1;
                    for (size_t i : pool) {
                        if (curAumid.empty() || all[i].aumid != curAumid) continue;
                        if (found >= 0) {
                            found = -1;
                            break;
                        }
                        found = static_cast<int>(i);
                    }
                    idx = found;
                }
                if (idx >= 0) route = L"系统当前会话";
            }
        }

        // ---- 第 5 条：唯一会话 ----
        if (idx < 0 && pool.size() == 1) {
            idx = static_cast<int>(pool[0]);
            route = L"唯一会话";
        }

        if (idx < 0) {
            return Outcome::Skip(std::to_wstring(all.size()) + L" 个会话中挑不出目标（" +
                                 JoinAumids(all) + L"）");
        }
        if (configMatched && route != L"配置匹配") {
            route = L"配置匹配·" + route;
        }

        const Found& target = all[static_cast<size_t>(idx)];
        const wchar_t* direction = target.playing ? L"已暂停" : L"已播放";

        bool toggleEnabled = true;
        try {
            toggleEnabled = target.session.GetPlaybackInfo().Controls().IsPlayPauseToggleEnabled();
        } catch (const hresult_error&) {
        }

        try {
            // 我们自己造成的这次切换同样会刷新会话的时间线时间戳，
            // 所以"暂停之后再按一次恢复"天然落在同一个会话上，不需要额外记账。
            if (toggleEnabled) {
                if (Await(target.session.TryTogglePlayPauseAsync(), kCommandTimeout, L"播放/暂停")) {
                    return Outcome::Applied(target.aumid + L" → " + direction + L"（" + route + L"）");
                }
            }

            // Toggle 被声明为不可用、或调用后被拒绝时，按当前状态退到明确的 Play / Pause——
            // 有些播放器只在暂停态下开放 Play，Toggle 一直报 false，
            // 不退这一步就永远恢复不了。
            const bool ok =
                target.playing
                    ? Await(target.session.TryPauseAsync(), kCommandTimeout, L"暂停")
                    : Await(target.session.TryPlayAsync(), kCommandTimeout, L"播放");
            if (ok) {
                return Outcome::Applied(target.aumid + L" → " + direction + L"（" + route + L"·显式）");
            }
        } catch (const AwaitTimeout& t) {
            // 命令已经发给播放器了，只是它迟迟不回话——很可能稍后照样执行。
            // 此时若降级到下一层再切一次，两次一抵消等于没按。所以和第 3 层一样
            // 乐观报成功，但说清楚无法确认。
            return Outcome::Applied(target.aumid + L" · " + t.what + L"请求已发出，但播放器 " +
                                    std::to_wstring(kCommandTimeout.count()) +
                                    L"ms 内没有回应（无法确认）");
        }
        return Outcome::Failed(L"会话 " + target.aumid + L" 拒绝了播放/暂停请求");
    } catch (const AwaitTimeout& t) {
        return Outcome::Skip(std::wstring(L"媒体会话服务无响应（") + t.what + L"超时）");
    } catch (const hresult_error& e) {
        return Outcome::Skip(L"媒体会话不可用: " + ShortError(e));
    }
}
