#!/bin/bash
# =============================================================================
#  界面调试脚本  dbg.sh
# -----------------------------------------------------------------------------
#  作用：把「改一个数值 → 界面立刻变」变成一条命令，不用改代码、不用重编译。
#  原理：myui 用 --vars 监视 debug_vars.txt，本脚本只负责往里写，
#        界面在 120ms 内自动刷新。界面没启动时也可以先写好，起来就生效。
#
#  常用命令
#    ./dbg.sh start            启动界面（虚拟机里是 60% 窗口，按 Esc 退出）
#    ./dbg.sh start full       启动界面（全屏，板子上用这个）
#    ./dbg.sh stop             关掉界面
#    ./dbg.sh apply            把下面「① 数值接口」区的值全部推到界面
#    ./dbg.sh set kv=120 aec=1 临时改几个，不动脚本（可以写多个）
#    ./dbg.sh only focal=large 只保留这一条，其他全清（单变量对照用）
#    ./dbg.sh show             看界面当前收到的是什么
#    ./dbg.sh list             列出所有可改的 key
#    ./dbg.sh reset            全部回设计稿初值
#    ./dbg.sh demo             自动播放一遍（27 步，每步 2 秒）
# =============================================================================

# =============================================================================
#  ① 数值接口  ——  想改哪个就改这一行，然后执行  ./dbg.sh apply
# -----------------------------------------------------------------------------
#  值写 auto 表示「这一项不发」，由界面自己算（比如 mAs 跟着 mA×ms 走）。
# =============================================================================
PATIENT=auto          # 体型        L / M / S
WORKSPOT=auto         # 站位        wallstand / table / free
CENTER=auto           # 准星对中    0 / 1
FOLLOW=auto           # 跟随        0 / 1
AEC=auto              # AEC 总开关  0 / 1   （开 → DEN 才可调）
AECFIELD=auto         # AEC 场域    left / right / bottom / left+right / none
FOCAL=auto            # 焦点        small（小焦点）/ large（大焦点）
EL=auto               # EI 目标值   50 / 64 / 80 / 100 / 125 ｜ 320 = 不覆盖底图
EXP=auto              # 参数组合    kv / mas / ma ｜ baked = 不覆盖底图
KV=auto               # 管电压      40 ~ 150
MA=auto               # 管电流      10 ~ 500
MAS=auto              # 毫安秒      0.1 ~ 500（auto = 按 mA×ms 自动算，推荐）
MS=auto               # 曝光时间    1 ~ 1000
DEN=auto              # 密度        0 ~ 5（AEC 灭时显示「–」）
PATINFO=auto          # 患者面板    open / close
# ---- 顶栏 / 缩略图 ----------------------------------------------------------
NAME=auto             # 患者姓名    如 "Ahmed, Ayesha"（过长自动省略号）
PART=auto             # 部位名      如 Chest PA / Knee AP（卡内居中）
# ---- 读数区（屏幕上数第 1 条黑带，4 格）-------------------------------------
SID=auto              # Req.SID     第 1 格，只发数字（cm 固定）
PHEIGHT=auto          # 患者身高    第 2 格（cm）
ANGLE=auto            # 投照角度    第 3 格（°，单位固定）
STANDH=auto           # 支架高度    第 4 格（cm）
# ---- 底栏（屏幕最底 1 条黑带）----------------------------------------------
DET=auto              # 探测器尺寸  29 / 35 …（写 29x35 只取第一个数）
FILT=auto             # 滤过        整串文字，如 "0.9 Cu + 1 Al"
TEMPA=auto            # 球管 A 温度 ≥70 自动转警示色（红柱）
TEMPB=auto            # 球管 B 温度
HU=auto               # HU 百分比   0 ~ 100（会右对齐，不会压到右边的剂量环）
BATA=auto             # 电池 A      0..4 / chg（充电）/ low（低电，橙）
BATB=auto             # 电池 B      同上
TUBE=auto             # 在用球管    A / B（另一只的方块与箭头置灰）
GRID=auto             # 栅格 A      0 正常 / 2 告警（转橙）
GRIDB=auto            # 栅格 B      同上
DETA=auto             # 探测器 A    ready（蓝，就绪）/ off（灰，未就绪）
DETB=auto             # 探测器 B    ready / off
COLLIM=auto           # 准直器      on（白）/ off（灰）/ warn（橙）
FILTS=auto            # 滤过图标    ok（白）/ warn（橙），文字另用 filt
# =============================================================================

# ---------------------------------------------------------------------------
#  ② 环境（一般不用改）
# ---------------------------------------------------------------------------
UI_DIR=$(cd "$(dirname "$0")" && pwd)
VARS=${VARS:-$UI_DIR/debug_vars.txt}
LOG=${LOG:-/tmp/myui.log}
# 镜像落点：Qt Creator 是影子构建，二进制在 ~/build-UI-DesktopQt5-*/ 里，
# 老版本按 appDir/.. 找变量文件 ⇒ 落到 $HOME/debug_vars.txt，跟 $VARS 不是一个地方，
# 结果「set 了却毫无变化」。该文件存在时顺手同步一份，省得非得重编才能调。
MIRROR=${MIRROR_VARS:-$HOME/debug_vars.txt}

# 二进制：虚拟机用 build_host 里的，板子上用 myui
BIN=""
for c in "$UI_DIR/build_host/myui" "$UI_DIR/myui" /usr/bin/myui; do
    [ -x "$c" ] && { BIN="$c"; break; }
done
if [ "$BIN" = "$UI_DIR/build_host/myui" ]; then IS_HOST=1; else IS_HOST=0; fi

# ---------------------------------------------------------------------------
#  工具函数
# ---------------------------------------------------------------------------
die() { echo "✗ $*" >&2; exit 1; }

running() { pgrep -x myui >/dev/null 2>&1; }

# 界面没跑时给提示：值已经写进文件了，只是没人读
hint_stopped() {
    running && return 0
    echo "⚠ 界面没在运行（Esc 键会退出窗口）"
    echo "  值已写入 $VARS，执行 ./dbg.sh start 后立刻生效"
    return 1
}

# 原子写：先写临时文件再 mv，避免界面读到半个文件
commit() {
    mv "$VARS.tmp" "$VARS"
    if [ -n "$MIRROR" ] && [ "$MIRROR" != "$VARS" ] && [ -f "$MIRROR" ]; then
        cp -f "$VARS" "$MIRROR" 2>/dev/null || :
    fi
}

# 增量写一条：key=value（覆盖同名 key，保留其它行）
put_one() {
    local kv=$1 k=${1%%=*}
    [ "$kv" = "$k" ] && { echo "✗ 格式应为 key=value：$kv" >&2; return 1; }
    touch "$VARS"
    grep -v "^[[:space:]]*${k}[[:space:]]*=" "$VARS" > "$VARS.tmp" 2>/dev/null || : > "$VARS.tmp"
    printf '%s\n' "$kv" >> "$VARS.tmp"
    commit
}

# 发一条就提示一下
say() { printf '  %-9s = %s\n' "${1%%=*}" "${1#*=}"; }

# 等界面把文件读进去（watcher 防抖 120ms，留点余量）
settle() { sleep 0.3; }

# 复位：先让界面回到设计稿初值，再把文件清空（否则以后每次重载都会复位）
do_reset() {
    printf 'reset=1\n' > "$VARS.tmp"; commit
    settle
    : > "$VARS.tmp"; commit
}

# ---------------------------------------------------------------------------
#  命令
# ---------------------------------------------------------------------------
cmd_start() {
    [ -n "$BIN" ] || die "找不到 myui（先编译，或改脚本里的 BIN）"
    if running >/dev/null 2>&1; then
        echo "界面已经在跑了（pid $(pgrep -x myui | tr '\n' ' ')），变量直接 ./dbg.sh set 即可"
        return 0
    fi
    # 通过 SSH 执行时没有 DISPLAY，自动借用桌面 :0（否则窗口创建不出来）
    if [ -z "$DISPLAY" ] && [ -S /tmp/.X11-unix/X0 ]; then
        export DISPLAY=:0
        [ -f "$HOME/.Xauthority" ] && export XAUTHORITY="$HOME/.Xauthority"
        echo "（无 DISPLAY，自动使用 :0）"
    fi
    [ -f "$VARS" ] || : > "$VARS"
    local mode=$1
    if [ "$mode" = "full" ]; then
        echo "启动（全屏）… 日志 $LOG"
        nohup "$BIN" --vars "$VARS" > "$LOG" 2>&1 &
    else
        # 虚拟机里默认开窗口，方便和编辑器并排；板上没桌面，全屏
        if [ "$IS_HOST" = "1" ]; then
            echo "启动（窗口模式，Esc 退出）… 日志 $LOG"
            nohup "$BIN" --windowed --vars "$VARS" > "$LOG" 2>&1 &
        else
            echo "启动（全屏）… 日志 $LOG"
            nohup "$BIN" --vars "$VARS" > "$LOG" 2>&1 &
        fi
    fi
    sleep 1.2
    if running >/dev/null 2>&1; then
        echo "✓ 已启动，变量文件 $VARS"
        cmd_show
    else
        echo "✗ 启动失败，最后几行日志："
        tail -15 "$LOG"
    fi
}

cmd_stop() {
    # 用 -x 精确匹配进程名：pkill -f 会连 "vim myui.cpp"、ssh 命令行一起杀掉
    pkill -x myui 2>/dev/null
    sleep 0.4
    pkill -9 -x myui 2>/dev/null
    running >/dev/null 2>&1 && echo "还在跑（pid $(pgrep -x myui | tr '\n' ' ')）" || echo "✓ 已关闭"
}

cmd_apply() {
    # 先复位：保证结果是「接口区描述的状态」，不被之前的 set 残留污染（幂等）
    do_reset
    : > "$VARS.tmp"
    emit() { [ "$2" = "auto" ] || [ -z "$2" ] || printf '%s=%s\n' "$1" "$2" >> "$VARS.tmp"; }
    emit patient  "$PATIENT"
    emit workspot "$WORKSPOT"
    emit center   "$CENTER"
    emit follow   "$FOLLOW"
    emit aec      "$AEC"
    emit aecfield "$AECFIELD"
    emit focal    "$FOCAL"
    emit el       "$EL"
    emit exp      "$EXP"
    emit kv       "$KV"
    emit ma       "$MA"
    emit mas      "$MAS"
    emit ms       "$MS"
    emit den      "$DEN"
    emit patinfo  "$PATINFO"
    emit name     "$NAME"
    emit part     "$PART"
    emit sid      "$SID"
    emit pheight  "$PHEIGHT"
    emit angle    "$ANGLE"
    emit standh   "$STANDH"
    emit det      "$DET"
    emit filt     "$FILT"
    emit tempa    "$TEMPA"
    emit tempb    "$TEMPB"
    emit hu       "$HU"
    emit bata     "$BATA"
    emit batb     "$BATB"
    emit tube     "$TUBE"
    emit grid     "$GRID"
    emit gridb    "$GRIDB"
    emit deta     "$DETA"
    emit detb     "$DETB"
    emit collim   "$COLLIM"
    emit filts    "$FILTS"
    commit
    settle
    echo "✓ 已推送："
    cmd_show
    hint_stopped
}

cmd_set() {
    [ $# -ge 1 ] || die "用法：./dbg.sh set kv=120 [aec=1 ...]"
    echo "推送："
    for kv in "$@"; do
        put_one "$kv" && say "$kv"
    done
    settle
    hint_stopped
}

cmd_only() {
    [ $# -ge 1 ] || die "用法：./dbg.sh only focal=large"
    do_reset
    : > "$VARS.tmp"
    for kv in "$@"; do printf '%s\n' "$kv" >> "$VARS.tmp"; done
    commit
    settle
    echo "✓ 只保留："
    cmd_show
    hint_stopped
}

cmd_reset() {
    do_reset
    echo "✓ 已回设计稿初值"
}

cmd_show() {
    if [ -s "$VARS" ]; then
        echo "--- 界面当前收到（$VARS）---"
        sed 's/^/    /' "$VARS"
        echo "------------------------------"
    else
        echo "（变量文件是空的：界面显示设计稿原样）"
    fi
}

cmd_list() {
    cat <<'EOF'
可改的 key（等号后面是取值）
  patient    L | M | S                        体型（顶栏 M 卡）
  workspot   wallstand | table | free         站位卡
  focal      small | large                    焦点：小焦点 / 大焦点
  el         50 | 64 | 80 | 100 | 125 | 320    EI 目标值（320=底图原样）
  exp        kvmas | kvmas_ms | kvma_ms | baked  曝光参数组合（决定哪几格可调；首屏 kvmas）
  aec        0 | 1                             AEC 开关（连带 DEN 可调）
  center     0 | 1                             准星对中
  follow     0 | 1                             跟随
  aecfield   left / right / bottom / left+right / none   AEC 场域（可多选）
  kv         40 ~ 150                          步进器 kV
  ma         10 ~ 500                          步进器 mA（改它 → 重算 mAs）
  mas        0.1 ~ 500                         步进器 mAs（改它 → 反算 ms）
  ms         1 ~ 1000                          步进器 ms（改它 → 重算 mAs）
  den        0 ~ 5                             步进器 DEN（要 AEC 亮）
  patinfo    open | close | toggle             患者详情面板
  reset      1                                 全部回设计稿初值

顶栏 / 缩略图
  name       任意文字       患者姓名，如 "Ahmed, Ayesha"（太长自动加省略号）
  part       任意文字       缩略图部位名，如 Chest PA / Knee AP

读数区（屏幕上 4 格读数，单位固定，只发数字）
  sid        0 ~ 999       Req.SID（cm）
  pheight    0 ~ 999       患者身高（cm）
  angle      0 ~ 999       投照角度（°）
  standh     0 ~ 999       支架高度（cm）

底栏（屏幕最底一条）
  det        20 ~ 60       探测器尺寸 cm（写 29x35 只取 29）
  filt       任意文字      滤过，如 "0.9 Cu + 1 Al"
  tempa      0 ~ 150       球管 A 温度（℃），≥70 转红
  tempb      0 ~ 150       球管 B 温度（℃），≥70 转红
  hu         0 ~ 100       HU 百分比（右对齐，长数字不会压到剂量环）
  bata       0..4 / chg / low    电池 A：电量格 / 充电 / 低电（橙）
  batb       0..4 / chg / low    电池 B
  tube       A | B         哪只球管在用（另一只置灰）
  grid       0 | 2         栅格 A：正常 / 告警（转橙）
  gridb      0 | 2         栅格 B
  deta       ready | off   探测器 A：蓝（就绪）/ 灰（未就绪）
  detb       ready | off   探测器 B
  collim     on | off | warn   准直器：白 / 灰 / 橙
  filts      ok | warn     滤过图标：白 / 橙（文字用 filt）

注意：值超出范围不会报错，会自动夹到边界，正好用来看边界态。
EOF
}

cmd_demo() {
    echo "开始演示（27 步，每步 2 秒）…"
    running || cmd_start

    step() {
        printf '\n[%2d] %-20s %s\n' "$1" "$2" "$3"
        put_one "$2"
        sleep 2
    }
    step  1 patient=L           "体型 M → L"
    step  2 workspot=free       "站位 → 自由探测器"
    step  3 center=1            "准星对中点亮"
    step  4 follow=1            "跟随点亮"
    step  5 aec=1               "AEC 点亮（DEN 行解锁）"
    step  6 aecfield=left+right "场域：左野 + 右野"
    step  7 el=125              "EI 320 → 125"
    step  8 exp=kvmas_ms        "参数组合 kV−mAs−ms（mA 变灰）"
    step  9 focal=large         "小焦点 → 大焦点"
    step 10 kv=120              "kV 75 → 120"
    step 11 ma=200              "mA 80 → 200（mAs 跟着变）"
    step 12 mas=12.5            "mAs 12.5（反算 ms 62.5→63）"
    step 13 den=3               "DEN 3（需 AEC 亮）"
    step 14 patinfo=open        "患者详情面板展开"
    step 15 sid=150             "读数区 Req.SID 115 → 150"
    step 16 pheight=180         "患者身高 75 → 180"
    step 17 angle=45            "投照角度 110° → 45°"
    step 18 tempa=75            "球管 A 温度 48 → 75（转警示红）"
    step 19 tube=B              "切到球管 B（A 的方块/箭头置灰）"
    step 20 bata=low            "电池 A 低电（转橙）"
    step 21 hu=100              "HU 0% → 100%"
    step 22 "name=Lee, Minjun"  "患者姓名 → Lee, Minjun"
    step 23 "part=Knee AP"      "部位 Chest PA → Knee AP"
    step 24 "deta=off"          "探测器 A 蓝 → 灰（未就绪）"
    step 25 "detb=ready"        "探测器 B 灰 → 蓝（就绪）"
    step 26 "collim=warn"       "准直器 白 → 橙（告警）"
    step 27 "filts=warn"        "滤过图标 白 → 橙（告警）"
    echo
    echo "[28] reset                  回设计稿初值"
    cmd_reset
    echo "演示结束。"
}

usage() {
    cat <<'EOF'
界面调试脚本 dbg.sh —— 改数值看效果，不用重编译

  ./dbg.sh start            启动界面（虚拟机 60% 窗口，Esc 退出）
  ./dbg.sh start full       启动界面（全屏，板子上用）
  ./dbg.sh stop             关掉界面
  ./dbg.sh apply            把脚本顶部「① 数值接口」的值全部推给界面
  ./dbg.sh set kv=120       临时改几个（可写多个：set kv=120 aec=1 den=3）
  ./dbg.sh only focal=large 只保留这一条，其他清空（单变量对照）
  ./dbg.sh show             看界面当前收到的变量
  ./dbg.sh list             列出所有可改的 key 和取值范围
  ./dbg.sh reset            全部回设计稿初值
  ./dbg.sh demo             自动播放一遍（27 步，每步 2 秒）

改数值的两种姿势
  ① 临时改：  ./dbg.sh set kv=120 ms=5
  ② 长期改：  编辑本脚本顶部「① 数值接口」里对应那几行，然后 ./dbg.sh apply

两种都是写进 debug_vars.txt，界面 120ms 内自动刷新；界面没启动也能先写好。
EOF
}

# ---------------------------------------------------------------------------
case "$1" in
    start)  cmd_start "$2" ;;
    stop)   cmd_stop ;;
    apply)  cmd_apply ;;
    set)    shift; cmd_set "$@" ;;
    only|one) shift; cmd_only "$@" ;;
    show)   cmd_show ;;
    list)   cmd_list ;;
    reset)  cmd_reset ;;
    demo)   cmd_demo ;;
    ""|-h|--help|help) usage ;;
    *)      echo "不认识的命令：$1"; echo; usage; exit 1 ;;
esac
