# UI ← 主控链路接入说明（屏替代 Android 平板当 WS 客户端）

> 日期：2026-11-13　适用：GR3566 / RK3566 触控屏
> 相关：`128_Net/PTO/PLIP_分析/触控屏指令协议对照.md`（协议实测依据）
> 　　　`128_Net/PTO/001_分析/指令对照表.csv`（46 个 code 全表）

---

## 0 三条结论（用户 2026-11-13 拍板）

| # | 结论 | 影响 |
|---|---|---|
| ① | **我们的屏确定替代那台 Android 平板当 WS 客户端** | 屏直连主控 `ws://<主控IP>:8080/ws`，链路上与原平板完全等价、可互换；不再需要平板做中间人 |
| ② | **buildroot 的 Qt 原本没有 QtWebSockets** | 已补：交叉编译出 `libQt5WebSockets.so.5.15.8`（aarch64），装进 sysroot + 需推到板上 `/usr/lib`（见 §1） |
| ③ | **SID / 光野 / 滤过 / 部位 / 机架 等只读项先不做可控** | 下行照常接收显示；上行只做 kV / mA / ms / mAs / DEN 五个 ±1 意图，其余留口不接 |

---

## 1 QtWebSockets：查证 + 补装

### 1.1 查证结果（改造前）

```
$ grep -i WEBSOCKET  buildroot/output/rockchip_rk3566/.config
# BR2_PACKAGE_QT5WEBSOCKETS is not set          ← 没编
$ find output/rockchip_rk3566/host -name 'libQt5WebSocket*'
（空）
$ find ... -name 'libQt5Network*'
.../sysroot/usr/lib/libQt5Network.so.5.15.8    ← QtNetwork 有（必需，OK）
```

buildroot 里有现成配方 `package/qt5/qt5websockets`，但 `dl/` 没源码缓存，所以走
「下载官方子模块源码 + 用现有交叉 qmake 单独编译」这条路，**不动 buildroot 配置、不重刷镜像**。

### 1.2 已完成的补装（VM 上，`~/qtmods/`）

```bash
# 1) 下载 5.15.8 子模块源码（必须与板上 Qt 同版本）
wget https://download.qt.io/archive/qt/5.15/5.15.8/submodules/qtwebsockets-everywhere-opensource-src-5.15.8.tar.xz
tar xf qtwebsockets-everywhere-opensource-src-5.15.8.tar.xz

# 2) 用 buildroot 的交叉 qmake 配置 + 编译（约 40 秒）
cd qtwebsockets-everywhere-src-5.15.8
~/g3566/g356x-linux/buildroot/output/rockchip_rk3566/host/bin/qmake .
make -j4
make install        # 装进 sysroot：lib + 头文件 + .prl + qml 插件 + mkspecs
```

产物：`libQt5WebSockets.so.5.15.8`　252 KB　`ELF 64-bit LSB, ARM aarch64`
依赖只有 `libQt5Network.so.5` / `libQt5Core.so.5` / `libatomic` / `libstdc++`（板上全都有）。

已装进 sysroot，所以 `QT += websockets` 现在能直接交叉编译（验证过，见 §5）。

### 1.3 板上部署（**必做**，否则起不来）

```bash
# 【Windows】把两个文件推到板子
pscp -pw rockchip share/qtlibs/libQt5WebSockets.so.5.15.8 root@192.168.1.10:/tmp/
pscp -pw rockchip share/qtlibs/myui                      root@192.168.1.10:/tmp/

# 【板子串口】
cp /tmp/libQt5WebSockets.so.5.15.8 /usr/lib/
ln -sf libQt5WebSockets.so.5.15.8 /usr/lib/libQt5WebSockets.so.5
ln -sf libQt5WebSockets.so.5.15.8 /usr/lib/libQt5WebSockets.so
ldconfig
killall myui
cp /tmp/myui /usr/bin/myui
/usr/bin/myui --ws ws://192.168.8.188:8080/ws        # 先手动跑，看日志
```

启动脚本里建议改用环境变量，免得每次改命令行：

```sh
# /etc/init.d/S50myui 里加一行
export MYUI_WS=${MYUI_WS:-ws://192.168.8.188:8080/ws}
```

### 1.4 ⚠ 网络前提：网段要通

原平板在 **192.168.8.0/24**（主控 192.168.8.188）。我们的板子现在是 **192.168.1.10/24**，
两者不通。三选一：

| 方案 | 做法 | 备注 |
|---|---|---|
| A（推荐） | 板子加第二地址 `192.168.8.20/24`：`ip addr add 192.168.8.20/24 dev eth0` | 保留 192.168.1.10 调试通道，两不误 |
| B | 板子改到 192.168.8.x，调试改走串口 | 会丢 192.168.1.10 直连调试 |
| C | 主控侧加路由/改监听地址 | 要动主控，风险大 |

A 要写进启动脚本才会开机生效。

---

## 2 代码结构（新增 2 个文件，界面绘制代码零改动）

```
UI/
├── drlink.h / drlink.cpp   ← 新增：WS 客户端 + 协议映射 + 意图发送
├── main.cpp                ← 加 --ws / --ws-trace / --sim-step，装配链路
├── mainwindow.h/.cpp       ← 加 attachLink()：把 link 接到 setVar 和五个步进器
├── patientcard.h/.cpp      ← StepperCard 加 stepped 信号 + setRemoteDriven
└── UI.pro                  ← QT += network websockets
```

### 2.1 两条耦合面（就这两条）

```
主控 ──下行 2000+──→ DrLink::handleFrame ──varChanged(key,value)──→ MainWindow::setVar
主控 ←─上行 4000+─── DrLink::sendStep ←──StepperCard::stepped(dir)─── 用户点 ±
```

`setVar` 是当初改造时就留好的唯一入口（`mainwindow.h` 注释里写的"以后接网口：收到协议帧
→ 解析成 (key,value) → 调同一个 setVar()"），这次真的只走了这条路，**没有加任何 if 分支到绘制代码**。

### 2.2 远程驱动（StepperCard）

接上链路后五个步进器切 `setRemoteDriven(true)`：

| | 本地模式（无 `--ws`） | 远程模式（有 `--ws`） |
|---|---|---|
| 点 ± | 本地立刻 ±step | **只发 `{"val":±1}`**，本地数值不动 |
| 数值来源 | 本地累加 | 主控广播回填 |
| 边界 | 到 lo/hi 单侧变灰 | 300 ms 没等到广播 → 该侧变灰 + 停连发 |

为什么不在本地预测：抓包实测 mA/ms/mAs 是**主控查表跳档**
（mA: 180→200→220→250→280→320→360→400→450→500→560→630→710→800），
本地按 +5 算出来的数一定是错的，会先跳错值再被广播纠正，视觉上跳两次。

### 2.3 回环防护

- `StepperCard::stepped()` 只在 `applyStep()`（用户点 ± / 长按连发）里发；
  `setValue()` 只发 `valueChanged()`，**不发 stepped** → 主控广播进来不会又发出去。
- mA/ms/mAs 的本地联动（mAs = mA×ms/1000）在接链路后**整体关闭**（`if (m_link) return`）。
  主控三帧各自是权威值，本地反算只会打架（实测：MA=200/MS=5.6/MAS=1.2，反算会把 ms 改成 6.0）。

---

## 3 下行映射表（已接 / 未接）

| code | 字段 | 界面 key | 状态 |
|---|---|---|---|
| 2001 | `KV` | `kv` | ✅ |
| 2002 | `MA` | `ma` | ✅ |
| 2003 | `MS` | `ms` | ✅ |
| 2004 | `MAS` | `mas` | ✅ |
| 4005 | `DEN` | `den` | ✅（上下行同 code） |
| 2005 | `Focus` 0=大 1=小 | `focal` | ✅ |
| 2007 | `HU` | `hu` | ✅ |
| 2012 | `x`/`y` | `det` = "42x42" | ✅（只读显示） |
| 2013 | `SID` | `sid` | ✅（只读显示） |
| 2017/2018/2019 | `AECL`/`AECC`/`AECR` | `aecfield`（0x1 左 / 0x2 中 / 0x4 右） | ✅ 三帧合成一个 mask |
| 3016 | `wall`/`table`/`free` | `workspot` | ✅ |
| 2034 | `NAME` | `name` | ✅ |
| 2009 | `ItemName` | `part` | ✅（`BinData` 缩略图未接） |
| 3007 | `temperature` | `hutemp` | ✅ |
| 3023 | `Filter` | `filt` | ✅（原样显示 "0.9mmCu"，未转 "0.9 Cu + 1 Al" 格式） |
| 3001/3002 | `flag` | `deta`/`detb` | ⚠ 语义未定，只落 trace |
| 2008 | `wallTable` | `workspot` | ⚠ 语义未定，用 3016 代替 |
| 2006 | `Mode` | — | ⚠ AEC 开关状态可能在这里，待验证 |
| 2015 / 2020 | `SysStatus` / `info`+`level` | — | ⚠ 告警文本，界面暂无位置，落日志 |
| 3020 | `CurrEI`/`TargetEI` | `el` | ⚠ 量纲不一致（协议 250~1000 vs 界面 50~125），待定 |
| 3005 | `nodeType`/`nodePos` | `angle`/`standh` | ⚠ 机架（只读项，排后） |
| 2025 / 2028 / 4013 | `Size`/`SuggestSID`/`TOD` | — | ⚠ 未接 |

### 3.1 顺带修掉的两个显示偏差（都有抓包依据）

1. **ms 小数位**：主控给 `5.6 / 12.5 / 2.2`，原来 `decimals=0` 会四舍五入成 6 → 改 `decimals=1`；
   同时 `setValue(v, snapGrid=false)`，不再按 step=1 的网格把 5.6 吸成 6.0。
2. **DEN 范围**：实测是 `-8 … 0 … +8` 共 17 档（原界面写的 0..5）→ 改 `lo=-8, hi=+8`。

---

## 4 上行指令表（本版实现）

| 界面操作 | 发出帧 | 实测依据 |
|---|---|---|
| kV ± | `{"code":4001,"data":{"val":±1}}` | #19283 等 49 次 |
| mA ± | `{"code":4002,"data":{"val":±1}}` | #24854 等 15 次 |
| ms ± | `{"code":4003,"data":{"val":±1}}` | #23990 等 26 次 |
| mAs ± | `{"code":4004,"data":{"val":±1}}` | #19398 等 32 次 |
| DEN ± | `{"code":4005,"data":{"val":±1}}` | #25978 等 38 次 |
| 连心跳 | `{"code":100,"data":{}}` 每 5 s | 37 帧 / 421 s |

已实现但界面还没接的（`DrLink` 里留了方法）：`sendAecField`(4008)、`sendFocus`(2005)、
`sendEi`(3020)、`sendMode`(2006)。
只读项（SID / 光野 / 滤过 / 部位 / 机架）**不做上行**。

其它链路行为：
- 连上后自动发一次 `4009 {}` 拉部位库（照平板的做法）；
- 断线重连：1 s → 2 s → 4 s → 8 s 封顶；
- 15 s 收不到任何下行 → 主动 abort 重建（3 个心跳周期）；
- 上行帧由 Qt 自动加掩码（`MASK=1`），与真机一致（抓包 23518 那帧的 `A5D26374` 就是掩码）。

---

## 5 联调验证（没有真主控也能验）

写了个假主控：`share/tools/fake_dr_server.py`（纯标准库，120 行，RFC6455 服务端）。

```bash
# 【Ubuntu 虚拟机】
python3 ~/tools/fake_dr_server.py 18080
```

### 5.1 链路打通（实测日志）

```
link: connecting ws://127.0.0.1:18080/ws
link: connected
tx {"code":4009,"data":{}}
rx 2001 {"KV":"81"}          → 界面 kV 变 81
rx 2002 {"MA":"200"}         → mA 200
rx 2003 {"MS":"5.6"}         → ms 5.6
rx 2004 {"MAS":"1.2"}        → mAs 1.2
rx 4005 {"DEN":"2"}
rx 2005 {"Focus":0}          → focal spot -> Large
rx 2012 {"x":"42","y":"42"}  → 42 x 42 cm
rx 2013 {"SID":"115"}        → Req.SID 115 cm
rx 2017/2018/2019            → aec field mask 0x0 → 0x2
rx 2007 {"HU":"37"}          → HU 37%
rx 3007 {"temperature":"31"} → hutemp 31
rx 3023 {"Filter":"0.9mmCu"} → 滤过 0.9mmCu
rx 2034 {"NAME":"AHMED, AYESHA"} → 顶栏 AHMED, AYES…
rx 2009 {"ItemName":"Chest PA"}  → 缩略图 Chest PA
rx 3016 {"wall":1,...}           → workspot -> Wallstand
```

### 5.2 按钮 → 上行 → 回推 → 界面（`--sim-step` 自检）

`--sim-step <ms>` 会用真实 QMouseEvent 依次点五个步进器的 +，走完整路径：

```
sim-step kv +1 -> value=81 remote=1      ← 本地值不动（正确）
rx 2001 {"KV":"82"}                      ← 主控回推 82，界面更新
sim-step ma +1 -> value=200 remote=1
rx 2002 {"MA":"220"}                     ← 200→220 是查表跳档，不是 +5
sim-step den +1 -> value=2 remote=1
rx 4005 {"DEN":"3"}
sim-step: ms 不可用                      ← 当前 exp 模式 kV-mA，ms/mAs 本来就灰
```

### 5.3 界面截图核对

`QT_QPA_PLATFORM=offscreen ./myui --ws ... --shot /tmp/s7` 导出的整页，
数值全部来自主控广播（kV 81 / mA 200 / mAs 1.2 / ms 5.6 / SID 115 / 42x42 / HU 37% / AHMED, AYES…）。

---

## 6 命令行

```
./myui --ws ws://192.168.8.188:8080/ws     连主控（不给就不连，保持本地演示）
      --ws-trace                           打印每帧收发原文（排错必开）
      --sim-step 1500                      1.5 s 后自检点五个 +（验上行）
      （或环境变量 MYUI_WS 指定地址）
```

---

## 7 下一步（待定，需用户拍板）

1. **板上实机联调**：先解决网段（§1.4），再推 `libQt5WebSockets.so.5.15.8` + `myui`。
2. **AEC 开关状态**：界面 AEC 开关现在纯本地切换；主控的 AEC 状态可能在 `2006.Mode`
   或 `4008` 相关帧里，需要现场抓一次「在平板上关 AEC」的包确认。
3. **告警显示**：`2015.SysStatus` / `2020.info` 有内容但界面还没有位置（设计稿没这块），
   建议先落日志 + 底栏叹号，等设计定。
4. **EI 量纲**：`3020.CurrEI` 是 250/375/500/750/1000，界面 EL 下拉是 50/64/80/100/125，
   两者不是同一套刻度，需要确认映射关系再显示。
5. **缩略图**：`2009.BinData` 里是真实体位示意图（4009 应答里也有 819×830 的图），
   现在缩略图还是静态 Chest PA，可以接上。
6. 长按节奏已从 500/80 ms 改成 300/150 ms（贴近平板实测 130~180 ms）。

---

## 8 踩过的坑（写下来免得重犯）

1. **pscp 保留源文件 mtime** → 传上去后 `make` 认为不用重编，改了代码却没生效。
   每次改完先 `touch` 再 `make`，或比对产物大小。
2. **`pkill -f xxx` 会杀掉自己**：plink 执行的命令行里也含 xxx，匹配到就把 ssh 断了。
   用 `FPID=$!` 记 pid 再 `kill $FPID`。
3. **`cd dir && nohup cmd &` 会把整个 `cd &&` 丢进后台**，后续命令还在原目录 →
   拆成 `cd dir; nohup cmd &`。
4. **`--shot <dir>` 不会自建目录**，目录不存在就静默不落盘。
5. **pscp 上传前目标目录必须先存在**，否则静默失败（脚本还是报 uploaded）。
