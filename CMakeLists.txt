cmake_minimum_required(VERSION 3.20)
project(quietkey LANGUAGES CXX)
enable_language(RC)

# C++17：媒体会话那一层要用 C++/WinRT，它官方要求 17。
# 其余代码仍然写得保守，不为用新特性而用新特性。
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# ---------------------------------------------------------------------------
# 图标：像素在 tools/make_icon.cpp 里程序化生成，构建期跑一次产出 .ico，
# 再由 app.rc 编进 exe 的资源段。好处是图案只有一处定义，
# 不用在仓库里维护二进制资源文件，改图案只改那一个 .cpp。
# ---------------------------------------------------------------------------
add_executable(make_icon tools/make_icon.cpp)
# 源码里有中文注释，不加 /utf-8 的话中文系统（代码页 936）会把 UTF-8 字节当 GBK 读，
# 注释吞掉换行，整个文件编译不过。
target_compile_options(make_icon PRIVATE /utf-8)
set(QUIETKEY_ICON "${CMAKE_CURRENT_BINARY_DIR}/quietkey.ico")
add_custom_command(
    OUTPUT "${QUIETKEY_ICON}"
    COMMAND make_icon "${QUIETKEY_ICON}"
    DEPENDS make_icon
    COMMENT "生成 quietkey.ico"
    VERBATIM)
add_custom_target(quietkey_icon DEPENDS "${QUIETKEY_ICON}")

# rc.exe 要能同时找到生成出来的 .ico 和 src 下的 manifest。
set(CMAKE_RC_FLAGS "${CMAKE_RC_FLAGS} /I \"${CMAKE_CURRENT_BINARY_DIR}\" /I \"${CMAKE_CURRENT_SOURCE_DIR}/src\"")

add_executable(quietkey WIN32
    src/main.cpp
    src/log.cpp
    src/hotkey.cpp
    src/tray.cpp
    src/config.cpp
    src/winutil.cpp
    src/recency.cpp
    src/strategy.cpp
    src/gsmtc.cpp
    src/messages.cpp
    src/fallback.cpp
    src/settings.cpp
    src/app.rc)

add_dependencies(quietkey quietkey_icon)

target_compile_definitions(quietkey PRIVATE
    UNICODE _UNICODE          # 全程走宽字符 API
    WIN32_LEAN_AND_MEAN
    NOMINMAX)

target_compile_options(quietkey PRIVATE
    /utf-8                    # 源码里的中文字面量必须靠它，否则编译出乱码
    /W4)

# manifest 由 app.rc 显式嵌入（要视觉样式 + DPI 感知 + asInvoker），
# 关掉链接器自动生成的那一份，否则两份会打架。
set_target_properties(quietkey PROPERTIES LINK_FLAGS "/MANIFEST:NO")

# WindowsApp.lib 是 C++/WinRT（媒体会话那一层）要的。
target_link_libraries(quietkey PRIVATE comctl32 shlwapi dwmapi windowsapp)

# 静态链接 C++ 运行时：拷到没装 VC++ 可再发行组件的机器上也能直接跑，代价是体积大 100 多 KB。
# Debug 配置用调试版 CRT（/MTd），否则 _DEBUG 断言、堆检查全部失效。
set_property(TARGET quietkey PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# ---------------------------------------------------------------------------
# 单元测试：只覆盖纯函数（热键字符串互转、最近活跃判定），不碰系统状态。
# 跑法：build\quietkey_tests.exe，或 ctest --test-dir build
# ---------------------------------------------------------------------------
enable_testing()
add_executable(quietkey_tests
    tests/test_main.cpp
    src/hotkey.cpp
    src/recency.cpp
    src/log.cpp)
target_include_directories(quietkey_tests PRIVATE src)
target_compile_definitions(quietkey_tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(quietkey_tests PRIVATE /utf-8 /W4)
set_property(TARGET quietkey_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_test(NAME quietkey_tests COMMAND quietkey_tests)
