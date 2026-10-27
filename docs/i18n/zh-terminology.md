# P4OS 简繁中文术语表

本表适用于 P4OS 第一方固件界面、动态应用和管理网页。权威源语言是西班牙语；表中的英文仅用于消除技术歧义。

## 区域与文体

- `zh-Hans` 使用中国大陆常用软件与物联网术语，对外区域格式为 `zh-CN`。
- `zh-Hant` 使用台湾常用软件与物联网术语，对外区域格式为 `zh-TW`。不混用香港特有用语。
- 面向普通用户时使用简洁的动作和状态；安装、接线、协议和诊断页面保留必要技术细节。
- 保留 P4OS、Home Assistant、MQTT、Modbus、CAN、BLE、Wi-Fi、GPIO、I2C、SPI、UART、USB、OTA、TLS、HTTP、JSON、URL、API、SSID、PSRAM、microSD 等名称或缩写。
- 保留路径、文件名、命令、配置键、实体 ID、MQTT 主题、寄存器地址、格式参数和型号原文。

## 核心与设备

| 源概念（西语 / 英语） | 简体中文 | 繁體中文（台湾） | 使用说明 |
|---|---|---|---|
| placa / board, device | 设备；开发板 | 裝置；開發板 | 面向用户的一体机用“设备/裝置”；强调 PCB 或引脚时用“开发板/開發板”。 |
| firmware | 固件 | 韌體 | 不译为“软件”或“固件程序”。 |
| aplicación / app | 应用 | 應用程式 | 空间紧张的按钮可保留“App”。 |
| ajustes / settings | 设置 | 設定 | `zh-Hant` 不使用“設置”。 |
| guardar / save | 保存 | 儲存 | 表单提交统一使用此词。 |
| aplicar / apply | 应用 | 套用 | 与“应用程序”按上下文区分。 |
| habilitar / enable | 启用 | 啟用 | 状态用“已启用/已啟用”。 |
| deshabilitar / disable | 禁用 | 停用 | 状态用“已禁用/已停用”。 |
| reiniciar / restart, reboot | 重启 | 重新啟動 | 指设备或服务重新启动，不表示清除数据。 |
| restablecer / reset | 重置 | 重設 | 仅恢复某项设置时使用。 |
| restaurar de fábrica / factory reset | 恢复出厂设置 | 恢復原廠設定 | 必须与普通“重置”区分。 |
| encendido / on | 开；已开启 | 開；已開啟 | 开关状态，不用于网络连通状态。 |
| apagado / off | 关；已关闭 | 關；已關閉 | 不等同于“离线/離線”。 |
| disponible / available | 可用 | 可用 | 资源或功能可使用。 |
| no disponible / unavailable | 不可用 | 無法使用 | 不等同于“离线/離線”。 |
| desconocido / unknown | 未知 | 未知 | 状态无法确定。 |
| almacenamiento / storage | 存储空间 | 儲存空間 | 介质本身按上下文用“存储/儲存裝置”。 |
| tarjeta / card | 存储卡 | 記憶卡 | 本项目通常指 microSD，避免“卡片”。 |
| archivo / file | 文件 | 檔案 | `zh-Hant` 不使用“文件”指计算机档案。 |
| carpeta / folder | 文件夹 | 資料夾 | `zh-Hant` 不使用“文件夾”。 |
| memoria / memory | 内存 | 記憶體 | Flash 明确写作“Flash”；不要笼统译为“存储”。 |

## 网络、连接与安全

| 源概念（西语 / 英语） | 简体中文 | 繁體中文（台湾） | 使用说明 |
|---|---|---|---|
| red / network | 网络 | 網路 | 包括 Wi-Fi、USB 网络和局域网。 |
| conectar / connect | 连接 | 連線 | 动作；物理接线可用“连接/連接”。 |
| conectado / connected | 已连接 | 已連線 | 表示通信链路已建立。 |
| desconectado / disconnected | 未连接 | 未連線 | 主动或被动断开；不等于设备离线。 |
| sin conexión / offline | 离线 | 離線 | 目标当前无法通过网络访问。 |
| conexión agotada / connection timed out | 连接超时 | 連線逾時 | 不写作含糊的“时间到”。 |
| punto de acceso / access point | 接入点 | 存取點 | Wi-Fi AP；设备自建网络可写“热点”用于用户说明。 |
| emparejar / pair | 配对 | 配對 | BLE 等设备关系；不用于 Wi-Fi 配网。 |
| configurar la red / network setup | 配置网络 | 設定網路 | Wi-Fi 凭据与网络参数。 |
| autenticación / authentication | 身份验证 | 身分驗證 | `zh-Hant` 使用“身分”。 |
| autorización / authorization | 授权 | 授權 | 与身份验证区分。 |
| contraseña / password | 密码 | 密碼 | 不译为“口令”，除非协议上下文要求。 |
| certificado / certificate | 证书 | 憑證 | TLS/HTTPS 证书。 |
| autoridad certificadora / certificate authority | 证书颁发机构 | 憑證授權單位 | 可在紧凑界面保留 CA。 |

## 固件、恢复与诊断

| 源概念（西语 / 英语） | 简体中文 | 繁體中文（台湾） | 使用说明 |
|---|---|---|---|
| actualización OTA / OTA update | OTA 固件更新 | OTA 韌體更新 | 首次出现可写全称；不得暗示会更新板载中文字库。 |
| grabar firmware / flash firmware | 烧录固件 | 燒錄韌體 | `flash` 作动词时用“烧录/燒錄”。 |
| ranura / firmware slot | 固件分区 | 韌體分割區 | 指 OTA A/B 分区，不译为普通“插槽”。 |
| imagen / firmware image | 固件镜像 | 韌體映像 | `zh-Hant` 统一用“映像”。 |
| modo seguro / safe mode | 安全模式 | 安全模式 | 明确其不会加载存储卡应用时保留说明。 |
| volcado / crash dump | 崩溃转储 | 當機傾印 | 与普通日志区分。 |
| registro / log | 日志 | 記錄 | 作为页面名时遵循现有“日志/記錄”。 |
| diagnóstico / diagnostics | 诊断 | 診斷 | 不译为“检测”除非确指测试动作。 |
| copia de seguridad / backup | 备份 | 備份 | 与导出单个文件区分。 |
| recuperación / recovery | 恢复 | 復原 | 指故障恢复流程；恢复设置仍用“恢复/恢復”。 |

## 自动化与工业协议

| 源概念（西语 / 英语） | 简体中文 | 繁體中文（台湾） | 使用说明 |
|---|---|---|---|
| entidad / Home Assistant entity | 实体 | 實體 | 保留实体 ID 原文。 |
| automatización / automation | 自动化 | 自動化 | 不与单次“自动”状态混淆。 |
| escena / scene | 场景 | 場景 | Home Assistant 场景。 |
| sensor | 传感器 | 感測器 | 台湾用“感測器”。 |
| actuador / actuator | 执行器 | 致動器 | 工业控制上下文。 |
| relé / relay | 继电器 | 繼電器 | 触点状态按实际电路描述。 |
| interruptor físico / physical switch | 物理开关 | 實體開關 | 与网络交换机、UI 切换开关区分。 |
| conmutador de interfaz / UI toggle | 切换开关 | 切換開關 | 可按控件空间简称“开关/開關”。 |
| switch de red / network switch | 网络交换机 | 網路交換器 | 不译为一般“开关”。 |
| canal / channel | 通道 | 通道 | 输出、采样或通信通道；不可一律译为“频道”。 |
| contacto normalmente abierto / NO contact | 常开触点（NO） | 常開接點（NO） | 保留 NO。 |
| contacto normalmente cerrado / NC contact | 常闭触点（NC） | 常閉接點（NC） | 保留 NC。 |
| broker MQTT | MQTT 代理服务器 | MQTT Broker | 台湾界面保留行业常用 Broker。 |
| tópico MQTT / MQTT topic | MQTT 主题 | MQTT 主題 | 主题字符串保持原样。 |
| mensaje retenido / retained message | 保留消息 | 保留訊息 | 不写作“已保存消息”。 |
| carga útil / payload | 有效载荷 | 承載資料 | 面向普通用户时可按语境写“消息内容/訊息內容”。 |
| bobina Modbus / coil | Modbus 线圈 | Modbus 線圈 | 与离散输入区分。 |
| entrada discreta / discrete input | 离散输入 | 離散輸入 | 只读位输入。 |
| registro de entrada / input register | 输入寄存器 | 輸入暫存器 | Modbus 只读寄存器。 |
| registro de retención / holding register | 保持寄存器 | 保持暫存器 | Modbus 可读写寄存器。 |

## HTTP 客户端（Cartero）

| 源概念（西语 / 英语） | 简体中文 | 繁體中文（台湾） | 使用说明 |
|---|---|---|---|
| solicitud / request | 请求 | 請求 | HTTP 请求。 |
| respuesta / response | 响应 | 回應 | 台湾统一使用“回應”。 |
| encabezado / header | 请求头／响应头 | 請求標頭／回應標頭 | 不译为页面标题。 |
| cuerpo / body | 请求正文／响应正文 | 請求本文／回應本文 | 可在紧凑标签中用“正文/本文”。 |
| método / method | 方法 | 方法 | GET、POST 等保持大写。 |
| redirección / redirect | 重定向 | 重新導向 | 301/302 等 HTTP 行为。 |
| variable de entorno / environment variable | 环境变量 | 環境變數 | 变量名保持原样。 |
| tiempo de espera / timeout | 超时时间 | 逾時時間 | 错误状态用“请求超时/請求逾時”。 |

## 3D 打印

| 源概念（西语 / 英语） | 简体中文 | 繁體中文（台湾） | 使用说明 |
|---|---|---|---|
| impresora 3D / 3D printer | 3D 打印机 | 3D 印表機 | 台湾采用“印表機”。 |
| trabajo de impresión / print job | 打印任务 | 列印工作 | 与文件本身区分。 |
| cama / print bed | 热床 | 熱床 | 若不加热则按界面上下文用“打印平台/列印平台”。 |
| boquilla / nozzle | 喷嘴 | 噴嘴 | 保留直径单位。 |
| filamento / filament | 耗材 | 線材 | 材料类型 PLA、PETG 等保持原文。 |
| capa / layer | 层 | 層 | 层高单位保持 mm。 |
| pausar / pause | 暂停 | 暫停 | 状态用“已暂停/已暫停”。 |
| reanudar / resume | 继续打印 | 繼續列印 | 避免含糊的单字“继续”。 |
| cancelar impresión / cancel print | 取消打印 | 取消列印 | 破坏性操作需保留确认提示。 |
| origen / home axes | 归零 | 歸零 | 轴运动命令，不译为“主页”。 |

## 质量规则

1. 同一目录中不得混用“设备/裝置”与错误区域词形；因硬件语境需要使用“开发板/開發板”时除外。
2. 状态与动作必须区分，例如“已连接”是状态，“连接”是命令。
3. 值与西班牙语键完全相同仅允许品牌、协议、型号、单位、路径、命令或本来就是中文无关的专名；完整西语句子不得作为中文译文。
4. `%s`、`%d`、`%u`、转义序列、路径、URL 和示例载荷必须原样保留；可在框架允许时调整占位符前后的语序。
5. 自动简繁转换只能处理字形，不能替代台湾术语和句法审校。