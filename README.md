# quietkey

一个热键暂停/播放后台的视频，**不切窗口、不抢焦点、不打断打字**。

写笔记的时候想暂停视频，不用再 Alt+Tab 过去按空格再切回来。

Windows 专用，C++17 + Win32 + C++/WinRT，无第三方依赖，单文件约 500 KB。


# 自己编译(纯手工编译)

## 环境

- **Visual Studio 生成工具 2022**，安装时勾选「使用 C++ 的桌面开发」
  （自带 MSVC、Windows SDK、CMake、Ninja，不用另外装），其它什么都不需要——没有第三方库，没有包管理器。

# C++ 和 Rust 手工编译区别？

**根本区别：Rust 有 cargo，C++ 什么都没有。**

| 编译型语言 | 包管理器 | 便捷度 |
| --- | --- | --- |
| **Rust** | `cargo build` | ← 一条命令，全包了 |
| **C++** | ??? | 没有官方的「这条命令」 |

- `cargo` 同时是**包管理器 + 构建系统 + 编译器驱动**。
- C++ 里这 3 样东西官方一个都不提供，你只有一个编译器 `cl.exe`，它只会做一件很窄的事：把一个 `.cpp` 变成一个 `.obj`。剩下的全靠你自己组织。

---

# Rust 优势

对照一下 `cargo build` 背地里替你做的事：

| cargo 自动做的 | C++ 里谁来做 |
| --- | --- |
| **知道要编哪些源文件** | 你自己列（`src\*.cpp`） |
| **知道链接哪些系统库** | 你自己写（`user32.lib shell32.lib...`） |
| **传编译参数（edition、优化等级）** | 你自己敲（`/std:c++17 /O2 /utf-8...`） |
| **增量编译、依赖跟踪** | 没有，除非你上 CMake/Ninja |
| **处理资源文件（图标等）** | 你自己调 `rc.exe` |

- 所以 C++ 圈才有 CMake、Ninja、vcpkg 这些工具——它们都是在补 cargo 自带的那些能力。
- 如果有 `build.bat` 之类的文件，就是我替你把这些步骤串起来的脚本。

---

# C++ 编译概念

C++ 编译是**三段**流水线，Windows 上多一条支线：

```
源码.cpp ─[编译器 cl.exe]─► 目标文件.obj ─┐
                                          ├─[链接器 link.exe]─► quietkey.exe
资源.rc  ─[资源编译器 rc]─► 资源文件.res ─┤
                                          │
系统库 user32.lib / windowsapp.lib ...  ─┘
```

## 1. `.obj`（目标文件）

一个 `.cpp` 编出一个 `.obj`。里面是机器码，但函数地址还没确定，调用别处的函数只留了个「待填」的记号。

## 2. 链接器

把所有 `.obj` 拼一起，把那些「待填」的记号一个个对上号。对不上就报 `LNK2019 无法解析的外部符号`。

## 3. `.lib`（导入库）

不是代码，是一张索引表，告诉链接器「`SendInput` 这个函数在 `user32.dll` 里」。所以调用系统 API 必须链接对应的 `.lib`。

- Rust 里这三步 cargo 全包了，你从来不用知道 `.o` 文件存在。
- C++ 里你得亲手指挥。

---

# 手工编译 C++ 源码

打开一个 **cmd** 窗口（不是 PowerShell，原因最后说）跟着敲就行。

`Win + R` 输入 `cmd`。

## 1. 准备工作（了解基础）

先准备一个空目录当输出目录（所有中间产物都会落这儿，不脏项目）：

```bat
mkdir D:\qk-build
```

这是很多新人最懵的一步。这一步叫**进入编译环境**，现在这个 CMD 已经变成 Visual Studio 专用 CMD：

```bat
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
```

这一步在干什么：往当前 cmd 窗口塞三个环境变量。

| 变量 | 内容 | 没有它会怎么样 |
| --- | --- | --- |
| `PATH` | 加上 `cl.exe` / `link.exe` / `rc.exe` 所在目录 | `'cl' 不是内部或外部命令` |
| `INCLUDE` | 头文件目录（CRT、Windows SDK、cppwinrt） | 找不到 `windows.h` |
| `LIB` | 库文件目录 | 找不到 `user32.lib` |

- 只对**当前这个窗口**有效，关掉就没了。所以每次新开窗口都要先 `call` 一次。
- `INCLUDE` 里包含 SDK 的 cppwinrt 目录——这就是 `#include <winrt/Windows.Media.Control.h>` 能找到的原因。C++/WinRT 是 Windows SDK 自带的，不用装任何第三方东西。

验证一下 `cl` 编译器（类似查看版本）：

```bat
where cl
```

进入刚才新建的目录：

```bat
cd /d D:\qk-build
```

`/d` 这里相当于 Linux 系统终端里的 `-d` 选项。  
`/d` 代表 Drive / 驱动器，告诉 CMD：「不仅要切换到这个文件夹，同时把当前驱动器（盘符）也一起切过去。」

## 4. 阶段 1：先只编一个文件，理解 `cl` 在干什么

```bat
cl /nologo /c /std:c++17 /EHsc /utf-8 /DUNICODE /D_UNICODE /DNOMINMAX D:\len\src\recency.cpp
```

这里好多选项，最终是为了编译 `D:\len\src\recency.cpp` 文件。

- `/c` 选项：「只编译，不链接」。这就是流水线的第一段：一个 `.cpp` → 一个 `.obj`。
- 你可以对每个源文件都这么干，最后再一起链接。
- 你会看到，目录里多出：`recency.obj`

## 5. 阶段 2：编全部 + 链接，得到能跑的 exe

```bat
cl /nologo /std:c++17 /EHsc /utf-8 /DUNICODE /D_UNICODE /DNOMINMAX ^
D:\len\src\*.cpp ^
/Fe:mini.exe ^
/link /SUBSYSTEM:WINDOWS ^
user32.lib shell32.lib advapi32.lib ole32.lib comctl32.lib dwmapi.lib windowsapp.lib
```

- `^` 是 cmd 的续行符（相当于 Linux 的 `\`）。嫌麻烦就写成一行。
- 文件名依次滚过，然后「正在生成代码...」，目录里出现 `mini.exe`。

这条命令的结构要看懂：

```
cl [编译器参数] [源文件] /Fe:输出名 /link [链接器参数] [要链接的库]
                                              ↑
                                这里开始，后面的全部转交给 link.exe
```

`cl` 没有 `/c` 时会自动帮你调用 `link.exe`，所以看起来像一步，其实是两步。

验证它（`mini.exe`）能跑：

```bat
mini.exe --list-sessions
```

会打印当前的媒体会话。到这里你已经手工编译出一个可用的 quietkey 了——功能完整，只是没图标、没界面、控件是 Win95 灰扁样式、高分屏下发糊。

## 6. 阶段 3：加上资源（图标 + manifest + 对话框）

这是 Windows 特有的支线。先看 `src\app.rc` 里打包了什么：

- 图标
- manifest（视觉样式 / DPI 感知 / asInvoker）
- 版本信息（右键属性里那些）
- 两个对话框模板（设置界面和窗口拾取器的布局就在里面）

编译出图标生成器：

```bat
cl /nologo /EHsc /std:c++17 /Fe:make_icon.exe D:\len\tools\make_icon.cpp
```

用图标生成器生成 `.ico`：

```bat
.\make_icon.exe quietkey.ico
```

- `.\` 不能省略。我第一次就栽在这儿：直接敲 `make_icon.exe` 报「不是内部或外部命令」，而文件明明就在当前目录——这台机器的 cmd 不从当前目录找可执行文件。

编译资源：

```bat
rc /nologo /I. /ID:\len\src /fo app.res D:\len\src\app.rc
```

- `/I.` → 让 rc 在当前目录找到刚生成的 `quietkey.ico`
- `/ID:\len\src` → 找到 `app.manifest`
- `/fo app.res` → 输出文件名

重新编译，把 `app.res` 资源文件一起塞进去：

```bat
cl /nologo /std:c++17 /EHsc /utf-8 /DUNICODE /D_UNICODE /DNOMINMAX ^
D:\len\src\*.cpp app.res ^
/Fe:quietkey.exe ^
/link /SUBSYSTEM:WINDOWS /MANIFEST:NO ^
user32.lib shell32.lib advapi32.lib ole32.lib comctl32.lib shlwapi.lib dwmapi.lib windowsapp.lib
```

- 和阶段 2 只有两处不同：源文件列表后面多了 `app.res`，链接参数多了 `/MANIFEST:NO`（禁止生成清单）
- 现在 `quietkey.exe` 有图标了，界面也是正常的系统控件。

## 7. 阶段 4：加上优化（Release）

阶段 2/3 编的其实是「无优化」版本。加两个参数：

```bat
cl /nologo /std:c++17 /EHsc /W4 /O2 /DNDEBUG /utf-8 ^
/DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
D:\len\src\*.cpp app.res ^
/Fe:quietkey.exe ^
/link /SUBSYSTEM:WINDOWS /MANIFEST:NO ^
user32.lib shell32.lib advapi32.lib ole32.lib comctl32.lib shlwapi.lib dwmapi.lib windowsapp.lib
```

- `/O2` 优化速度
- `/DNDEBUG` 关掉 assert

产出：`D:\qk-build\quietkey.exe`（带图标、界面完整）

写成一行：

```bat
cl /nologo /std:c++17 /EHsc /W4 /O2 /DNDEBUG /utf-8 /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX D:\len\src\*.cpp app.res /Fe:quietkey.exe /link /SUBSYSTEM:WINDOWS /MANIFEST:NO user32.lib shell32.lib advapi32.lib ole32.lib comctl32.lib shlwapi.lib dwmapi.lib windowsapp.lib
```

想编 Debug 版（能单步调试）则相反：

```bat
cl /nologo /std:c++17 /EHsc /W4 /Od /Zi /utf-8 ^
/DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
D:\len\src\*.cpp app.res ^
/Fe:quietkey.exe ^
/Fd:quietkey.pdb ^
/link /SUBSYSTEM:WINDOWS /MANIFEST:NO /DEBUG ^
user32.lib shell32.lib advapi32.lib ole32.lib comctl32.lib shlwapi.lib dwmapi.lib windowsapp.lib
```

- `/Od`：关闭优化，方便单步
- `/Zi`：生成调试信息
- `/Fd:quietkey.pdb`：指定 PDB 文件名
- `/DEBUG`：链接器生成调试用信息

产出：`quietkey.exe` + `quietkey.pdb`（可用 VS 附加进程或直接调试）

写成一行：

```bat
cl /nologo /std:c++17 /EHsc /W4 /Od /Zi /utf-8 /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX D:\len\src\*.cpp app.res /Fe:quietkey.exe /Fd:quietkey.pdb /link /SUBSYSTEM:WINDOWS /MANIFEST:NO /DEBUG user32.lib shell32.lib advapi32.lib ole32.lib comctl32.lib shlwapi.lib dwmapi.lib windowsapp.lib
```

## 8. 命令参数分两类记

### 1）不加就编不过的

| 参数 | 不加的后果 |
| --- | --- |
| `/std:c++17` | C++/WinRT 编不过（它要 `if constexpr` 等 17 特性） |
| `/EHsc` | C++/WinRT 用异常报错，关异常直接编不过 |
| `/DNOMINMAX` | `windows.h` 的 `min`/`max` 宏和 `std::max` 打架 |
| `user32.lib` 等 8 个库 | `LNK2019` 一堆未解析符号 |

### 2）不加能编过但结果不对的

| 参数 | 不加的后果 |
| --- | --- |
| `/utf-8` | 源码里的中文全变乱码，界面全是问号 |
| `/DUNICODE` `/D_UNICODE` | 宽字符 API 退回 ANSI 版，中文路径/标题出问题 |
| `/SUBSYSTEM:WINDOWS` | 双击会一直挂个黑框；而且入口点会去找 `main` 而不是 `wWinMain`，链接失败 |
| `/MANIFEST:NO` | 和 `.rc` 里的 manifest 打架 |

### 3）那 8 个库分别为什么要

| 库 | 项目里用到的 |
| --- | --- |
| `user32` | 窗口、消息、菜单、`SendInput`、`EnumWindows` |
| `shell32` | `Shell_NotifyIcon`（托盘）、`SHGetKnownFolderPath`（获取已知文件夹路径） |
| `advapi32` | 注册表（开机自启） |
| `ole32` | `CoTaskMemFree`（协同任务可用内存） |
| `comctl32` | `InitCommonControlsEx`（热键控件） |
| `dwmapi` | `DwmGetWindowAttribute`（过滤隐形窗口） |
| `windowsapp` | C++/WinRT 运行时，媒体会话那层 |
| `shlwapi` | 历史遗留，其实可以去掉 |

- 第二个坑：前四个是 CMake 默默替你加的。
- 手工调 `cl` 编译不写就会报 50 个 `LNK2019` 报错，我第二次就栽在这儿。
- Rust 里 `windows` crate 会自己声明要链接哪些库，所以你从来没遇到过这问题。

### 4）和 cargo 逐条对照

| 你熟悉的 Rust 包管理器 | C++ 手工等价物 |
| --- | --- |
| `cargo build` | 阶段 0~3 那四条命令 |
| `cargo build --release` | 同上 + `/O2` `/DNDEBUG` |
| `cargo run` | 编完自己敲 `quietkey.exe` |
| `cargo clean` | 删掉输出目录 |
| `Cargo.toml` 的 `[dependencies]` | 不存在——系统库靠手写 `.lib`，第三方库靠 vcpkg 或手动下载 |
| `Cargo.toml` 的 `edition = "2021"` | `/std:c++17` |
| `build.rs` | `make_icon.cpp` + 手动跑它 |
| `windows` crate 的 features | `INCLUDE` 里有什么头文件就能用什么 |
| 增量编译 | 没有，除非上 CMake/Ninja |

### 5）常见报错怎么读

| 报错 | 原因 |
| --- | --- |
| `'cl' 不是内部或外部命令` | 忘了 `call vcvars64.bat`，或者你在 PowerShell 里跑 |
| `LNK2019 无法解析的外部符号 __imp_XXX` | 缺 `.lib`。看符号名猜是哪个库：`SendInput`→`user32`、`RegSetValueEx`→`advapi32` |
| `RC2135 file not found: quietkey.ico` | 没先跑 `make_icon.exe`，或者 `rc` 的 `/I` 路径不对 |
| `LNK1181 无法打开输入文件 "app.res"` | 上一步 `rc` 失败了，`.res` 根本没生成 |
| `fatal error C1083: 找不到 windows.h` | `INCLUDE` 没设好，还是 vcvars 的问题 |
| 界面中文全是问号 | 忘了 `/utf-8` |

### 6）清理

其实不用下面这条命令这么麻烦，你最终要的是最终能用的 `.exe`，直接复制走其它删掉就行。

```bat
del /q D:\qk-build\*.obj D:\qk-build\*.res
```

### 7）为什么要用 cmd 而不是 PowerShell

`vcvars64.bat` 是批处理，它靠 `set` 修改当前进程的环境变量。在 PowerShell 里 `call xxx.bat` 会开一个子 cmd 进程，那些变量随子进程一起消失，回到 PowerShell 就没了。

PowerShell 里要用得绕一圈（把 bat 的环境变量导出来再逐个 `$env:` 设回去），没必要。手工编译就老老实实开 cmd。

### 8）意外发现：手工版反而更「单文件」

查 DLL 依赖时发现的，这条值得单独说：

| | 依赖 VC 运行时？ | 体积 |
| --- | --- | --- |
| 手工编译（默认 `/MT` 静态 CRT） | 不依赖 | 642 KB |
| build.bat（CMake 默认 `/MD` 动态 CRT） | 依赖 `MSVCP140.dll`、`VCRUNTIME140.dll`、`VCRUNTIME140_1.dll` | 511 KB |

131 KB 的差距就是这么来的——CMake 版把 C++ 运行时甩给了系统 DLL。

「无依赖、单文件」的说法，对 CMake 版是打折扣的：拷到一台没装过 VC++ 可再发行组件的干净 Windows 上，它会报「找不到 VCRUNTIME140.dll」。你现在这台机器装过，所以没暴露出来。

手工版那 642 KB 才是真正拷到任何 Win10/11 上双击就能跑的。

> **现状：** `CMakeLists.txt` 已经加上了下面这行，`build.bat` 产出的 exe 同样不依赖 VC 运行时
> （Debug 配置用调试版 `/MTd`），上表的 `/MD` 那一行只是当时的记录。
>
> ```cmake
> set_property(TARGET quietkey PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
> ```

---

# 建议操作顺序（最终直接快速复制用）

```bat
REM === 0. 环境 ===
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
mkdir D:\qk-build 2>nul
cd /d D:\qk-build

REM === 1. 图标 + 资源 ===
cl /nologo /EHsc /std:c++17 /O2 /Fe:make_icon.exe D:\len\tools\make_icon.cpp
.\make_icon.exe quietkey.ico
rc /nologo /I. /ID:\len\src /fo app.res D:\len\src\app.rc

REM === 2a. Release ===
cl /nologo /std:c++17 /EHsc /W4 /O2 /DNDEBUG /utf-8 /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX D:\len\src\*.cpp app.res /Fe:quietkey.exe /link /SUBSYSTEM:WINDOWS /MANIFEST:NO user32.lib shell32.lib advapi32.lib ole32.lib comctl32.lib shlwapi.lib dwmapi.lib windowsapp.lib

REM === 2b. 若要 Debug，用下面这条代替 2a（不要两条一起跑覆盖）===
cl /nologo /std:c++17 /EHsc /W4 /Od /Zi /utf-8 /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX D:\len\src\*.cpp app.res /Fe:quietkey.exe /Fd:quietkey.pdb /link /SUBSYSTEM:WINDOWS /MANIFEST:NO /DEBUG user32.lib shell32.lib advapi32.lib ole32.lib comctl32.lib shlwapi.lib dwmapi.lib windowsapp.lib
```

- 路径 `D:\len\...` 若和你本机不一致，改成你的源码实际路径即可。

---

# （可略）关于其它 C++ 项目的编译

## 一、核心答案：原理不变，配方随项目变

编译原理是死的：

```
源码 → .obj → 链接 → exe
```

从 hello world 到 Chromium，永远是这三步，没有例外。

但「配方」随项目规模指数级膨胀。你手工编译我们这个项目很顺利，不是因为你运气好，而是因为它有一个极其关键的属性：

> **零第三方依赖。**

这才是真正的分水岭。

## 二、vmangos 那种项目能手工编译吗

理论上能——因为 CMake 生成的也不过是同样的 `cl`/`link` 命令。实际上不可行，具体卡在六个地方：

### 1. 命令行长度有硬上限

Windows 命令行上限 32767 字符。

- 我们项目：12 个源文件，完整命令约 450 字符 ✅
- 中大型项目：几千个源文件，光路径就 10 万字符以上 ❌

所以工具链才发明了 response file（`cl @args.rsp`，把参数写进文件）。你手工也能这么干，但这已经是在自己造构建系统了。

### 2. 第三方依赖 —— 真正的门槛

我们项目要链接的都是 Windows 自带的系统库，`vcvars64.bat` 一跑，`LIB` 环境变量里就都有了，你只要写个名字。

而这类服务端项目典型需要 MySQL 客户端库、Boost、以及仓库内自带的若干第三方库。每一个都意味着：

- 先把它自己编出来（它自己又有一套构建流程，可能又依赖别的东西）
- 然后告诉编译器：头文件在哪（`/I`）、库文件在哪（`/LIBPATH`）、链接哪个 `.lib`
- 还要保证架构和 CRT 一致——比如你刚设的 `/MT`，如果依赖库是 `/MD` 编的，链接时会报一堆诡异的重复符号错误

**C++ 的难点从来不是编译器，是依赖管理。** 这也正是 cargo 最大的价值所在。

### 3. 产物不止一个 exe

大项目的典型结构是：先编出十几个静态库，再把它们链成多个可执行文件。

```
源码 → libshared.lib ┐
源码 → libgame.lib   ├→ mangosd.exe
源码 → libnet.lib    ┘
                     └→ realmd.exe
```

手工要自己维护这张拓扑图，还要保证顺序对。

### 4. 配置分支爆炸

平台（Win/Linux） × 编译器（MSVC/GCC/Clang） × 类型（Debug/Release） × 一堆功能开关

手工意味着为每个组合各写一套命令。CMake 存在的意义之一就是一份描述，生成任意组合。

### 5. 构建期代码生成

大项目常有「先跑个工具生成 `.cpp`/`.h`，再编译」的步骤（注入 git hash、生成协议代码等）。

我们项目里其实有一个迷你版：`make_icon.cpp` → `quietkey.ico` → 才能编 `.rc`。你手工时是按顺序敲的，规模一大，这种顺序有几十条，靠人记必然出错。

### 6. 增量编译

你改了一个头文件，哪些 `.cpp` 需要重编？

手工：无解，只能全量重编。我们项目全量 12 个文件几秒钟；几千个文件的项目全量一次几十分钟起步，改一行等半小时，没法工作。

## 三、CMake 到底是什么（用你自己的项目实证）

CMake 不是编译器，是「生成器」——它读 `CMakeLists.txt`，吐出一份给 Ninja 执行的清单。我把你项目里生成的那份 `build.ninja` 扒开给你看：

它生成的编译参数，和你手工敲的一模一样：

```
FLAGS = /DWIN32 /D_WINDOWS /EHsc /O2 /Ob2 /DNDEBUG -std:c++17 -MT /utf-8 /W4
```

它偷偷加的基础库（就是你手工时漏掉、报 50 个 `LNK2019` 的那批）：

```
LINK_LIBRARIES = kernel32.lib user32.lib gdi32.lib winspool.lib shell32.lib
ole32.lib oleaut32.lib uuid.lib comdlg32.lib advapi32.lib
```

它把你脑子里的顺序，写成了一张依赖图：

```
build quietkey_icon: phony  quietkey.ico make_icon.exe
build cmake_object_order_depends_target_quietkey: phony || quietkey_icon
build CMakeFiles\quietkey.dir\src\main.cpp.obj: CXX_COMPILER ... || cmake_object_order_depends_target_quietkey
```

翻译成人话就是你手工敲的那三步：先编 `make_icon` → 生成 `.ico` → 才能编源文件。

> **一句话总结：CMake 没有做任何你做不到的事，它只是把你手工做的事写成了一份机器可执行、可增量、可跨平台的清单。**

## 四、按规模分级，现实中怎么做

| 规模 | 例子 | 现实做法 |
| --- | --- | --- |
| 单文件 | `hello.cpp` | 一条 `cl` 就够 |
| 小 · 无第三方依赖 | 我们这个项目 | 手工 4 条命令 ✅ / 一个 bat ✅ / CMake ✅ 都行 |
| 中 · 有第三方依赖 | 大多数开源工具 | CMake + vcpkg（或手动装依赖）。手工理论可行，但没人这么干 |
| 大 | vmangos 这类 | CMake 是事实上的必需。手工等于自己重写一遍构建系统 |
| 超大 | Chromium / LLVM | 连 CMake 都不够，自研构建系统（GN + Ninja）+ 分布式编译 + 编译缓存 |

我们项目恰好卡在「手工还很舒服」的那一档，所以你能完整走一遍——这运气不错，它是个很好的教学样本。

## 五、直接回答「流程是跟着项目变的吗」

是，而且这正是 C++ 最反直觉的地方：

| | 构建流程属于谁 |
| --- | --- |
| **Rust** | 属于语言。`cargo build` 对所有项目都一样，从 hello world 到 Servo |
| **C++** | 属于项目。每个项目自己定义怎么编，没有通用答案 |

所以拿到一个陌生的 C++ 项目，第一件事永远是读它的 `README` / `BUILDING.md`——不是因为你不会编译，而是因为只有作者知道这个项目的配方。

这也解释了你的直观感受：不是 C++ 编译难，是 C++ 没有统一的配方格式。

## 六、为什么 Rust 舒服（本质原因）

不是 Rust 编译器更聪明，而是官方强制统一了配方：

| | C++ | Rust |
| --- | --- | --- |
| 包管理 | 无官方（vcpkg/Conan 是第三方，且不统一） | crates.io，唯一 |
| 构建描述 | 无官方（CMake 是事实标准，但还有 Meson/Bazel/裸 Makefile） | `Cargo.toml`，唯一 |
| 构建命令 | 无官方 | `cargo build`，唯一 |
| 依赖传递 | 手工处理 | 自动 |

C++ 诞生于 1985 年，比「包管理器」这个概念本身还早。等到大家意识到需要它时，几十亿行存量代码已经各自为政了。

C++ 也在追赶：vcpkg/Conan 补包管理，CMake 成为事实标准，C++20 modules 想干掉头文件。但历史包袱不是一代人能清完的。

## 七、给你的实用套路

以后遇到陌生 C++ 项目：

1. 先读 `README` / `BUILDING.md` —— 没有例外
2. 看根目录有没有 `CMakeLists.txt` —— 有的话，九成能用这三板斧：

   ```bat
   cmake -S . -B build
   cmake --build build --config Release
   ```

3. 依赖怎么办 —— 项目文档会说。Windows 上现在多数推荐 vcpkg
4. 报错先分类：
   - 找不到 `xxx.h` → 依赖没装，或 `/I` 路径没给对
   - `LNK2019` 未解析的外部符号 → 少链接了某个 `.lib`
   - 重复定义的符号 → CRT 不一致（`/MT` vs `/MD`），或同一个库链了两遍

你现在手工走过一遍完整流程，这[四类报错](#5常见报错怎么读)你都亲眼见过了（`RC2135`、`LNK2019`、`LNK1181`、`cl` 不是命令）——这比会用 CMake 有价值得多，因为 CMake 报错时，你知道它底下在干什么。






## 项目结构

```
quietkey/
├── CMakeLists.txt         工程定义（C++17、/utf-8、/W4）
├── build.bat              一键构建（纯 ASCII，见 CLAUDE.md 里的原因）
├── README.md              本文件
├── CLAUDE.md              设计依据与踩过的坑，改代码前该看的文件
├── tools/
│   └── make_icon.cpp      构建期程序化生成多尺寸 .ico，不在仓库放二进制资源
├── tests/
│   └── test_main.cpp      纯函数单元测试（热键互转、最近活跃），build\quietkey_tests.exe
└── src/
    ├── main.cpp           入口、隐藏窗口、消息循环、单实例、命令行诊断
    ├── app.rc             图标、manifest、版本信息、两个对话框模板
    ├── app.manifest       视觉样式 + PerMonitorV2 + asInvoker
    ├── resource.h         资源与控件 ID
    ├── settings.h/.cpp    设置对话框 + 窗口拾取器
    ├── config.h/.cpp      INI 读写
    ├── hotkey.h/.cpp      热键注册 + "Alt+Q" 字符串解析
    ├── tray.h/.cpp        托盘图标与右键菜单
    ├── log.h/.cpp         日志与控制台挂载
    ├── winutil.h/.cpp     窗口枚举、目标解析、抢前台、SendInput
    ├── strategy.h/.cpp    策略链调度器
    ├── gsmtc.cpp          第 1 层：媒体会话（C++/WinRT）
    ├── recency.h/.cpp     「最近被播放/暂停过的是哪个会话」
    ├── messages.cpp       第 2 层 WM_APPCOMMAND + 第 3 层 PostMessage
    └── fallback.cpp       第 4 层 抢焦点 + SendInput
```

## 资源占用

| | 数值 |
|---|---|
| exe 体积 | ~500 KB |
| 常驻内存（仅托盘） | ~7 MB |
| 打开设置界面后 | ~38 MB |
| 线程数 | 4（常驻） |
| 空闲 CPU | 0% |
