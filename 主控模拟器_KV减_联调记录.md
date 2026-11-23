# 触控屏 KV− → PC 主控 → 回 KV 值：联调记录

> 角色：**板子 192.168.8.10 = 触控屏（WS 客户端）**，**PC 192.168.8.188 = 主控（WS 服务端）**。
> 协议依据：`PLIP.pcapng` 帧 #23518（上行）、#23523（下行），见
> `128_Net/PTO/PLIP_分析/帧23518_逐字段解析.md` / `帧23523_逐字段解析.md`。

---

## 1 结论先行

按一次 KV−，整条链路的实际时序（实测，非推测）：

```
[板子] 点「−」           → tx {"code":4001,"data":{"val":-1}}         ← 与抓包 #23518 逐字节一致
[PC]   收到，0.1ms 内判定  → ✓ accepted
[PC]   状态 81 → 80       → tx {"code": 2001, "data": {"KV": "80"}}   ← 与抓包 #23523 同格式
[PC]   连带重播           → 2004 MAS / 2003 MS / 2002 MA              ← 与抓包一致
[板子] 收帧落界面          → KV 显示 80（截图 `板子截图_KV从81变80.png`）
```

---

## 2 新增/改动的文件

| 文件 | 作用 |
|---|---|
| `share/tools/pc_master_sim.py` | **PC 端主控模拟器**，按抓包规则校验后回帧（本次核心） |
| `share/tools/proto_conformance.py` | 校验逻辑的一致性测试，8 条用例 |
| `share/UI/main.cpp` | `myui` 新增 `--sim-key` / `--sim-dir`，可程序化点「−」格 |
| `share/UI/主控模拟器_KV减_联调记录.md` | 本文 |

与旧脚本的分工，别用混：

| 脚本 | 干什么 | 有无校验 |
|---|---|---|
| `fake_dr_server.py` | 只回一格，验证「链路通不通」 | 无 |
| `pc_master_sim.py` | 校验来源/结构/使能/范围/档位表，越界静默 | **有** |

---

## 3 关键步骤（可复现）

### 步骤 1 起 PC 端主控

【Windows】

```
"C:\Users\Administrator\.workbuddy\binaries\python\envs\default\Scripts\python.exe" "D:\1_dongshengjin\128\128_Linux\ubuntu20\share\tools\pc_master_sim.py"
```

默认参数即对齐真机：监听 `:8080/ws`、白名单只收 `192.168.8.10`、曝光模式 `kv-ma`
（只有 kV/mA 可调，ms/mAs/DEN 视为灰）。启动会打印初始状态：

```
pc master sim  :8080/ws   白名单=192.168.8.10  模式=kv-ma  校验=on
初始状态：KV=81 MA=200 MS=5.6 MAS=1.2 DEN=2
```

> 常用开关：`--port 18080`（换端口）、`--allow-any`（不校验来源，本机自测用）、
> `--mode all`（放开 ms/mAs/DEN）、`--init "kv=150"`（改初始值测边界）、
> `--no-guard`（关全部校验，做对照组）。

### 步骤 2 板子连主控

平时不用管——`/etc/myui.conf` 里已经是 `MYUI_WS="ws://192.168.8.188:8080/ws"`，开机自连。

【板子串口】要手动跑（带链路 trace）：

```
killall myui; XDG_RUNTIME_DIR=/var/run /usr/bin/myui --ws ws://192.168.8.188:8080/ws --ws-trace > /tmp/ws.log 2>&1 &
```

### 步骤 3 按 KV−

手指点屏即可。没有手指条件时，用内置自检（走的是**真实鼠标事件路径**：
`QMouseEvent → mousePressEvent → applyStep → stepped → DrLink::sendStep`，和手指点击完全同一条代码）：

【板子串口】

```
killall myui; XDG_RUNTIME_DIR=/var/run /usr/bin/myui --ws ws://192.168.8.188:8080/ws --ws-trace --sim-step 4000 --sim-key kv --sim-dir -1 > /tmp/kvminus.log 2>&1 &
```

`--sim-step 4000` = 起 4 秒后点一次（等链路把全量状态收完），`--sim-dir -1` = 点「−」格。

### 步骤 4 两侧对日志

【板子串口】

```
grep -E 'tx |rx 200' /tmp/kvminus.log
```

---

## 4 实测日志（原始输出）

**板子侧**（`/tmp/kvminus.log`）：

```
link: connected
tx {"code":4009,"data":{}}          ← 连上先拉部位库
rx 2001 {"KV":"81"}  rx 2002 {"MA":"200"}  rx 2003 {"MS":"5.6"}  rx 2004 {"MAS":"1.2"}   ← 全量
tx {"code":4001,"data":{"val":-1}}   ← 按 KV−
rx 2001 {"KV":"80"}                  ← 主控回的新值
rx 2004 {"MAS":"1.2"}  rx 2003 {"MS":"5.6"}  rx 2002 {"MA":"200"}   ← 连带重播
tx {"code":100,"data":{}}            ← 心跳
```

**PC 主控侧**：

```
15:18:17.589 TX  36B  {"code": 2001, "data": {"KV": "81"}}      ← 连上全量下发
...
15:18:21.339 RX mask=1 {"code":4001,"data":{"val":-1}}          ← 收到 KV−
15:18:21.339 判定 ✓ accepted  (0.1ms)
15:18:21.339 TX -> 1 client(s)   36B  {"code": 2001, "data": {"KV": "80"}}
15:18:21.339 TX -> 1 client(s)   38B  {"code": 2004, "data": {"MAS": "1.2"}}
15:18:21.339 TX -> 1 client(s)   37B  {"code": 2003, "data": {"MS": "5.6"}}
15:18:21.339 TX -> 1 client(s)   37B  {"code": 2002, "data": {"MA": "200"}}
```

> `RX mask=1` 说明上行帧带掩码（客户端→服务端），下行 `TX` 不带——与抓包一致，
> 这也是 Wireshark 里能不能直接读到 JSON 的分水岭。

---

## 5 主控「判断是否正常」的规则（已实现）

按顺序判，任何一条不过就**静默不回帧**（不报错、不改状态）——这是实测行为，见
`触控屏指令协议对照.md` §5.3（ms 连按 3 次无变化）。

| # | 判定 | 不过时的日志 |
|---|---|---|
| 1 | 来源 IP = `192.168.8.10` | `!! 来源不在白名单，直接断开` |
| 2 | JSON 可解析 | `非法 JSON（判定 bad-json，丢弃）` |
| 3 | `code` 在 4001~4005 | `unknown-code(不在 4001~4005)` |
| 4 | `data.val` 是整数且 `abs(val)==1` | `bad-val(协议只允许 ±1)` |
| 5 | 该参数在当前曝光模式下可用 | `not-enabled(ms 在 kV-mA 模式下为灰)` |
| 6 | 步进后未越出范围 / 档位表端点 | `at-limit(已到端点，静默不回)` |
| 7 | 通过 → 改状态 + 广播 | `判定 ✓ accepted` |

**范围与档位表**（抓包实测段 + R10 外推补齐小档位）：

| 参数 | 规则 |
|---|---|
| kV | 固定 ±1，范围 40 ~ 150 |
| mA | 查表：180→200→220→250→280→320→360→400→450→500→560→630→710→800 |
| ms | 查表：71→63→56→50→45→40→36→32→28→25→22→18→16→14→12.5→11→10→9→8 |
| mAs | 查表：3.2→3.6→4.0→4.5→5.0→5.6→6.3→7.1→8.0→9.0→10→11→12.5→14→16→18 |
| DEN | 有符号等级 −8 ~ +8 |

**耦合重算**（抓包体现的规律，已实现）：
`mAs = mA × ms / 1000`。改 mA 或 ms → 重算 mAs；改 mAs → 反算 ms（mA 不动）。
改 kV 不动三者，但按抓包会**连带重播** 2004/2003/2002。

---

## 6 校验逻辑自测（8/8 通过）

【Windows】先起被测实例（换端口，不干扰板子那条线）

```
"C:\Users\Administrator\.workbuddy\binaries\python\envs\default\Scripts\python.exe" "D:\1_dongshengjin\128\128_Linux\ubuntu20\share\tools\pc_master_sim.py" --port 18081 --allow-any --init kv=41 --quiet-snapshot
```

【Windows】上一条起好后再执行

```
"C:\Users\Administrator\.workbuddy\binaries\python\envs\default\Scripts\python.exe" "D:\1_dongshengjin\128\128_Linux\ubuntu20\share\tools\proto_conformance.py" 18081
```

实测结果：

```
[PASS] kv −1（41→40，到下限的最后一格）   {'code': 2001, 'data': {'KV': '40'}}
[PASS] kv −1（已在下限 40，应静默）       无下行（正确）
[PASS] kv +2（协议只许 ±1）              无下行（正确）
[PASS] kv 0（非法值）                    无下行（正确）
[PASS] DEN +1（kV-mA 模式下 DEN 是灰的）  无下行（正确）
[PASS] 4008（未实现的 code）             无下行（正确）
[PASS] 心跳 100                          {'code': 101, 'data': None}
[PASS] 非 JSON 垃圾帧                    无下行（正确）

8/8 通过
```

---

## 7 界面上「到底变灰」是怎么来的（重要）

主控在边界**不回帧**，所以界面不能靠返回值判断。做法：
`DrLink` 发出意图后起 300 ms 计时，期间没收到该参数的下行 → `stepAtLimit` →
`StepperCard::noteLimit(dir)` 把**那一侧**格子变灰；一旦广播把值改回来，灰自动清掉。
实测中位回包 34 ms（抓包），本地 0.1 ms，300 ms 余量充足。

---

## 8 待你核对的疑问

1. **kV 上限到顶时主控到底回不回？** 抓包只抓到 ms 到顶静默（22→22→22→22），
   kV 150 的样本没有。我按「静默」实现。若真机是「回当前值」，
   界面那边 300 ms 判灰就得改成比对值是否变化。
2. **`4008`（AEC 场域）的 `val` 语义未定**，现在一律拒收。
   要支持 AEC 左/中/右开关，得再抓一次「在触控屏上单独点一个场域」的包。
3. **组合上限没实现**：抓包里出现过 `2010.ExpState=6` + `2020.info="MA LIMIT"`
   （mAs 加到 16.0 后不再上升并弹告警），说明主控还有「kV/mA/mAs 搭配上限」这层互锁。
   我只做了单参数范围，没做组合表。要不要做？需要你给机型的参数上限表。
4. **曝光模式切换**（`2006 Mode`，抓包见过 0/1/2/9）会改变哪些参数可用。
   现在固定 `kv-ma` 模式；要不要做模式切换的下行/上行？
5. **长按连发 150 ms** 时主控会不会限速/丢帧？抓包只见到 130~180 ms，
   我们界面已按 150 ms 连发，理论上 1 帧约 37 字节，带宽毫无压力，但真主控是否自带节流未知。
6. **是否需要 ack/id/seq**？抓包里的上行帧只有 `code` + `data`，
   没有任何序号或确认字段，所以现在是「无 ack，靠广播回填」。
