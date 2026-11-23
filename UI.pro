QT       += core gui

greaterThan(QT_MAJOR_VERSION, 4): QT += widgets

# 主控链路（drlink.*）：QWebSocket 当客户端 → 需要 network + websockets。
# 板上 buildroot 默认**没开** QtWebSockets，需先把 libQt5WebSockets.so.5.15.8
# 推到 /usr/lib（见 share/link_qtwebsockets.md）。去掉 --ws 参数时不连网，
# 但仍必须能链上，所以这里不能条件编译。
QT       += network websockets

CONFIG += c++11

# The following define makes your compiler emit warnings if you use
# any Qt feature that has been marked deprecated (the exact warnings
# depend on your compiler). Please consult the documentation of the
# deprecated API in order to know how to port your code away from it.
DEFINES += QT_DEPRECATED_WARNINGS

# You can also make your code fail to compile if it uses deprecated APIs.
# In order to do so, uncomment the following line.
# You can also select to disable deprecated APIs only up to a certain version of Qt.
#DEFINES += QT_DISABLE_DEPRECATED_BEFORE=0x060000    # disables all the APIs deprecated before Qt 6.0.0

SOURCES += \
    main.cpp \
    mainwindow.cpp \
    patientcard.cpp \
    statusstrip.cpp \
    drlink.cpp \
    tabpages.cpp \
    uicards.cpp \
    camsource.cpp

HEADERS += \
    mainwindow.h \
    patientcard.h \
    statusstrip.h \
    drlink.h \
    tabpages.h \
    uicards.h \
    camsource.h

# 摄像头取流：只有板端（交叉编译 aarch64）链 GStreamer。
# host 版没有 gst 开发包，camsource.cpp 走假帧分支，保证虚拟机上的
# --shot / 布局验证仍然能编、能看画面位置。
contains(QT_ARCH, arm64)|contains(QT_ARCH, aarch64) {
    # ⚠ link_pkgconfig 是必须的开关：只写 PKGCONFIG += 而漏了它，
    #   qmake 什么都不做（Makefile 里连 -I/-l 都不会出现），报错就是
    #   「gst/gst.h: 没有那个文件」。而且 qmake 运行时 PATH 里必须有
    #   buildroot 的 pkg-config，见 build_UI.sh。
    CONFIG    += link_pkgconfig
    # gstreamer-video-1.0：读 caps 里的 stride（解码器输出行跨度带对齐填充，
    # 不能想当然用 width*4，否则画面整幅错位）
    PKGCONFIG += gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0
    DEFINES   += HAVE_GST
}

FORMS += \
    mainwindow.ui

# 下拉控件的切图 + 内嵌 Ubuntu 字体（约 354KB，编进可执行文件，不依赖 res.rcc）
RESOURCES += \
    mselect.qrc

# Default rules for deployment.
qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target
TARGET = myui

# 注意：res.qrc 里有 116 张 1200x1920 的 PNG（约 45MB），rcc 转出的 cpp 有 223MB，
# g++ 编译它要吃 6.6GB 内存 → 在 8GB 的虚拟机上直接被 OOM killer 杀掉，
# 留下一个 656 字节的空 .o，资源根本没进二进制（运行时报 Could not create pixmap）。
# 正确做法：用 rcc -binary 生成外部 res.rcc，运行时 QResource::registerResource() 加载。
#RESOURCES += \
#    res.qrc
