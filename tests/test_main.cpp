// 纯函数的单元测试：热键字符串互转、最近活跃判定。
//
// 不引入测试框架（项目约定零第三方依赖），一个 CHECK 宏够用。
// 跑法：build\quietkey_tests.exe，退出码非 0 即有失败。

#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

#include "hotkey.h"
#include "recency.h"

namespace {

int g_failures = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);   \
            ++g_failures;                                                 \
        }                                                                 \
    } while (0)

bool Parses(const wchar_t* text, UINT mods, UINT vk) {
    HotkeySpec s;
    return ParseHotkey(text, &s) && s.modifiers == mods && s.vk == vk;
}

bool Rejects(const wchar_t* text) {
    HotkeySpec s;
    return !ParseHotkey(text, &s);
}

void TestParseHotkey() {
    CHECK(Parses(L"Alt+Q", MOD_ALT, 'Q'));
    CHECK(Parses(L"ctrl-shift-p", MOD_CONTROL | MOD_SHIFT, 'P'));
    CHECK(Parses(L" Control + Alt + F9 ", MOD_CONTROL | MOD_ALT, VK_F9));
    CHECK(Parses(L"Alt+KeyQ", MOD_ALT, 'Q'));
    CHECK(Parses(L"Win+Space", MOD_WIN, VK_SPACE));
    CHECK(Parses(L"Ctrl+VK_186", MOD_CONTROL, 186));
    CHECK(Parses(L"Ctrl+F24", MOD_CONTROL, VK_F24));

    CHECK(Rejects(L"Q"));            // 不带修饰键
    CHECK(Rejects(L"Alt+"));
    CHECK(Rejects(L"Alt+Q+Ctrl"));   // 主键必须在最后
    CHECK(Rejects(L"Alt+F25"));
    CHECK(Rejects(L"Alt+F1x"));      // 以前 _wtoi 会把它当成 F1
    CHECK(Rejects(L"Alt+VK_0"));
    CHECK(Rejects(L"Alt+VK_256"));
    CHECK(Rejects(L"Alt+VK_12abc"));
    CHECK(Rejects(L""));
}

// 界面录到的任何键，FormatHotkey 写进配置后都必须能被 ParseHotkey 原样认回来。
// 以前标点、小键盘等键会写成 "VK_186" 却认不回来，下次启动热键直接失效。
void TestRoundTripAllKeys() {
    const UINT mods[] = {MOD_ALT, MOD_CONTROL | MOD_SHIFT, MOD_WIN | MOD_ALT};
    for (UINT m : mods) {
        for (UINT vk = 1; vk < 256; ++vk) {
            const HotkeySpec in{m, vk};
            HotkeySpec out;
            const std::wstring text = FormatHotkey(in);
            const bool ok = ParseHotkey(text, &out) && out.modifiers == m && out.vk == vk;
            if (!ok) {
                std::printf("FAIL round-trip vk=%u mods=%u text=%ls\n", vk, m, text.c_str());
                ++g_failures;
            }
        }
    }
}

void TestRecency() {
    CHECK(MostRecentIndex({}) == -1);
    CHECK(MostRecentIndex({{L"a", 0}, {L"b", 0}}) == -1);
    CHECK(MostRecentIndex({{L"a", 100}, {L"b", 300}, {L"c", 200}}) == 1);
    CHECK(MostRecentIndex({{L"a", 0}, {L"b", 5}}) == 1);
    CHECK(MostRecentIndex({{L"a", 7}, {L"b", 7}}) == 0);  // 平局取先出现的
    CHECK(BehindSeconds(0, 25000000) == 2.5f);

    const std::wstring d = DescribeActivity({{L"chrome", 10000000}, {L"msedge", 30000000}, {L"x", 0}});
    CHECK(d.find(L"msedge(最近)") != std::wstring::npos);
    CHECK(d.find(L"chrome(2.0s前)") != std::wstring::npos);
    CHECK(d.find(L"x(无时间线)") != std::wstring::npos);
}

}  // namespace

int main() {
    TestParseHotkey();
    TestRoundTripAllKeys();
    TestRecency();
    if (g_failures == 0) {
        std::printf("all tests passed\n");
        return 0;
    }
    std::printf("%d failure(s)\n", g_failures);
    return 1;
}
