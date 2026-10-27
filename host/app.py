"""霍尔编码器上位机。

在工程目录执行：

    python -m host

或：

    python host/app.py
"""

from __future__ import annotations

import csv
import math
import queue
import sys
import time
from collections import deque

import numpy as np
import pyqtgraph as pg
from PySide6.QtCore import QPointF, Qt, QTimer
from PySide6.QtGui import QColor, QFont, QPainter, QPen, QPolygonF
from PySide6.QtWidgets import (
    QApplication,
    QCheckBox,
    QComboBox,
    QDoubleSpinBox,
    QFileDialog,
    QFrame,
    QGridLayout,
    QGroupBox,
    QHBoxLayout,
    QLabel,
    QMainWindow,
    QMessageBox,
    QPushButton,
    QPlainTextEdit,
    QScrollArea,
    QSizePolicy,
    QSpinBox,
    QSplitter,
    QVBoxLayout,
    QWidget,
)

if __package__:
    from . import bus as busmod
else:
    import bus as busmod

pg.setConfigOptions(antialias=False, background="#14171c", foreground="#d5d8de")

CAP = 32768
STYLE = """
QMainWindow { background: #1b1e24; }
#panel, #scroll, QScrollArea { background: #1b1e24; border: none; }
QGroupBox {
    background: #22262e;
    border: 1px solid #343a46;
    border-radius: 8px;
    margin-top: 8px;
    padding: 6px 8px 6px 8px;
    font-weight: 600;
}
QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 4px; color: #9aa3b2; }
QLabel { background: transparent; color: #e6e8eb; }
QLabel#muted { color: #8b93a1; }
QLabel#angle { font-size: 34px; font-weight: 650; color: #f4f7fb; }
QLabel#rpm { font-size: 16px; color: #b6f25c; }
QPushButton {
    background: #2c313b;
    border: 1px solid #434b5a;
    border-radius: 6px;
    padding: 6px 10px;
    color: #e6e8eb;
}
QPushButton:hover { background: #38404c; }
QPushButton:pressed { background: #252a33; }
QPushButton:disabled { color: #6c7380; background: #262b33; }
QPushButton#primary { background: #1f8f62; border: none; font-weight: 600; }
QPushButton#primary:hover { background: #25a571; }
QPushButton#danger { background: #8d3a3a; border: none; }
QComboBox, QSpinBox {
    background: #12151a;
    border: 1px solid #3c4350;
    border-radius: 4px;
    padding: 4px 6px;
    color: #e6e8eb;
    min-height: 22px;
}
QComboBox QAbstractItemView { background: #12151a; color: #e6e8eb; selection-background-color: #1f8f62; }
QCheckBox { color: #d5d8de; spacing: 6px; }
QPlainTextEdit {
    background: #12151a;
    color: #c5cad3;
    border: 1px solid #343a46;
    border-radius: 6px;
    font-family: Consolas, "Cascadia Mono", monospace;
    font-size: 12px;
}
QSplitter::handle { background: #2a303a; }
"""


class Dial(QWidget):
    """0° 在右侧、逆时针增加的角度盘。"""

    def __init__(self) -> None:
        super().__init__()
        self.angle = 0.0
        self.linear = False
        self.setMinimumSize(150, 150)
        self.setMaximumHeight(168)
        self.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Fixed)

    def set_angle(self, degrees: float) -> None:
        self.angle = degrees % 360.0
        self.update()

    def set_linear(self, linear: bool) -> None:
        self.linear = linear
        self.update()

    def paintEvent(self, _event) -> None:
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        side = min(self.width(), self.height()) - 8
        painter.translate(self.width() / 2, self.height() / 2)
        radius = side / 2
        painter.setPen(QPen(QColor("#3c4452"), 8))
        painter.setBrush(QColor("#171b21"))
        painter.drawEllipse(int(-radius), int(-radius), int(radius * 2), int(radius * 2))
        painter.setPen(QPen(QColor("#8b93a1"), 1))
        for mark in range(0, 360, 10):
            painter.save()
            painter.rotate(-mark)
            inner = radius - (14 if mark % 30 == 0 else 8)
            painter.drawLine(int(inner), 0, int(radius - 4), 0)
            painter.restore()
        painter.setPen(QColor("#9aa3b2"))
        font = QFont("Microsoft YaHei UI", 8)
        painter.setFont(font)
        if self.linear:
            labels = ((0, "+20"), (90, "0"), (180, "-20"))
        else:
            labels = tuple((mark, str(mark)) for mark in range(0, 360, 30))
        for mark, text in labels:
            rad = math.radians(mark)
            x = math.cos(rad) * (radius - 28)
            y = -math.sin(rad) * (radius - 28)
            painter.drawText(int(x - 18), int(y - 8), 36, 16, int(Qt.AlignmentFlag.AlignCenter), text)
        painter.setPen(Qt.PenStyle.NoPen)
        painter.setBrush(QColor("#b6f25c"))
        painter.rotate(-self.angle)
        painter.drawPolygon(QPolygonF([
            QPointF(radius - 18, 0),
            QPointF(-18, 7),
            QPointF(-18, -7),
        ]))
        painter.setBrush(QColor("#1b1e24"))
        painter.drawEllipse(-8, -8, 16, 16)


class TraceStore:
    """环形采样缓冲。角度同时保留 0–360 和连续展开。"""

    def __init__(self) -> None:
        self.t = np.zeros(CAP, np.float64)
        self.raw = np.zeros((3, CAP), np.float32)
        self.wrapped = np.zeros(CAP, np.float32)
        self.unwrapped = np.zeros(CAP, np.float64)
        self.i = 0
        self.n = 0
        self.primed = False
        self.last_deg = 0.0
        self.last_t = 0.0
        self.unwrap = 0.0
        self.rpm = 0.0
        self.linear = False
        self.latest: busmod.Sample | None = None
        self.rate_times: deque[float] = deque(maxlen=240)

    def clear(self) -> None:
        self.i = 0
        self.n = 0
        self.primed = False
        self.rpm = 0.0
        self.latest = None
        self.rate_times.clear()

    def add(self, sample: busmod.Sample) -> None:
        if self.linear:
            deg = sample.angle_cdeg / 100.0
            if self.primed:
                dt = sample.t - self.last_t
                if 0.0004 < dt < 0.5:
                    inst = (deg - self.last_deg) / dt
                    self.rpm += 0.2 * (inst - self.rpm)
            self.unwrap = deg
            self.last_deg = deg
            self.last_t = sample.t
            self.primed = True
        else:
            deg = (sample.angle_cdeg % 36000) / 100.0
            if not self.primed:
                self.unwrap = deg
                self.last_deg = deg
                self.last_t = sample.t
                self.primed = True
            else:
                delta = deg - self.last_deg
                if delta > 180.0:
                    delta -= 360.0
                elif delta < -180.0:
                    delta += 360.0
                dt = sample.t - self.last_t
                self.unwrap += delta
                if 0.0004 < dt < 0.5:
                    inst = (delta / 360.0) / dt * 60.0
                    self.rpm += 0.2 * (inst - self.rpm)
                self.last_deg = deg
                self.last_t = sample.t
        slot = self.i
        self.t[slot] = sample.t
        self.raw[0, slot] = sample.raw[0]
        self.raw[1, slot] = sample.raw[1]
        self.raw[2, slot] = sample.raw[2]
        self.wrapped[slot] = deg
        self.unwrapped[slot] = self.unwrap
        self.i = (slot + 1) % CAP
        if self.n < CAP:
            self.n += 1
        self.latest = sample
        self.rate_times.append(sample.t)

    def rate_hz(self) -> float:
        if len(self.rate_times) < 2:
            return 0.0
        span = self.rate_times[-1] - self.rate_times[0]
        if span <= 0.05:
            return 0.0
        return (len(self.rate_times) - 1) / span

    def visible(self, span: float, wrap_angle: bool):
        if self.n == 0:
            return None
        if self.n < CAP:
            t = self.t[:self.n]
            raw = self.raw[:, :self.n]
            wrapped = self.wrapped[:self.n]
            unwrapped = self.unwrapped[:self.n]
        else:
            cut = self.i
            t = np.concatenate((self.t[cut:], self.t[:cut]))
            raw = np.concatenate((self.raw[:, cut:], self.raw[:, :cut]), axis=1)
            wrapped = np.concatenate((self.wrapped[cut:], self.wrapped[:cut]))
            unwrapped = np.concatenate((self.unwrapped[cut:], self.unwrapped[:cut]))
        start = int(np.searchsorted(t, t[-1] - span, side="left"))
        t = t[start:]
        raw = raw[:, start:]
        if wrap_angle:
            angle = wrapped[start:].astype(np.float64, copy=True)
            if angle.size > 1:
                jumps = np.flatnonzero(np.abs(np.diff(angle)) > 180.0)
                angle[jumps] = np.nan
        else:
            angle = unwrapped[start:]
        return t, raw, angle


def style_plot(plot: pg.PlotWidget, ylabel: str) -> None:
    plot.setBackground("#14171c")
    plot.showGrid(x=True, y=True, alpha=0.22)
    label_style = {"color": "#c5c8ce", "font-family": "Microsoft YaHei UI", "font-size": "12px"}
    plot.setLabel("bottom", "时间", units="s", **label_style)
    plot.setLabel("left", ylabel, **label_style)
    tick_font = QFont("Microsoft YaHei UI", 9)
    plot.showAxis("right", False)
    plot.setMouseEnabled(x=True, y=True)
    for name in ("left", "bottom"):
        axis = plot.getAxis(name)
        axis.setPen(pg.mkPen("#5c6370"))
        axis.setTextPen(pg.mkPen("#c5c8ce"))
        axis.setTickFont(tick_font)
    plot.getViewBox().setDefaultPadding(0.0)


class MainWindow(QMainWindow):
    def __init__(self) -> None:
        super().__init__()
        self.setWindowTitle("霍尔传感器上位机")
        self.resize(1280, 780)
        self.bus = busmod.Bus()
        self.bus.start()
        self.store = TraceStore()
        self.connected = False
        self.kind = busmod.TYPE_ENCODER
        self.streaming = False
        self.demo_on = False
        self.demo_theta = 0.0
        self.demo_next = 0.0
        self.csv_file = None
        self.csv_writer = None
        self._fps_n = 0
        self._fps_t = time.perf_counter()
        self._fps = 0.0
        self._dirty = False
        self._programmatic_range = False
        self._wrap_applied = False
        self._build()
        self.timer = QTimer(self)
        self.timer.setTimerType(Qt.TimerType.PreciseTimer)
        self.timer.timeout.connect(self._on_tick)
        self.timer.start(16)
        self._refresh_ports()
        self._sync_buttons()

    def _build(self) -> None:
        central = QWidget()
        self.setCentralWidget(central)
        root = QHBoxLayout(central)
        root.setContentsMargins(10, 10, 10, 10)
        root.setSpacing(10)
        root.addWidget(self._build_panel())
        root.addWidget(self._build_plots(), stretch=1)

    def _build_panel(self) -> QWidget:
        panel = QWidget()
        panel.setObjectName("panel")
        panel.setFixedWidth(320)
        layout = QVBoxLayout(panel)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(8)

        link = QGroupBox("连接")
        grid = QGridLayout(link)
        self.port = QComboBox()
        self.btn_refresh = QPushButton("刷新")
        self.btn_refresh.clicked.connect(self._refresh_ports)
        self.baud = QComboBox()
        for rate in busmod.BAUD_TABLE:
            self.baud.addItem(str(rate), rate)
        self.baud.setCurrentIndex(4)
        self.dev_id = QSpinBox()
        self.dev_id.setRange(1, 254)
        self.dev_id.setValue(1)
        self.dev_type = QComboBox()
        self.dev_type.addItem("不限", busmod.TYPE_ANY)
        self.dev_type.addItem("角度编码器", busmod.TYPE_ENCODER)
        self.dev_type.addItem("直线位置", busmod.TYPE_POSITION)
        self.btn_link = QPushButton("连接")
        self.btn_link.setObjectName("primary")
        self.btn_link.clicked.connect(self._toggle_link)
        grid.addWidget(QLabel("串口"), 0, 0)
        grid.addWidget(self.port, 0, 1)
        grid.addWidget(self.btn_refresh, 0, 2)
        grid.addWidget(QLabel("波特率"), 1, 0)
        grid.addWidget(self.baud, 1, 1, 1, 2)
        grid.addWidget(QLabel("设备号"), 2, 0)
        grid.addWidget(self.dev_id, 2, 1, 1, 2)
        grid.addWidget(QLabel("类型"), 3, 0)
        grid.addWidget(self.dev_type, 3, 1, 1, 2)
        grid.addWidget(self.btn_link, 4, 0, 1, 3)
        hint = QLabel("USB 转 485 使用自动收发即可")
        hint.setObjectName("muted")
        grid.addWidget(hint, 5, 0, 1, 3)
        layout.addWidget(link)

        live = QGroupBox("实时")
        live_layout = QVBoxLayout(live)
        self.dial = Dial()
        self.angle_label = QLabel("0.00°")
        self.angle_label.setObjectName("angle")
        self.angle_label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.rpm_label = QLabel("0.0 RPM")
        self.rpm_label.setObjectName("rpm")
        self.rpm_label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.detail = QLabel("幅度 —    U1 —  U2 —  U4 —")
        self.detail.setWordWrap(True)
        self.detail.setObjectName("muted")
        self.flags = QLabel("未连接")
        self.flags.setObjectName("muted")
        live_layout.addWidget(self.dial)
        live_layout.addWidget(self.angle_label)
        live_layout.addWidget(self.rpm_label)
        live_layout.addWidget(self.detail)
        live_layout.addWidget(self.flags)
        layout.addWidget(live)

        cap = QGroupBox("采集")
        cap_grid = QGridLayout(cap)
        self.stream_mode = QComboBox()
        self.stream_mode.addItem("主从查询", "poll")
        self.stream_mode.addItem("主动上报", "report")
        self.period = QSpinBox()
        self.period.setRange(0, 1000)
        self.period.setValue(10)
        self.period.setSuffix(" ms")
        self.btn_stream = QPushButton("开始采集")
        self.btn_stream.setObjectName("primary")
        self.btn_stream.clicked.connect(self._toggle_stream)
        self.follow = QCheckBox("跟随最新")
        self.follow.setChecked(True)
        self.span = QComboBox()
        for seconds in (2, 5, 10, 30):
            self.span.addItem(f"{seconds} 秒", seconds)
        self.span.setCurrentIndex(2)
        self.wrap = QCheckBox("角度按 0–360 显示")
        self.demo = QCheckBox("无设备演示")
        self.demo_rpm = QSpinBox()
        self.demo_rpm.setRange(0, 3000)
        self.demo_rpm.setValue(60)
        self.demo_rpm.setSuffix(" RPM")
        self.btn_clear = QPushButton("清空波形")
        self.btn_clear.clicked.connect(self._clear_plot)
        self.btn_csv = QPushButton("开始录制")
        self.btn_csv.clicked.connect(self._toggle_csv)
        cap_grid.addWidget(self.stream_mode, 0, 0, 1, 2)
        cap_grid.addWidget(QLabel("周期"), 1, 0)
        cap_grid.addWidget(self.period, 1, 1)
        cap_grid.addWidget(self.btn_stream, 2, 0, 1, 2)
        cap_grid.addWidget(self.follow, 3, 0)
        cap_grid.addWidget(self.span, 3, 1)
        cap_grid.addWidget(self.wrap, 4, 0, 1, 2)
        cap_grid.addWidget(self.demo, 5, 0)
        cap_grid.addWidget(self.demo_rpm, 5, 1)
        cap_grid.addWidget(self.btn_clear, 6, 0)
        cap_grid.addWidget(self.btn_csv, 6, 1)
        note = QLabel("主从周期 0 表示尽快查询。主动上报最小 5 ms。")
        note.setWordWrap(True)
        note.setObjectName("muted")
        cap_grid.addWidget(note, 7, 0, 1, 2)
        layout.addWidget(cap)

        cal = QGroupBox("模式、零点与校准")
        cal_layout = QGridLayout(cal)
        self.btn_profile = QPushButton("切换为直线位置")
        self.btn_profile.clicked.connect(self._toggle_profile)
        self.sigma = QDoubleSpinBox()
        self.sigma.setRange(2.0, 30.0)
        self.sigma.setSingleStep(0.5)
        self.sigma.setDecimals(1)
        self.sigma.setValue(8.0)
        self.sigma.setSuffix(" mm")
        self.btn_sigma = QPushButton("写宽度")
        self.btn_sigma.clicked.connect(self._write_sigma)
        self.btn_zero = QPushButton("当前位置设为 0°")
        self.btn_zero.clicked.connect(lambda: self.bus.submit("zero"))
        self.btn_clear_zero = QPushButton("清除零点")
        self.btn_clear_zero.clicked.connect(lambda: self.bus.submit("clear_zero"))
        self.btn_cal_begin = QPushButton("开始校准")
        self.btn_cal_begin.clicked.connect(self._cal_begin)
        self.btn_cal_end = QPushButton("结束校准")
        self.btn_cal_end.clicked.connect(lambda: self.bus.submit("cal_end"))
        self.btn_cal_clear = QPushButton("清除校准")
        self.btn_cal_clear.clicked.connect(lambda: self.bus.submit("cal_clear"))
        self.cal_label = QLabel("角度模式先转满一圈。直线模式先把磁铁拿开，再采基线。")
        self.cal_label.setWordWrap(True)
        self.cal_label.setObjectName("muted")
        cal_layout.addWidget(self.btn_profile, 0, 0, 1, 2)
        cal_layout.addWidget(self.sigma, 1, 0)
        cal_layout.addWidget(self.btn_sigma, 1, 1)
        cal_layout.addWidget(self.btn_zero, 2, 0, 1, 2)
        cal_layout.addWidget(self.btn_clear_zero, 3, 0)
        cal_layout.addWidget(self.btn_cal_clear, 3, 1)
        cal_layout.addWidget(self.btn_cal_begin, 4, 0)
        cal_layout.addWidget(self.btn_cal_end, 4, 1)
        cal_layout.addWidget(self.cal_label, 5, 0, 1, 2)
        layout.addWidget(cal)

        device = QGroupBox("设备参数")
        dev_grid = QGridLayout(device)
        self.new_id = QSpinBox()
        self.new_id.setRange(1, 254)
        self.new_id.setValue(1)
        self.btn_id = QPushButton("写设备号")
        self.btn_id.clicked.connect(lambda: self.bus.submit("set_id", self.new_id.value()))
        self.new_baud = QComboBox()
        for rate in busmod.BAUD_TABLE:
            self.new_baud.addItem(str(rate), rate)
        self.new_baud.setCurrentIndex(4)
        self.btn_baud = QPushButton("写波特率")
        self.btn_baud.clicked.connect(lambda: self.bus.submit("set_baud", self.new_baud.currentIndex()))
        self.btn_info = QPushButton("读信息")
        self.btn_info.clicked.connect(lambda: self.bus.submit("info"))
        self.btn_factory = QPushButton("恢复出厂")
        self.btn_factory.setObjectName("danger")
        self.btn_factory.clicked.connect(self._factory)
        dev_grid.addWidget(self.new_id, 0, 0)
        dev_grid.addWidget(self.btn_id, 0, 1)
        dev_grid.addWidget(self.new_baud, 1, 0)
        dev_grid.addWidget(self.btn_baud, 1, 1)
        dev_grid.addWidget(self.btn_info, 2, 0)
        dev_grid.addWidget(self.btn_factory, 2, 1)
        layout.addWidget(device)
        layout.addStretch(1)

        scroll = QScrollArea()
        scroll.setObjectName("scroll")
        scroll.setWidgetResizable(True)
        scroll.setWidget(panel)
        scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        scroll.setFixedWidth(338)
        return scroll

    def _build_plots(self) -> QWidget:
        wrap = QWidget()
        layout = QVBoxLayout(wrap)
        layout.setContentsMargins(0, 0, 0, 0)
        splitter = QSplitter(Qt.Orientation.Vertical)
        self.plot_adc = pg.PlotWidget()
        self.plot_angle = pg.PlotWidget()
        style_plot(self.plot_adc, "霍尔原码")
        style_plot(self.plot_angle, "角度")
        self.plot_angle.setXLink(self.plot_adc)
        legend = self.plot_adc.addLegend(offset=(8, 8))
        legend.setBrush(pg.mkBrush(20, 24, 30, 190))
        legend.setPen(pg.mkPen("#3c424d"))
        pens = (("#3ec6ff", "U1"), ("#ffb020", "U2"), ("#ff5d73", "U4"))
        self.curves = []
        for color, name in pens:
            curve = self.plot_adc.plot(pen=pg.mkPen(color, width=1.6), name=name)
            curve.setClipToView(True)
            curve.setDownsampling(auto=True, method="peak")
            self.curves.append(curve)
        self.angle_curve = self.plot_angle.plot(pen=pg.mkPen("#b6f25c", width=1.8), name="角度")
        self.angle_curve.setClipToView(True)
        self.angle_curve.setDownsampling(auto=True, method="peak")
        self.plot_adc.getViewBox().enableAutoRange(x=False, y=False)
        self.plot_adc.setYRange(0, 4095)
        self.plot_angle.getViewBox().enableAutoRange(x=False, y=True)
        self.plot_adc.getViewBox().sigRangeChangedManually.connect(self._user_panned)
        self.plot_angle.getViewBox().sigRangeChangedManually.connect(self._user_panned)
        splitter.addWidget(self.plot_adc)
        splitter.addWidget(self.plot_angle)
        splitter.setStretchFactor(0, 3)
        splitter.setStretchFactor(1, 2)
        layout.addWidget(splitter, stretch=1)
        self.log = QPlainTextEdit()
        self.log.setReadOnly(True)
        self.log.setMaximumHeight(120)
        self.log.setPlaceholderText("通信记录")
        layout.addWidget(self.log)
        self.status = QLabel("就绪")
        self.status.setObjectName("muted")
        layout.addWidget(self.status)
        return wrap

    def _refresh_ports(self) -> None:
        current = self.port.currentData()
        self.port.clear()
        ports = busmod.list_serial_ports()
        if not ports:
            self.port.addItem("没有串口", "")
            return
        for device, label in ports:
            self.port.addItem(label, device)
            if device == current:
                self.port.setCurrentIndex(self.port.count() - 1)

    def _toggle_link(self) -> None:
        if self.connected:
            self.bus.submit("disconnect")
            return
        port = self.port.currentData()
        if not port:
            self._log("没有可用串口")
            return
        self.bus.submit("connect", port, int(self.baud.currentData()), self.dev_id.value(), int(self.dev_type.currentData()))

    def _toggle_stream(self) -> None:
        if self.streaming or self.demo_on:
            if self.demo_on:
                self.demo_on = False
                self.store.linear = self.kind == busmod.TYPE_POSITION
                self.store.clear()
                self._dirty = True
                self._sync_buttons()
                self._log("停止演示")
            if self.streaming:
                self.bus.submit("stream", "off", 0)
            return
        self.store.clear()
        self._dirty = True
        if self.demo.isChecked():
            self.demo_on = True
            self.store.linear = False
            self.demo_theta = 0.0
            self.demo_next = time.perf_counter()
            self._sync_buttons()
            self._log("演示波形")
            return
        if not self.connected:
            self._log("先连接串口，或勾选无设备演示")
            return
        mode = self.stream_mode.currentData()
        self.bus.submit("stream", mode, self.period.value())

    def _toggle_profile(self) -> None:
        target = busmod.TYPE_ENCODER if self.kind == busmod.TYPE_POSITION else busmod.TYPE_POSITION
        if target == busmod.TYPE_POSITION:
            self._log("切换后把磁铁拿开，点开始校准再结束，采集零磁场基线。")
        self.bus.submit("set_profile", target)

    def _write_sigma(self) -> None:
        self.bus.submit("set_sigma", int(round(self.sigma.value() * 1000)))

    def _cal_begin(self) -> None:
        if self.kind == busmod.TYPE_POSITION:
            self.cal_label.setText("正在采集零磁场基线。确认磁铁已经拿开，再点结束校准。")
        else:
            self.cal_label.setText("正在记录。请把磁铁匀速转满至少一圈，再点结束校准。")
        self.bus.submit("cal_begin")

    def _factory(self) -> None:
        answer = QMessageBox.question(self, "恢复出厂", "设备号、波特率、零点和校准都会恢复成出厂值。")
        if answer == QMessageBox.StandardButton.Yes:
            self.bus.submit("factory")

    def _clear_plot(self) -> None:
        self.store.clear()
        self._dirty = True
        self._draw()

    def _toggle_csv(self) -> None:
        if self.csv_file is not None:
            self._close_csv()
            return
        path, _ = QFileDialog.getSaveFileName(self, "保存波形", "hall.csv", "CSV (*.csv)")
        if not path:
            return
        self.csv_file = open(path, "w", newline="", encoding="utf-8")
        self.csv_writer = csv.writer(self.csv_file)
        if self.store.linear:
            header = ["t_s", "position_mm", "amplitude", "u1", "u2", "u4", "flags", "mm_s"]
        else:
            header = ["t_s", "angle_deg", "amplitude", "u1", "u2", "u4", "flags", "rpm"]
        self.csv_writer.writerow(header)
        self.btn_csv.setText("停止录制")
        self._log(f"正在写入 {path}")

    def _close_csv(self) -> None:
        if self.csv_file is not None:
            self.csv_file.close()
        self.csv_file = None
        self.csv_writer = None
        self.btn_csv.setText("开始录制")
        self._log("录制已停止")

    def _user_panned(self) -> None:
        if not self._programmatic_range:
            self.follow.setChecked(False)

    def _on_tick(self) -> None:
        self._drain_events()
        got = self._drain_samples()
        if self.demo_on:
            got = self._produce_demo() or got
        if self.csv_file is not None and got:
            self.csv_file.flush()
        if got or self._dirty:
            self._draw()
            self._dirty = False
        self._fps_n += 1
        now = time.perf_counter()
        if now - self._fps_t >= 0.5:
            self._fps = self._fps_n / (now - self._fps_t)
            self._fps_n = 0
            self._fps_t = now
            self._update_status()

    def _drain_events(self) -> None:
        while True:
            try:
                event = self.bus.events.get_nowait()
            except queue.Empty:
                return
            kind = event[0]
            if kind == "log":
                self._log(str(event[1]))
            elif kind == "connected":
                self.connected = bool(event[1])
                self.btn_link.setText("断开" if self.connected else "连接")
                if not self.connected:
                    self.streaming = False
                    self.flags.setText("未连接")
                self._sync_buttons()
            elif kind == "streaming":
                self.streaming = bool(event[1])
                self._sync_buttons()
            elif kind == "info":
                self._show_info(event[1])
            elif kind == "id":
                self.dev_id.setValue(int(event[1]))
                self.new_id.setValue(int(event[1]))
            elif kind == "baud":
                self._select_baud(int(event[1]))
            elif kind == "mode":
                self.stream_mode.setCurrentIndex(1 if int(event[1]) == busmod.MODE_REPORT else 0)
                self.period.setValue(int(event[2]))
            elif kind == "zero":
                zero = int(event[1]) / 100.0
                if self.kind == busmod.TYPE_POSITION:
                    self.cal_label.setText(f"机械零点偏移 {zero:.2f} mm")
                else:
                    self.cal_label.setText(f"机械零点偏移 {zero:.2f}°")
            elif kind == "sigma":
                self.sigma.setValue(int(event[1]) / 1000.0)
            elif kind == "cal":
                offsets, gains = event[1], event[2]
                flags = int(event[3]) if len(event) > 3 else 0
                if self.kind == busmod.TYPE_POSITION:
                    self.sigma.setValue(gains[0] / 1000.0)
                    if flags & 2:
                        off_text = " / ".join(str(v) for v in offsets)
                        self.cal_label.setText(f"基线 {off_text}\n敏感宽度 {gains[0] / 1000:.1f} mm")
                    else:
                        self.cal_label.setText(f"尚未采集基线，当前按 2048 估算\n敏感宽度 {gains[0] / 1000:.1f} mm")
                else:
                    gain_text = " / ".join(f"{g / 4096:.3f}" for g in gains)
                    off_text = " / ".join(str(v) for v in offsets)
                    self.cal_label.setText(f"偏置 {off_text}\n增益 {gain_text}")
            elif kind == "factory":
                self.dev_id.setValue(int(event[1]))
                self.new_id.setValue(int(event[1]))
                self._select_baud(int(event[2]))
                self.stream_mode.setCurrentIndex(0)
                self.period.setValue(20)

    def _apply_kind(self, dev_type: int) -> None:
        linear = dev_type == busmod.TYPE_POSITION
        if linear != self.store.linear and not self.demo_on:
            self.store.linear = linear
            self.store.clear()
            self._dirty = True
            self._wrap_applied = not self.wrap.isChecked()
        self.kind = dev_type if dev_type in (busmod.TYPE_ENCODER, busmod.TYPE_POSITION) else busmod.TYPE_ENCODER
        self.dial.set_linear(linear and not self.demo_on)
        self.btn_profile.setText("切换为角度编码器" if linear else "切换为直线位置")
        self.btn_zero.setText("当前位置设为 0 mm" if linear else "当前位置设为 0°")
        self.wrap.setEnabled(not linear)
        style = {"color": "#c5c8ce", "font-family": "Microsoft YaHei UI", "font-size": "12px"}
        if linear:
            self.plot_angle.setLabel("left", "位置", units="mm", **style)
        else:
            self.plot_angle.setLabel("left", "角度", units="°", **style)

    def _show_info(self, info: busmod.DeviceInfo) -> None:
        self.dev_id.setValue(info.dev_id)
        self.new_id.setValue(info.dev_id)
        if 0 <= info.baud_index < len(busmod.BAUD_TABLE):
            self._select_baud(busmod.BAUD_TABLE[info.baud_index])
        self.stream_mode.setCurrentIndex(1 if info.mode == busmod.MODE_REPORT else 0)
        self.period.setValue(info.report_ms)
        self._apply_kind(info.dev_type)
        mode = "主动上报" if info.mode == busmod.MODE_REPORT else "主从"
        cal = "已校准" if info.cal_valid else "未校准"
        if info.dev_type == busmod.TYPE_POSITION:
            name = "直线位置"
            zero = f"{info.zero_cdeg / 100:.2f} mm"
        else:
            name = "角度编码器"
            zero = f"{info.zero_cdeg / 100:.2f}°"
        self.flags.setText(f"{name}   固件 {info.fw >> 8}.{info.fw & 0xFF}   {mode}   {cal}   零点 {zero}")
        self._sync_buttons()

    def _select_baud(self, baud: int) -> None:
        index = busmod.BAUD_TABLE.index(baud) if baud in busmod.BAUD_TABLE else 4
        self.baud.setCurrentIndex(index)
        self.new_baud.setCurrentIndex(index)

    def _drain_samples(self) -> bool:
        got = False
        while True:
            try:
                sample = self.bus.samples.get_nowait()
            except queue.Empty:
                break
            self._take_sample(sample)
            got = True
        return got

    def _take_sample(self, sample: busmod.Sample) -> None:
        self.store.add(sample)
        if self.csv_writer is not None:
            if self.store.linear:
                reading = sample.angle_cdeg / 100.0
            else:
                reading = (sample.angle_cdeg % 36000) / 100.0
            self.csv_writer.writerow([
                f"{sample.t:.6f}",
                f"{reading:.2f}",
                sample.amplitude,
                sample.raw[0],
                sample.raw[1],
                sample.raw[2],
                sample.flags,
                f"{self.store.rpm:.2f}",
            ])

    def _produce_demo(self) -> bool:
        now = time.perf_counter()
        if self.demo_next <= 0:
            self.demo_next = now
        produced = False
        rpm = self.demo_rpm.value()
        step = 1.0 / 200.0
        omega = rpm / 60.0 * 2.0 * math.pi
        while self.demo_next <= now:
            theta = self.demo_theta
            amp = 1500.0
            raw = []
            phase = (0.0, -2.0 * math.pi / 3.0, 2.0 * math.pi / 3.0)
            for index, shift in enumerate(phase):
                noise = 1.5 * math.sin(theta * 17 + index)
                raw.append(int(2048 + amp * math.cos(theta + shift) + noise))
            deg = math.degrees(theta) % 360.0
            sample = busmod.Sample(self.demo_next, int(round(deg * 100)), int(amp), (raw[0], raw[1], raw[2]), 0)
            self._take_sample(sample)
            self.demo_theta += omega * step
            self.demo_next += step
            produced = True
            if self.demo_next < now - 0.5:
                self.demo_next = now
                break
        return produced

    def _draw(self) -> None:
        latest = self.store.latest
        linear = self.store.linear and not self.demo_on
        if latest is not None:
            if linear:
                mm = latest.angle_cdeg / 100.0
                clamped = max(-20.0, min(20.0, mm))
                self.dial.set_angle(90.0 - clamped / 20.0 * 90.0)
                self.angle_label.setText(f"{mm:.2f} mm")
                self.rpm_label.setText(f"{self.store.rpm:+.1f} mm/s")
            else:
                deg = (latest.angle_cdeg % 36000) / 100.0
                self.dial.set_angle(deg)
                self.angle_label.setText(f"{deg:.2f}°")
                self.rpm_label.setText(f"{self.store.rpm:+.1f} RPM")
            weak = bool(latest.flags & 1)
            self.angle_label.setStyleSheet("color: #ff6b6b;" if weak else "color: #f4f7fb;")
            self.detail.setText(
                f"幅度 {latest.amplitude}    U1 {latest.raw[0]}    U2 {latest.raw[1]}    U4 {latest.raw[2]}"
            )
            parts = []
            if latest.flags & 1:
                parts.append("磁场弱")
            if latest.flags & 2:
                parts.append("已校准")
            if latest.flags & 4:
                parts.append("校准中")
            if latest.flags & 8:
                parts.append("超出阵列")
            if parts:
                self.flags.setText("  ".join(parts))
        view = self.store.visible(float(self.span.currentData()), self.wrap.isChecked() and not linear)
        if view is None:
            for curve in self.curves:
                curve.setData([], [])
            self.angle_curve.setData([], [])
            return
        t, raw, angle = view
        for index, curve in enumerate(self.curves):
            curve.setData(t, raw[index], connect="all", skipFiniteCheck=True)
        wrap = self.wrap.isChecked()
        if wrap != self._wrap_applied:
            self._wrap_applied = wrap
            if wrap:
                self.plot_angle.getViewBox().enableAutoRange(y=False)
                self.plot_angle.setYRange(-10, 370, padding=0)
            else:
                self.plot_angle.getViewBox().enableAutoRange(y=True)
        connect = "finite" if wrap else "all"
        self.angle_curve.setData(t, angle, connect=connect, skipFiniteCheck=not wrap)
        if self.follow.isChecked() and t.size:
            self._programmatic_range = True
            end = float(t[-1])
            span = float(self.span.currentData())
            self.plot_adc.setXRange(end - span, end + span * 0.02, padding=0)
            self._programmatic_range = False
        self._update_status()

    def _update_status(self) -> None:
        link = "已连接" if self.connected else "未连接"
        run = "演示" if self.demo_on else ("采集中" if self.streaming else "停止")
        self.status.setText(
            f"{link}   {run}   采样 {self.store.rate_hz():.0f} Hz   画面 {self._fps:.0f} fps   丢弃 {self.bus.dropped}"
        )

    def _sync_buttons(self) -> None:
        running = self.streaming or self.demo_on
        self.btn_stream.setText("停止采集" if running else "开始采集")
        online = self.connected and not self.demo_on
        for button in (
            self.btn_profile, self.btn_sigma, self.btn_zero, self.btn_clear_zero,
            self.btn_cal_begin, self.btn_cal_end, self.btn_cal_clear,
            self.btn_id, self.btn_baud, self.btn_info, self.btn_factory,
        ):
            button.setEnabled(online)
        self.sigma.setEnabled(online and self.kind == busmod.TYPE_POSITION)
        self.btn_sigma.setEnabled(online and self.kind == busmod.TYPE_POSITION)
        self.port.setEnabled(not self.connected)
        self.baud.setEnabled(not self.connected)
        self.btn_refresh.setEnabled(not self.connected)

    def _log(self, text: str) -> None:
        stamp = time.strftime("%H:%M:%S")
        bar = self.log.verticalScrollBar()
        follow = bar.value() >= bar.maximum() - 4
        self.log.appendPlainText(f"{stamp}  {text}")
        if self.log.document().blockCount() > 200:
            cursor = self.log.textCursor()
            cursor.movePosition(cursor.MoveOperation.Start)
            cursor.select(cursor.SelectionType.BlockUnderCursor)
            cursor.removeSelectedText()
            cursor.deleteChar()
        if follow:
            bar.setValue(bar.maximum())

    def closeEvent(self, event) -> None:
        self._close_csv()
        self.bus.stop()
        event.accept()


def main() -> None:
    app = QApplication(sys.argv)
    app.setStyle("Fusion")
    app.setFont(QFont("Microsoft YaHei UI", 10))
    app.setStyleSheet(STYLE)
    window = MainWindow()
    window.show()
    sys.exit(app.exec())


if __name__ == "__main__":
    main()
