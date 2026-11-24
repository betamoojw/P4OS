# main 合入 dev 的验证记录

日期：2026-10-23。合入源为 `main` 的 `4608216`，合入前 `dev` 为 `56bc30c`。

后续 COM24 烧录与实机验证见 [DEVICE-MERGE-VALIDATION.md](DEVICE-MERGE-VALIDATION.md)。下文保留合并时的构建验证范围。

## 冲突处理

10 个冲突文件已按功能合并：更新记录保留双方条目；六个大字号字体保留上游 RLE 压缩和线程锁，同时保留中文 fallback；英语、德语系统目录保留双方新增键；内嵌语言 C 文件由合并后的目录重新生成。

保留 `dev` 的四种板型支持、ABI 101、简繁中文与板载 Flash 字库。引入上游管道拼图、相机优化、录屏、照片/动态壁纸、主题色和系统音效。原生应用目录仍压缩内嵌，SD 卡可按键覆盖。为新游戏及新增设置项补充简繁中文，现在每种中文有 76 份目录，其中 50 份为原生应用目录。

现有构建目录的 `sdkconfig` 曾覆盖默认值，导致压缩字体解码未开启。已更新本机八种配置，并增加编译期检查和升级说明；配置关闭时不能生成可能出现乱码的固件。动态应用也需按新 LVGL 配置重建。

录屏按钮采用独立翻译上下文，避免与固件烧录共用 `Grabar` 后显示成“写入”。传感器状态也按触发状态本地化。

## 验证

自动化回归共 28 项通过：7 项语言、字库、网页资源及 RISC-V UTF-8 检查，5 项动态 ABI 检查器测试，16 项浏览器语言与上传行为测试。简体和繁体目录完整性检查也通过。

日志：`build/merge-main-language-tests.log`、`build/merge-main-abi-tests.log`、`build/merge-main-web-tests.log`。

八种最终固件全部构建成功，单位为字节（`p4os.bin`）：

| 板型 | rev1_3 | rev3_x |
| --- | ---: | ---: |
| LCD-5 | 7,043,280 | 7,046,720 |
| LCD-7 | 7,054,032 | 7,057,488 |
| LCD-8 | 7,049,008 | 7,052,592 |
| LCD-10.1 | 7,049,632 | 7,053,072 |

八种配置均启用压缩字体解码，固件通过分区大小检查；所有打包的 `cjk.bin` 与源码字库的 SHA-256 一致。构建未启用设备自测模式。

全部 51 个动态应用和 11 个模拟器模块分别按两种硅配置重新构建，并通过四种板型的 ABI 101 静态兼容性检查，失败数为 0。检查包括导入符号、LVGL 版本/配置和指令集选项。

固件报告：`build/merge-main-matrix.json`，最终构建日志：`build/merge-main-matrix-final.log`。应用日志：`build/merge-main-apps-build.log` 和 `build/merge-main-apps-check.log`；每种硅配置的详细报告和应用包位于 `build/apps-abi101/<profile>/`。这些构建产物保留在本机，不纳入 Git。

本次验证针对合并后的源码、构建和 ABI 兼容性；没有将合并版本重新烧录到开发板，也没有执行录屏等新增功能的实机测试。之前 COM24 的中文验证见 [CHINESE-VALIDATION.md](CHINESE-VALIDATION.md)，其中的板端结果对应合并前版本。

## 2026-11-08 再次合入 main

合入源为远程 `main` 的 `521d2c2`，合入前 `dev` 为 `a0c8ac8`，引入 78 个上游提交。本地 `main` 已快进至同一版本。

- 解决 11 个冲突文件：CAN 网页保留按页面隔离的翻译接口及简繁区域格式；USB 保留新增 CMSIS-DAP 探针和主机借用逻辑，同时使用当前板型的针脚提示；射频页面保留 OTG 与电脑共用线路的说明，移除 LCD-5 专用针脚编号。
- 英语、德语目录按键合并，保留双方翻译，再按合并后的源码更新。每种中文补齐 539 个新增条目，审校烧录器、SWD、串口桥接、U-Boot、CAN/OBD 和网页文案，并将审校结果保存到简繁覆盖表。保留旧条目的翻译，删除不再使用的键及一条过期覆盖项，重新生成内嵌语言资源。
- 烧录器接线页的地线改用板型映射：LCD-5 为 10 号针脚，LCD-X 为 32 号，避免在 LCD-X 上提示错误接线。
- 构建文件检查恢复了桥接、CAN OBD 和 2043 像素源文件引用，并移除了多余的括号；三个文件的最终内容与应有的上游/原有版本一致。
- 旧构建缓存需同步新增默认配置：`CONFIG_TINYUSB_VENDOR_COUNT=1`，`CONFIG_TINYUSB_VENDOR_RX_BUFSIZE=0`，`CONFIG_TINYUSB_VENDOR_TX_BUFSIZE=0`，用于 CMSIS-DAP 的 USB vendor 接口。本次验证目录已更新，压缩字体和三个 SNTP 服务器设置保持开启。

验证结果：

- 25 项 Python 回归检查与 22 项网页测试通过；中文相关检查在最后一次文案更新后重新通过。简繁目录检查、字形覆盖、压缩资源校验、CAN 网页语法检查通过。
- ESP-IDF 5.5.5 完整编译 `lcd10_1/rev3_x` 成功。应用为 7,352,480 字节，分区剩余约 12%；镜像校验有效，芯片范围为 P4 v3.0–v3.99。仅剩既有的 LVGL/stb_image 未使用函数警告和 `screen_off_cb` 未使用警告。
- CAN、射频、2043 三个动态应用分别重编译成功，并针对最终固件 ELF 通过 ABI 101、导入符号、LVGL 配置及指令集检查。未重新构建其余应用或其他七种固件组合。
- 未烧录设备，未进行本次合并版本的实机验证；Flash 字库和分区布局未变。

验证镜像在合并提交前构建，版本为 `v0.15.0-140-ga0c8ac8-dirty`；应用 SHA-256 为 `2f9659c6309abfe97034a4f2ffd925bf54bc553aa9e13d41b192c61f935e9d4c`，ELF SHA-256 为 `9ee2e583e2ac4af5179f20b0afc12466b5bf07fb21dc69ae0f9aebe572156d96`。提交源码不会自动更新镜像的版本标识。

本地记录：`build/merge-main-20261108-firmware.log`、`build/merge-main-20261108-python-tests.log`（含首次检查后已修复的两项语言问题）、`build/merge-main-20261108-chinese-final-tests.log`、`build/merge-main-20261108-web-tests.log`、`build/merge-main-20261108-apps.log`、`build/merge-main-20261108-apps-final-check.log`。最终应用兼容性报告为 `build/apps-abi101/rev3_x/report-selected.json`。

## 2026-11-12 合入远程 main

合入源为 `origin/main` 的 `44502f0d5df7e63fe4f180f7cad7bd8dfd342420`，合入前 `dev` 为 `7972bdd`。获取远程状态后确认本地与远程 `dev` 一致，本次引入 102 个上游提交，包括 W5500 以太网、SSH、系统搜索、剪贴板、Modbus、DMX 和灯光控制。

冲突及兼容性处理：

- 解决 8 个冲突文件：网络校时、六份英语/德语目录，以及生成的内嵌语言文件。语言目录按键合并，保留双方新增项，再从最终目录重新生成内嵌资源。
- 修复自动合并未发现的 Flash 布局冲突：上游把设置存储放在 `0x1000000`，会与本分支的中文资源重叠。本分支继续保留该地址的 12 MB 字库和 `0x1C00000` 的崩溃转储，将 256 KB 设置存储放在 `0x1D00000`。旧 `dev` 分区表通过 ESP-IDF 注册相同的空闲区域，注册时检查芯片容量和分区重叠；失败时退回旧 NVS。异常的已命名分区不会被初始化或擦除，擦除失败也不会继续初始化。迁移清理保留旧 NVS 中的语言选择，便于回退。
- 校时保留三个服务器及事件线程中的串行初始化，同时支持仅有线联网时手动校时，避免把 Wi-Fi 关闭误判为没有网络。
- 每种中文补齐 1,002 个新增条目，合并后各有 87 份目录，其中 57 份原生应用目录。保留旧翻译，为新增条目保存简繁审校覆盖表，检查占位符、换行和字形覆盖。DMX、SSH 保留协议名称。字库二进制无需改变，也不需要 SD 卡。
- 新增灯光和 Modbus 网页使用各自的翻译目录，避免与管理页相互覆盖。灯带提示移除仅适用于 LCD-5 的固定针脚编号，保留 GPIO 编号和各板型引脚图。
- `docs/BUILDING.md` 更新分区布局及旧 `dev` 的 OTA 兼容方式。上游 `main` 的不同分区布局不能直接当作本分支的布局使用。

验证范围：

- 27 项 Python 回归与 22 项网页测试通过，共 49 项。新增分区保护测试，扩展仅以太网联网的校时测试和新增网页的翻译隔离检查；保留四种板型的几何、针脚映射及横竖屏触摸检查。最后一次资源整理后，六项中文检查与三项网页资源检查再次通过。
- 本机 `lcd10_1/rev3_x` 的旧构建配置同步开启 `CONFIG_ETH_USE_SPI_ETHERNET=y` 和 `CONFIG_ETH_SPI_ETHERNET_W5500=y`。压缩字体、三个 SNTP 服务器及 USB vendor 接口配置保持开启。
- ESP-IDF 5.5.5 完整编译成功，并在最终目录整理后增量编译，确认原生语言对象和网页资源均晚于最终语言源文件。最终 `p4os.bin` 为 7,664,496 字节，应用分区剩余约 8%；镜像校验有效，芯片范围为 P4 v3.0–v3.99。只有既有的 LVGL/stb_image 及 `screen_off_cb` 未使用警告。
- 本次新增或改动的 11 个动态应用（`can`、`dmx`、`eeprom`、`equipos`、`golf`、`luces`、`modbus`、`monsterhop`、`notas`、`ssh`、`turbo`）按 `rev3_x` 重新编译成功，并针对最终 `lcd10_1/rev3_x` ELF 再次通过 ABI 101、导入符号、LVGL 版本/配置和指令集检查，失败数为 0。其余动态应用未重编译。
- 不包含烧录或实机测试；未重新编译另外七种板型/硅配置组合。

本地测试日志：`build/merge-main-20261112-python-tests.log`（首次检查提示 DMX、SSH 协议名需加入保留名称表，现已修复）、`build/merge-main-20261112-chinese-final-tests.log`、`build/merge-main-20261112-assets-final-tests.log`、`build/merge-main-20261112-web-tests.log`。构建产物和日志保留在本机，不纳入 Git。

最终固件日志：`build/merge-main-20261112-firmware-verified.log`；应用日志：`build/merge-main-20261112-apps.log` 和 `build/merge-main-20261112-apps-final-check.log`。兼容性报告在 `build/apps-abi101/rev3_x/report-selected.json`。

验证镜像在合并提交前构建，版本为 `v0.15.0-219-g7972bdd-dirty`；应用 SHA-256 为 `664a15b45330027bedba6a8fcfb37bb346a76cebaa9d31420ea48f0b44d4393a`，ELF SHA-256 为 `302800528e3927716d2505c61bca1ab60fc32abe8ead0c4c3960940bdb6143e1`。打包字库的 SHA-256 仍为 `c957559d3da940ca81caea0b256c06ff92e2df919a9e2896aac174eb8f679a6b`。提交不会自动改变这些镜像的版本标识。

## 2026-11-14 合入远程 main

合入源为 `origin/main` 的 `18cbaa0a881ea18564326a97de93fc4314286a0c`，合入前 `dev` 为 `5481de9`。获取远程状态后确认本地与远程 `dev` 一致，本次引入 23 个上游提交，主要包括有线网络诊断、设备发现与监测、网络共享及辅助板设计更新。

冲突及兼容性处理：

- 解决三个冲突文件：英语、德语系统目录按键合并，保留双方条目，再从最终目录重新生成内嵌语言 C 文件。
- 网络页面的 `Cable`、`Hosts` 标签纳入翻译提取。每种中文补齐 169 个新条目并保存审校覆盖表，使用简繁各自的本地用语。四种语言的系统目录各有 3,288 个条目；每种中文仍有 87 份目录，其中 57 份原生应用目录。板载 Flash 字库和分区布局不变，无需 SD 卡。
- 修复上游扩大以太网状态结构造成的 ABI 101 越界风险：旧二进制入口 `aos_eth_info` 只写原来的 204 字节；当前头文件通过内联函数将缓冲区长度传给新增的 `aos_eth_info_read`，读取含新增字段的 216 字节结构。P4 和模拟器后端均按调用方长度复制，未来扩展字段补零；通过函数指针调用同样传递正确长度。
- 重新生成动态应用符号表，并将符号生成工具查找编译器的方式改为 Python 的跨平台接口，修复 Windows 上缺少 `which` 命令时无法运行的问题。
- 更新构建说明：旧目录需开启 `CONFIG_LWIP_IP_FORWARD=y` 和 `CONFIG_LWIP_IPV4_NAPT=y`。本机验证配置已同步；W5500、压缩字体、三个 SNTP 服务器和 USB vendor 接口配置保持开启。网络共享仍须由用户开启，lwIP 转发限制已实际链接到固件。

验证范围：

- 30 项 Python 回归与 22 项网页测试通过，共 52 项。新增测试实际执行 P4/模拟器的以太网状态读取代码，检查旧缓冲区边界、新字段、小缓冲区、空指针及函数指针；转发测试检查共享关闭和不同共享接口。首次运行时测试夹具缺少 `libgcc`，修正后相应测试通过。
- ESP-IDF 5.5.5 完整编译 `lcd10_1/rev3_x`，更新符号表后再次链接成功。最终 `p4os.bin` 为 7,768,288 字节，应用分区剩余约 7%；镜像校验有效，芯片范围为 P4 v3.0–v3.99。仅有既有的 LVGL/stb_image 及 `screen_off_cb` 未使用警告。
- 从实际 ELF 读取动态应用解析表：原有 3,654 个导出均保留，最终为 3,725 个导出。旧版及当前以太网头文件的两个 ABI 101 样例均编译成功，并通过最终固件的导入符号检查。
- 本次上游未改动动态应用源码。现有 11 个应用（`can`、`dmx`、`eeprom`、`equipos`、`golf`、`luces`、`modbus`、`monsterhop`、`notas`、`ssh`、`turbo`）针对最终 `lcd10_1/rev3_x` ELF 通过 ABI 101、导入符号、LVGL 配置及指令集检查。本次仅检查这些已有二进制，未重新构建应用，也未重新编译另外七种板型/硅配置组合。
- 未烧录设备，未进行新增网络功能的实机验证；四种板型、横竖屏触摸、网络校时和简繁中文的自动化回归均通过。

本地记录：`build/merge-main-20261114-python-tests.log`、`build/merge-main-20261114-ethernet-abi-tests.log`、`build/merge-main-20261114-forwarding-final-tests.log`、`build/merge-main-20261114-web-tests.log`、`build/merge-main-20261114-symbols.log`、`build/merge-main-20261114-firmware.log`、`build/merge-main-20261114-firmware-final.log`、`build/merge-main-20261114-image-info.log`。导出及应用检查结果为 `build/merge-main-20261114-exports-report.json`、`build/merge-main-20261114-apps-check.log`、`build/merge-main-20261114-apps-report.json`。这些构建产物和日志不纳入 Git。

验证镜像在合并提交前构建，版本为 `v0.15.0-322-g5481de9-dirty`；应用 SHA-256 为 `4de2bab1f0062d588f0fd30eb409af605503d7b06b9c917017ec3b41c935673e`，ELF SHA-256 为 `d315d8de310f1f8d19ef0045c24840db4de31743dd3508571f6baaff4732d6af`。打包字库的 SHA-256 仍为 `c957559d3da940ca81caea0b256c06ff92e2df919a9e2896aac174eb8f679a6b`。提交不会自动更新镜像的版本标识。

## 2026-11-23 合入远程 main

合入源为 `origin/main` 的 `add4d83a1e8b0861aa7266a7dffa38ff5605153c`（0.32.1），合入前 `dev` 为 `cdb91dd`，新增 198 个上游提交，涉及 806 个文件。获取远程状态时确认本地与远程 `dev` 一致。下载曾中断，改用 OpenSSL 后端后完成对象校验和分支更新。

合并处理：

- 解决 10 个冲突文件，包含发布工作流、README、网页、触摸、四份英语/德语目录及两个生成文件。发布流程采用上游应用分片构建，同时保留本分支的双硅配置、独立构建目录、四语言资源和完整烧录包中的字库。
- 每种中文补齐 2,524 个新增界面词条，覆盖商店、备份、股票、音乐工作室、逻辑分析仪、办公、翻译器、航班和新游戏。每种中文现在有 106 份目录，包含 69 份原生应用目录，系统目录含 3,546 个条目。七个新应用网页使用独立词典，日期和数字随设备区域设置变化。简繁字形覆盖全部通过，字库二进制和 Flash 分区布局保持原有配置。
- 商店固件目录增加 ABI 与板型/硅配置匹配，例如 `lcd10_1/rev3_x`。应用包、目录和安装流程支持两种硅配置与 `zh-Hans`、`zh-Hant` 标签。默认源改为本 fork 的 `p4os-store-<profile>.ndjson`，发布脚本和终端工具使用相同命名，已有上游美术资源仍引用其实际发布仓库。目录格式与发布示例见 [STORE.md](STORE.md)。本次完成源码与本地验证，线上目录需经本 fork 的发布流程生成。
- 修复 Mesa 按 720 像素分配缓冲却按实际宽屏尺寸写入的越界，按宿主尺寸分配双缓冲及脏块表。Bastion、Hockey、Metegol 在原生屏幕尺寸与固定画布不匹配时使用已有 LVGL 画布。旋转时释放注入触点，直接读取触点也使用当前旋转坐标。逻辑分析仪的本机和网页接线说明改为查阅对应板型引脚图。

验证结果：

- 30 项 Python 回归和 22 项网页测试全部通过，共 52 项。包括中文格式、字库与内嵌资源，四板型、触摸、网络校时、ABI 101、设置分区保护和网络转发。简繁源目录完整性检查均通过。
- `lcd10_1/rev3_x` 固件编译成功，完整语言和商店修复编入后的镜像为 8,195,104 字节，应用分区剩余约 2%。该空间余量较小，后续增加原生功能仍需检查镜像大小。
- Mesa、Bastion、Hockey、Metegol、Logica 五个动态应用按 `rev3_x` 重新编译并通过 ABI 101 检查。两种硅配置的示例应用分别完成商店打包和临时卡目录安装，验证简繁资源与错误配置拒绝。70 份应用清单、85 个资源包和 6 个系统部件校验通过，发布工作流及脚本语法检查通过。
- 本次构建覆盖 10.1 英寸 `rev3_x`，其余七种固件组合由现有板型测试覆盖相关代码，但未重新编译。实机烧录、商店 OTA 和 GitHub Actions 未执行。

版本识别还依赖发布标签。原有本地标签仅到 `v0.15.0`，本 fork 远端也没有 `v0.32.1` 标签，会使 Git 构建版号与实际代码版本不一致，影响商店的最低固件版本检查。本次从原上游获取了 `v0.32.1` 标签，并核实其指向 `e7908c590e049473afac5d809570d60f277a017f`，确为合入源的祖先。标签当前仅在本地，合并提交后重新构建以生成正确的 `v0.32.1-…` 版号。远端的新克隆和非发布构建也需要对应版本标签，单独推送 `dev` 不会推送标签。

本地记录为 `build/merge-main-20261123-python-tests.log`、`build/merge-main-20261123-web-tests.log`、`build/merge-main-20261123-hans-check.log`、`build/merge-main-20261123-hant-check.log`、`build/merge-main-20261123-firmware-final.log`、`build/merge-main-20261123-firmware-committed.log`、`build/merge-main-20261123-apps.log`、`build/merge-main-20261123-apps-final-check.log` 和 `build/merge-main-20261123-store-manifests.log`。镜像校验和版本信息保存于 `build/merge-main-20261123-image-info.log`，构建产物不纳入 Git。

## 2026-11-24 合入远程 main

合入源为 `origin/main` 的 `6ac8cd8b7c1eb8e8ebae815160d2af0c78ca6ccd`，合入前本地和远程 `dev` 均为 `ead01bf5`。本次取得 77 个上游提交，上游差异涉及 754 个文件，包含串口帧格式和控制线、高清图标、九个应用，以及葡萄牙语、法语和意大利语目录。

合并与兼容性处理：

- 解决 11 个冲突文件，保留四板型、两种硅配置、ABI 101、分配置商店目录与完整烧录包中的 CJK 字库。检查 79 份应用清单（其中 2 份标记为不发布）、89 个资源包和 9 个系统部件；发布脚本整合七种翻译目录，高清图标包沿用上游的安装方式。
- 每种中文补齐 1,416 个新增或新增提取的词条，现在各有 117 份目录、78 份原生应用目录，系统目录含 3,576 个条目。小海龟的积木控件和 154 条内置关卡文字接入翻译目录，Logo/Lua 的实际命令保持原语法。英语、德语及两种中文的内置目录同步生成；葡萄牙语、法语和意大利语保留原来的英文编程控件回退，原生语言包仍可从卡上安装。
- 修复 Asteroides、Tanques 和 Tortuga 在宽板上的缓存或脏块表容量，补齐 Asteroides 的中文绘制、旋转和边缘裁剪。Vuelo 的三档渲染尺寸按实际屏幕比例计算，避免 1280×800 上的上下旧帧和触点偏移。Estado 与 Plotter 使用新增的独立目录复制接口，从 Flash 读取翻译并接受 SD 逐键覆盖，不依赖前台应用目录；语言切换先停止后台任务再重建。中文数值采用小数点。
- 上游扩大的串口状态结构通过带长度的 `aos_serial_stat_ex` 提供给新调用方，保留旧 `aos_serial_stat` 仅写入原有 248 字节。实际固件运行时导出 3,756 个符号，合入前的 3,706 个生成导出全部保留。新增独立语言目录接口为 `aos_i18n_app_catalog_dup`。
- 为保持原 Flash 分区布局，网页翻译按页面共享原文键，七种译文分别压缩，减少 363,134 字节；316 个原生目录采用更深的标准 Deflate 压缩，比相同内容的默认 zlib 减少 34,339 字节。常规固件编译仍直接使用已生成的 C 文件，设备仍用现有 zlib 解压器；维护者重新生成时使用 `gen_lang.py embed --compressor 7zip en de zh-Hans zh-Hant`，见 [BUILDING.md](BUILDING.md)。CJK 字库内容、分区表和无需 SD 卡的中文能力保持不变。

验证结果与范围：

- 37 项 Python 回归与 25 项网页测试通过，共 62 项，无跳过。覆盖板型与旋转、旧/新串口 ABI、简繁格式和字形、关卡文本提取、真实 C 语言加载与应用解析器的无卡/卡覆盖行为、网页压缩目录的 HTTP 加载与语言切换等。简繁源目录检查通过；小海龟的 47 个内置 Logo 关卡通过本机 C 测试。
- `lcd10_1/rev3_x` 最终固件编译成功，镜像为 8,307,680 字节，应用分区剩余 15,392 字节（约 15 KiB，0.18%）。编译器按整数显示为 0% 并提示空间紧张，后续增加原生功能必须再次核查容量。本次没有扩大或重新分配分区。
- Asteroides、DJ、Estado、Plotter、Sumar、Tanques、Tortuga、Vitral、Vuelo 九个动态应用已按 `rev3_x` 编译，并对最终固件通过 ABI 101、导入符号、浮点/指令集及 LVGL 配置检查。两种硅配置的已有示例应用完成七语言商店打包验证，三个工作流及其 23 段脚本语法通过检查。
- 本次只重新编译 10.1 英寸 `rev3_x` 固件；另外七种板型/硅配置组合未重编译，板型几何与相关代码由自动化测试覆盖。未烧录设备，未进行实机验证、商店 OTA 或 GitHub Actions 发布。

本次另从原上游取得本地 `v0.33.0` 标签，已确认其指向 `eef973cbc0dc6a1a9cd28b523431ecf8e430b42d`，属于合入源历史。合并后再次构建以更新固件版号；该标签仍只在本地，推送 `dev` 不会自动推送它。

本地日志为 `build/merge-main-20261124-python-tests.log`、`build/merge-main-20261124-web-tests.log`、`build/merge-main-20261124-hans-check.log`、`build/merge-main-20261124-hant-check.log`、`build/merge-main-20261124-tg-levels.log`、`build/merge-main-20261124-firmware-bootstrap.log`、`build/merge-main-20261124-firmware-final.log`、`build/merge-main-20261124-firmware-committed.log`、`build/merge-main-20261124-exports.log`、`build/merge-main-20261124-apps.log`、`build/merge-main-20261124-apps-precommit-check.log` 和 `build/merge-main-20261124-apps-final-check.log`。首轮应用检查使用的引导构建缺少新符号，最终检查已通过；镜像版本与校验信息保存在 `build/merge-main-20261124-image-info.log`。产物与日志不纳入 Git。
