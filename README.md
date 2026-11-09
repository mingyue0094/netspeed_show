# NetSpeed Show

轻量级 Windows 实时网速显示工具。

## 功能

- 单行显示：左侧绿色下载速度，右侧蓝色上传速度。
- 自动切换 KB/s / MB/s，保留两位小数。
- 每秒更新一次。
- 托盘右键菜单选择网卡，支持自动汇总活动物理网卡。
- 支持暂停、隐藏/显示和退出。
- C++17 + Win32 API，无 Qt、Python 或 .NET 依赖。

## 自动构建

GitHub Actions 在推送到 main、提交 PR 或手动触发时构建 Windows x86_64 版本。

构建完成后，在仓库 **Actions → Build Windows x86_64 → Artifacts** 下载：

\`NetSpeed-Windows-x86_64\`

解压即可得到 \`NetSpeed-x86_64.exe\`。

## 本地编译

使用 MinGW-w64 x86_64：

\`\`\`bash
g++ main.cpp -o NetSpeed-x86_64.exe -std=c++17 -O2 -s -DNDEBUG -mwindows -municode -static -static-libgcc -static-libstdc++ -liphlpapi -lgdi32 -luser32 -lshell32
\`\`\`

## 注意

网速显示条是贴近 Windows 任务栏通知区域的轻量悬浮显示窗口，托盘图标负责右键菜单。实际位置可能受任务栏位置、Windows 版本和缩放比例影响。
