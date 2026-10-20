# ltool 插件仓库

**这份 `plugins/` 是 [ltool 主仓库](https://github.com/mateo-ki/ltool) 里 `plugins/` 的一份镜像**：
类名、源文件、`plugin.json` 全在主仓库那边改，这里只负责"能不能脱离那棵树独立编出来"。
（2026-10-20 实测：主仓库的 PCH 挂在 `ltoolcore` 的 INTERFACE 上，整棵树白拿一份预编译头，
于是三颗插件漏 `#include <QPushButton>` / `<QLabel>` / `<QThread>` 它从来不红 ——
**独立编这一侧才量得出来**。别把这里当第二份源码改，那正是 #91 那批闪退的来路之一。）

主仓库只留宿主那一层薄壳（热键、托盘、单实例、命令行自测）和一份 `ltoolcore.dll`；界面本体在两边同源。

## 一面一条

| 东西 | 是什么 |
| --- | --- |
| `plugins/<id>/` | 一个插件：页面/动作本体 + 一层 `LToolPlugin` 壳 + `plugin.json` + 自己的 `CMakeLists.txt`（**19 个**，从主仓库同步） |
| `sdk/` | 主仓库 `tools/make_sdk.py` 生成的**快照**：core 公开的 25 个头 + `ltoolcore.lib`（导入库）+ `ltool-plugin.cmake` + `INFO.txt`。主仓库改了 core 的公开面就得重生成它（接口号那份由 `tools/core_abi_gate.py` 盯着） |
| `dist/` | 编好的 19 个 dll + `index.json`。**从 ltool 1.3.0 起，插件商店不再读这里** —— 商店读的是主仓库那次发版的资产（`releases/download/v<版本>/plugins-index.json`，dll 和 core 同一次构建）。这里留着 = 独立编出来的参照物 |
| `tools/make_index.py` | 生成 `dist/index.json`（sha256/字节数从产物现算） |
| `.github/workflows/build.yml` | CI：装依赖（vcpkg classic，只要 `qtbase`）→ 验 Qt 版本 → 验 SDK（**按 API 4 编，对不上就红**）→ 验 `dist/` 自洽 → 编 19 个 dll → 出 index.json → 存 artifact（**不自动发布**） |

## 本地编一个 dll


```
# 走 vcpkg（和 CI 同一条路：classic 模式装好的那棵树）
cmake -B build -G "Visual Studio 17 2022" -A x64 \
  -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows
# 或者 Qt 在别处（官方包/自己编的）
cmake -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH=<Qt6 6.10.2 所在>

cmake --build build --config Release            # 全部 12 个
cmake --build build --config Release --target ltool-json   # 只编一个
```

产物在 `build/plugins/ltool-<id>.dll`。装法就是拷到 `ltool.exe` 旁边的 `ltool-plugins\` 里，
**下次启动生效**（插件只装不卸：Qt 插件 unload 之后里面活着的控件就是悬空指针）。

## 十二个自带插件

| id | 是什么 | 触发方式 |
| --- | --- | --- |
| repair | DLL 修复（.NET / VC++ / DirectX / UCRT） | 卡片 |
| ads | 广告弹窗拦截 | 卡片 |
| disk | 可清理位置 + 所选盘的占用分析 | 卡片 |
| network | 网卡 DNS 预设 | 卡片 |
| registry | 注册表体检 | 卡片 |
| startup | 开机启动分析 | 卡片 |
| user | Windows 用户名 | 卡片 |
| json | JSON 格式化 | 卡片，也吃 `json 这串…` 递文本 |
| base64 | Base64 编解码 | 卡片，也吃 `b64 这串…` |
| shell | 占用分析 / 谁占用着它 / 粉碎 / 解压 / 压缩 | 资源管理器右键（verb 表在主程序） |
| record | 录屏出 mp4 | F1 之外的那颗热键 F2 |
| capture | 截图浮层 + 置顶窗 + OCR 取字 | F1 |

后三个是**动作型插件**（插件接口 API 3）：入口留在主程序，按下之后主程序把动作名交给
`PluginHost::invoke`，这里面的 `actions()`/`invoke()` 接住。热键注册不在插件里。

## 加一个插件

1. 建 `plugins/<id>/`：`plugin.json`（`Id/Name/Desc/Api/Keywords/Color`，动作型再加 `Actions`）+ 源码 + `CMakeLists.txt`（照抄隔壁那份，改 id 和类名）。
2. 类名 = 首字母大写的 id + `Plugin`；接口是 `sdk/include/ltool-plugin.h`。
3. 主程序的卡片墙、搜索直达、商店**全自动**认它 —— 没有"内置工具"特判。
4. `Api` 必须等于主程序那个数，不等主程序会拒装并在界面上说清是哪一侧旧。

## 商店怎么认这些 dll

`dist/index.json`：`{"schema":1,"core":"…","plugins":[{id,name,desc,api,keywords,actions,color,file,bytes,sha256,url}]}`。

- 每 dll 的 `sha256` 和 `bytes` 由 `tools/make_index.py` **从产物现算**，不是手抄。
- 商店下载完按这个哈希校验，不对就删掉半成品；装之前还会先看 `api` 对不对。
- 装到 `ltool-plugins/<file>` 走"先 `.part` 再改名"，中途断了不会留下半个 dll 冒充装好了。
- 这份 `dist/` 直接提交在 main 上，`url` 指 raw 地址，所以下载不需要登录。

## 改了 core 之后

`sdk/` 是快照，会过期：core 的导出面一变，这边就报"链接找不到符号"或装上去加载失败。
回主仓库跑 `python tools/make_sdk.py <这份仓库>/sdk` 重新生成、提交。
CI 第一步会验 `sdk/` 在不在、API 对不对，所以忘了更新不会拖到运行期。

## 三条已知的坑（别指望它们不存在）

- **Qt 版本必须和主程序同一个数**（现在钉 6.10.2）。插件运行期加载的是宿主那份 `Qt6*.dll`，
  对不上就是加载失败；CI 里 `QT_MUST_BE` 装的依赖、`PLUGINS_VCPKG_SHA` 钉的 vcpkg commit
  和主仓库 `vcpkg.json` 的 `builtin-baseline`/`overrides.qtbase` 必须四者同源。
  官方预编译 Qt（install-qt-action）这条路 2026-10-15 实测不通：在线安装器归档里 `qt6_6102/` 是空目录，
  aqt 当场报 "The packages ['qt_base'] were not found"，所以现在改走 vcpkg —— 宿主那份 Qt 本来就是 vcpkg 编的。
- **生成器必须是多配置（Visual Studio）**：`sdk/ltool-plugin.cmake` 把 dll 落在
  `RUNTIME_OUTPUT_DIRECTORY_RELEASE`，单配置生成器（Ninja/Make）不认这个属性，产物会散进
  `build-<id>/` 而不是 `build/plugins/`。
- **本仓库的 CI 不自动发布**：只编、只出 artifact。把 `dist/` 更新回 main 或发 Release 是对外动作，由人按下。
