#include "hotkey.h"

#include "log.h"

#include <cwctype>
#include <vector>

namespace {

std::wstring ToLower(std::wstring s) {
    for (wchar_t& c : s) {
        c = static_cast<wchar_t>(std::towlower(c));
    }
    return s;
}

std::wstring Trim(const std::wstring& s) {
    size_t b = s.find_first_not_of(L" \t");
    if (b == std::wstring::npos) {
        return std::wstring();
    }
    size_t e = s.find_last_not_of(L" \t");
    return s.substr(b, e - b + 1);
}

std::vector<std::wstring> Split(const std::wstring& s) {
    std::vector<std::wstring> parts;
    std::wstring cur;
    for (wchar_t c : s) {
        if (c == L'+' || c == L'-') {
            parts.push_back(Trim(cur));
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    parts.push_back(Trim(cur));
    return parts;
}

/// 修饰键名 → MOD_*，不是修饰键返回 0。
UINT ParseModifier(const std::wstring& lower) {
    if (lower == L"alt" || lower == L"menu") return MOD_ALT;
    if (lower == L"ctrl" || lower == L"control") return MOD_CONTROL;
    if (lower == L"shift") return MOD_SHIFT;
    if (lower == L"win" || lower == L"super" || lower == L"meta") return MOD_WIN;
    return 0;
}

/// 主键名 → 虚拟键码，认不出返回 0。
UINT ParseKey(const std::wstring& lower) {
    if (lower.empty()) {
        return 0;
    }
    // 配置里可能写成 "KeyQ"（沿用上一版的写法），去掉前缀照样认。
    std::wstring k = lower;
    if (k.size() > 3 && k.compare(0, 3, L"key") == 0) {
        k = k.substr(3);
    }

    if (k.size() == 1) {
        wchar_t c = k[0];
        if (c >= L'a' && c <= L'z') return static_cast<UINT>(L'A' + (c - L'a'));
        if (c >= L'0' && c <= L'9') return static_cast<UINT>(c);
    }
    // F1~F24
    if (k.size() >= 2 && k[0] == L'f') {
        wchar_t* end = nullptr;
        const long n = wcstol(k.c_str() + 1, &end, 10);
        if (end && *end == L'\0' && n >= 1 && n <= 24) {
            return static_cast<UINT>(VK_F1 + (n - 1));
        }
    }
    if (k == L"space") return VK_SPACE;
    if (k == L"enter" || k == L"return") return VK_RETURN;
    if (k == L"tab") return VK_TAB;
    if (k == L"escape" || k == L"esc") return VK_ESCAPE;
    if (k == L"insert") return VK_INSERT;
    if (k == L"delete") return VK_DELETE;
    if (k == L"home") return VK_HOME;
    if (k == L"end") return VK_END;
    if (k == L"pageup") return VK_PRIOR;
    if (k == L"pagedown") return VK_NEXT;
    if (k == L"up") return VK_UP;
    if (k == L"down") return VK_DOWN;
    if (k == L"left") return VK_LEFT;
    if (k == L"right") return VK_RIGHT;
    // "VK_186" 这种写法：KeyName 对没有友好名字的键（标点、小键盘等）就这么写回配置，
    // 这里必须能原样认回来，否则界面里录得到的热键，下次启动反而解析失败。
    if (k.size() > 3 && k.compare(0, 3, L"vk_") == 0) {
        wchar_t* end = nullptr;
        const unsigned long n = wcstoul(k.c_str() + 3, &end, 10);
        if (end && *end == L'\0' && end != k.c_str() + 3 && n > 0 && n < 256) {
            return static_cast<UINT>(n);
        }
    }
    return 0;
}

std::wstring KeyName(UINT vk) {
    if (vk >= L'A' && vk <= L'Z') return std::wstring(1, static_cast<wchar_t>(vk));
    if (vk >= L'0' && vk <= L'9') return std::wstring(1, static_cast<wchar_t>(vk));
    if (vk >= VK_F1 && vk <= VK_F24) {
        wchar_t buf[8];
        _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"F%u", vk - VK_F1 + 1);
        return buf;
    }
    switch (vk) {
        case VK_SPACE:  return L"Space";
        case VK_RETURN: return L"Enter";
        case VK_TAB:    return L"Tab";
        case VK_ESCAPE: return L"Escape";
        case VK_INSERT: return L"Insert";
        case VK_DELETE: return L"Delete";
        case VK_HOME:   return L"Home";
        case VK_END:    return L"End";
        case VK_PRIOR:  return L"PageUp";
        case VK_NEXT:   return L"PageDown";
        case VK_UP:     return L"Up";
        case VK_DOWN:   return L"Down";
        case VK_LEFT:   return L"Left";
        case VK_RIGHT:  return L"Right";
        default: break;
    }
    wchar_t buf[16];
    _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"VK_%u", vk);
    return buf;
}

}  // namespace

bool ParseHotkey(const std::wstring& text, HotkeySpec* out) {
    if (!out) {
        return false;
    }
    HotkeySpec spec{};
    const std::vector<std::wstring> parts = Split(Trim(text));
    for (size_t i = 0; i < parts.size(); ++i) {
        const std::wstring lower = ToLower(parts[i]);
        if (lower.empty()) {
            return false;
        }
        const UINT mod = ParseModifier(lower);
        if (mod != 0) {
            spec.modifiers |= mod;
            continue;
        }
        // 不是修饰键，那必须是最后一段，且能认成主键。
        if (i + 1 != parts.size()) {
            return false;
        }
        spec.vk = ParseKey(lower);
    }
    if (spec.vk == 0) {
        return false;
    }
    // 不带修饰键的热键会把那个键从整个系统里抢走，代价太大，不允许。
    if (spec.modifiers == 0) {
        return false;
    }
    *out = spec;
    return true;
}

std::wstring FormatHotkey(const HotkeySpec& spec) {
    std::wstring s;
    if (spec.modifiers & MOD_CONTROL) s += L"Ctrl+";
    if (spec.modifiers & MOD_ALT)     s += L"Alt+";
    if (spec.modifiers & MOD_SHIFT)   s += L"Shift+";
    if (spec.modifiers & MOD_WIN)     s += L"Win+";
    s += KeyName(spec.vk);
    return s;
}

HotkeyRegistration::~HotkeyRegistration() {
    Clear();
}

void HotkeyRegistration::Clear() {
    if (active_ && hwnd_) {
        UnregisterHotKey(hwnd_, id_);
    }
    active_ = false;
}

bool HotkeyRegistration::Apply(HWND hwnd, const HotkeySpec& spec) {
    if (active_ && hwnd_ == hwnd && current_.modifiers == spec.modifiers &&
        current_.vk == spec.vk) {
        return true;  // 同一个组合键已经挂着；再注册一次反而会因"已被占用"失败
    }
    // **先挂新的、成功了再摘旧的。** 反过来的话，新组合键被占用时旧的已经注销，
    // 用户手里一个能用的热键都不剩。两个 id 轮换使用，新旧可以短暂并存。
    const int newId = (active_ && id_ == kIdA) ? kIdB : kIdA;
    // MOD_NOREPEAT：按住不放只触发一次，避免长按刷屏。
    if (!RegisterHotKey(hwnd, newId, spec.modifiers | MOD_NOREPEAT, spec.vk)) {
        LogF(L"注册热键 %s 失败（错误码 %lu），多半已被其他程序占用",
             FormatHotkey(spec).c_str(), GetLastError());
        return false;  // 旧热键（如果有）保持不动
    }
    Clear();
    hwnd_ = hwnd;
    id_ = newId;
    current_ = spec;
    active_ = true;
    LogF(L"热键 %s 已注册，后台监听中", FormatHotkey(spec).c_str());
    return true;
}
