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
