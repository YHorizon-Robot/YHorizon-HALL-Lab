"""霍尔板 RS485 主机协议和串口线程。

帧格式与固件 inc/rs485_proto.h 一致：A5 5A、小端、CRC-16/MODBUS。
"""

from __future__ import annotations

import queue
import struct
import threading
import time
from dataclasses import dataclass

import serial
from serial.tools import list_ports

SOF = b"\xA5\x5A"
HOST = 0x00
TYPE_ENCODER = 0x01
TYPE_POSITION = 0x02
TYPE_ANY = 0xFF
PAYLOAD_MAX = 32

CMD_GET_INFO = 0x01
CMD_SET_ID = 0x02
CMD_SET_BAUD = 0x03
CMD_SET_MODE = 0x04
CMD_SAVE = 0x05
CMD_FACTORY = 0x06
CMD_SET_PROFILE = 0x07
CMD_GET_DATA = 0x10
CMD_SET_ZERO = 0x21
CMD_CLEAR_ZERO = 0x22
CMD_CAL_BEGIN = 0x23
CMD_CAL_END = 0x24
CMD_CAL_CLEAR = 0x25
CMD_GET_CAL = 0x26
CMD_SET_SIGMA = 0x27
CMD_REPORT = 0x30
CMD_RESPONSE = 0x80

MODE_POLL = 0
MODE_REPORT = 1

FLAG_WEAK = 0x01
FLAG_CAL_VALID = 0x02
FLAG_CAL_RUN = 0x04
FLAG_OUT = 0x08
FLAG_MAGNET = 0x10
FLAG_SAT = 0x20
# 固件在没有确认磁铁时把 int32 固定成这个值
NO_MAGNET = -0x80000000

BAUD_TABLE = (9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600)

STATUS_TEXT = {
    0x00: "成功",
    0x01: "长度不对",
    0x02: "参数超出范围",
    0x03: "当前状态不允许（设零点需要先确认有磁铁）",
    0x04: "校准数据不够。直线模式请把磁铁拿开后再结束",
    0x05: "Flash 写入失败",
    0x06: "不认识的命令",
}


def crc16(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            if crc & 1:
                crc = (crc >> 1) ^ 0xA001
            else:
                crc >>= 1
            crc &= 0xFFFF
    return crc


def build_frame(dst: int, src: int, dev_type: int, cmd: int, seq: int, payload: bytes = b"") -> bytes:
    if len(payload) > PAYLOAD_MAX:
        raise ValueError("payload too long")
    body = bytes((dst & 0xFF, src & 0xFF, dev_type & 0xFF, cmd & 0xFF, seq & 0xFF, len(payload))) + payload
    crc = crc16(body)
    return SOF + body + bytes((crc & 0xFF, (crc >> 8) & 0xFF))


@dataclass
class Frame:
    dst: int
    src: int
    dev_type: int
    cmd: int
    seq: int
    payload: bytes


@dataclass
class Sample:
    t: float
    angle_cdeg: int
    amplitude: int
    raw: tuple[int, int, int]
    flags: int

    @property
    def has_magnet(self) -> bool:
        """只有固件确认有磁铁，angle_cdeg 才是真实读数。"""
        return self.angle_cdeg != NO_MAGNET and bool(self.flags & FLAG_MAGNET)


@dataclass
class DeviceInfo:
    dev_type: int
    dev_id: int
    mode: int
    baud_index: int
    report_ms: int
    fw: int
    cal_valid: int
    flags: int
    zero_cdeg: int


class Parser:
    """从字节流里切出完整帧，校验失败时丢掉一个字节再同步。"""

    def __init__(self) -> None:
        self._buf = bytearray()

    def reset(self) -> None:
        self._buf.clear()

    def feed(self, data: bytes) -> list[Frame]:
        self._buf.extend(data)
        frames: list[Frame] = []
        while True:
            frame = self._pop()
            if frame is None:
                break
            frames.append(frame)
        return frames

    def _pop(self) -> Frame | None:
        buf = self._buf
        while True:
            if len(buf) < 10:
                return None
            if buf[0] != 0xA5 or buf[1] != 0x5A:
                del buf[0]
                continue
            plen = buf[7]
            if plen > PAYLOAD_MAX:
                del buf[0]
                continue
            total = 10 + plen
            if len(buf) < total:
                return None
            body = bytes(buf[2:8 + plen])
            got = buf[8 + plen] | (buf[9 + plen] << 8)
            if crc16(body) != got:
                del buf[0]
                continue
            frame = Frame(buf[2], buf[3], buf[4], buf[5], buf[6], bytes(buf[8:8 + plen]))
            del buf[:total]
            return frame


def status_text(code: int) -> str:
    return STATUS_TEXT.get(code, f"状态 {code}")


def parse_sample(payload: bytes, with_status: bool) -> tuple[str, int | Sample]:
    if with_status:
        if len(payload) < 1:
            return "bad", 0
        if payload[0] != 0:
            return "err", payload[0]
        payload = payload[1:]
    if len(payload) < 13:
        return "bad", 0
    angle, amp, u1, u2, u4, flags = struct.unpack_from("<ihHHHB", payload)
    sample = Sample(time.perf_counter(), angle, amp, (u1, u2, u4), flags)
    return "ok", sample


def parse_info(payload: bytes) -> tuple[str, int | DeviceInfo]:
    if len(payload) < 1:
        return "bad", 0
    if payload[0] != 0:
        return "err", payload[0]
    if len(payload) < 13:
        return "bad", 0
    report_ms, fw = struct.unpack_from("<HH", payload, 5)
    if payload[1] == TYPE_POSITION:
        zero = struct.unpack_from("<h", payload, 11)[0]
    else:
        zero = struct.unpack_from("<H", payload, 11)[0]
    info = DeviceInfo(
        dev_type=payload[1],
        dev_id=payload[2],
        mode=payload[3],
        baud_index=payload[4],
        report_ms=report_ms,
        fw=fw,
        cal_valid=payload[9],
        flags=payload[10],
        zero_cdeg=zero,
    )
    return "ok", info


def list_serial_ports() -> list[tuple[str, str]]:
    found = []
    for port in list_ports.comports():
        label = port.device
        if port.description:
            label = f"{port.device}  {port.description}"
        found.append((port.device, label))
    return found


class Bus:
    """独占串口的后台线程。界面只投递命令、取走采样和事件。"""

    def __init__(self) -> None:
        self.samples: queue.Queue[Sample] = queue.Queue(maxsize=8000)
        self.events: queue.Queue[tuple] = queue.Queue(maxsize=200)
        self.dropped = 0
        self._cmds: queue.Queue[tuple] = queue.Queue()
        self._quit = threading.Event()
        self._thread = threading.Thread(target=self._run, name="hall-rs485", daemon=True)
        self._ser: serial.Serial | None = None
        self._parser = Parser()
        self._seq = 0
        self._dev_id = 1
        self._dev_type = TYPE_ANY
        self._device_kind = TYPE_ENCODER
        self._streaming = False
        self._stream_mode = "poll"
        self._gap_s = 0.01
        self._device_mode: int | None = None
        self._report_ms = 20
        self._last_timeout_log = 0.0
        self._inbox: list[Frame] = []

    def start(self) -> None:
        self._thread.start()

    def stop(self) -> None:
        self._quit.set()
        self._cmds.put(("nop",))
        self._thread.join(timeout=1.5)
        ser = self._ser
        self._ser = None
        if ser is not None and ser.is_open:
            ser.close()

    def submit(self, *cmd: object) -> None:
        self._cmds.put(cmd)

    def offer_sample(self, sample: Sample) -> None:
        try:
            self.samples.put_nowait(sample)
        except queue.Full:
            self.dropped += 1

    def _emit(self, *event: object) -> None:
        try:
            self.events.put_nowait(event)
        except queue.Full:
            pass

    def _run(self) -> None:
        while not self._quit.is_set():
            try:
                self._drain_cmds()
                if self._quit.is_set():
                    break
                if self._ser is None:
                    self._quit.wait(0.05)
                    continue
                if self._streaming and self._stream_mode == "poll":
                    self._poll_once()
                    if self._gap_s > 0:
                        self._quit.wait(self._gap_s)
                else:
                    self._pump(0.05)
            except (serial.SerialException, OSError) as exc:
                self._fail_link(str(exc))

    def _drain_cmds(self) -> None:
        while True:
            try:
                cmd = self._cmds.get_nowait()
            except queue.Empty:
                return
            self._exec(cmd)

    def _exec(self, cmd: tuple) -> None:
        name = cmd[0]
        if name == "nop":
            return
        if name == "connect":
            self._connect(str(cmd[1]), int(cmd[2]), int(cmd[3]), int(cmd[4]))
            return
        if name == "disconnect":
            self._close()
            self._emit("connected", False)
            return
        if self._ser is None:
            self._emit("log", "还没有连接串口")
            return
        if name == "stream":
            self._set_stream(str(cmd[1]), int(cmd[2]))
            return
        if name == "info":
            self._read_info()
            return
        if name == "zero":
            self._checked("设零点", self._transact(CMD_SET_ZERO, b"", 0.4), self._after_zero)
            return
        if name == "clear_zero":
            self._checked("清除零点", self._transact(CMD_CLEAR_ZERO, b"", 0.4), None)
            return
        if name == "cal_begin":
            self._checked("开始校准", self._transact(CMD_CAL_BEGIN, b"", 0.3), None)
            return
        if name == "cal_end":
            self._checked("结束校准", self._transact(CMD_CAL_END, b"", 0.4), self._after_cal)
            return
        if name == "cal_clear":
            self._checked("清除校准", self._transact(CMD_CAL_CLEAR, b"", 0.4), None)
            return
        if name == "set_id":
            self._set_id(int(cmd[1]))
            return
        if name == "set_baud":
            self._set_baud(int(cmd[1]))
            return
        if name == "factory":
            self._factory()
            return
        if name == "set_profile":
            self._set_profile(int(cmd[1]))
            return
        if name == "set_sigma":
            self._set_sigma(int(cmd[1]))
            return
        if name == "set_mode":
            self._apply_mode(int(cmd[1]), int(cmd[2]), quiet=False)

    def _connect(self, port: str, baud: int, dev_id: int, dev_type: int) -> None:
        self._close()
        self._dev_id = dev_id
        self._dev_type = dev_type
        self._streaming = False
        try:
            self._ser = serial.Serial(port, baudrate=baud, bytesize=8, parity="N", stopbits=1, timeout=0.02)
        except serial.SerialException as exc:
            self._emit("log", f"打开 {port} 失败：{exc}")
            self._emit("connected", False)
            return
        self._parser.reset()
        self._inbox.clear()
        self._emit("connected", True)
        self._emit("log", f"已连接 {port}  {baud} 8N1，设备号 {dev_id}")
        self._read_info()

    def _close(self) -> None:
        self._streaming = False
        ser = self._ser
        self._ser = None
        if ser is not None and ser.is_open:
            ser.close()

    def _fail_link(self, message: str) -> None:
        self._emit("log", f"串口中断：{message}")
        self._close()
        self._emit("connected", False)

    def _next_seq(self) -> int:
        self._seq = (self._seq + 1) & 0xFF
        return self._seq

    def _transact(self, cmd: int, payload: bytes, timeout: float) -> Frame | None:
        ser = self._ser
        if ser is None:
            return None
        seq = self._next_seq()
        frame = build_frame(self._dev_id, HOST, self._dev_type, cmd, seq, payload)
        ser.reset_input_buffer()
        self._parser.reset()
        self._inbox.clear()
        ser.write(frame)
        ser.flush()
        deadline = time.perf_counter() + timeout
        expect = cmd | CMD_RESPONSE
        while time.perf_counter() < deadline:
            self._pump(min(0.02, max(0.0, deadline - time.perf_counter())))
            matched = None
            for item in self._inbox:
                if item.cmd == expect and item.seq == seq and item.src == self._dev_id:
                    matched = item
                    break
            self._inbox.clear()
            if matched is not None:
                return matched
        return None

    def _pump(self, seconds: float) -> None:
        ser = self._ser
        if ser is None:
            return
        deadline = time.perf_counter() + seconds
        while time.perf_counter() < deadline:
            waiting = ser.in_waiting
            data = ser.read(waiting or 1)
            if not data:
                return
            for frame in self._parser.feed(data):
                self._on_frame(frame)

    def _on_frame(self, frame: Frame) -> None:
        if frame.src != self._dev_id:
            return
        if frame.cmd == CMD_REPORT:
            kind, value = parse_sample(frame.payload, with_status=False)
            if kind == "ok" and isinstance(value, Sample):
                self.offer_sample(value)
            return
        if frame.cmd & CMD_RESPONSE:
            self._inbox.append(frame)

    def _poll_once(self) -> None:
        frame = self._transact(CMD_GET_DATA, b"", 0.08)
        if frame is None:
            now = time.monotonic()
            if now - self._last_timeout_log > 1.0:
                self._last_timeout_log = now
                self._emit("log", "查询超时")
            return
        kind, value = parse_sample(frame.payload, with_status=True)
        if kind == "ok" and isinstance(value, Sample):
            self.offer_sample(value)
        elif kind == "err":
            self._emit("log", f"读采样失败：{status_text(int(value))}")

    def _read_info(self) -> None:
        frame = self._transact(CMD_GET_INFO, b"", 0.3)
        if frame is None:
            self._emit("log", "没有读到设备信息，确认设备号、类型和波特率")
            return
        kind, value = parse_info(frame.payload)
        if kind != "ok" or not isinstance(value, DeviceInfo):
            self._emit("log", f"读信息失败：{status_text(int(value) if kind == 'err' else 0)}")
            return
        self._device_mode = value.mode
        self._report_ms = value.report_ms
        self._dev_id = value.dev_id
        self._device_kind = value.dev_type
        if value.dev_type in (TYPE_ENCODER, TYPE_POSITION):
            self._dev_type = value.dev_type
        self._emit("info", value)
        mode = "主动上报" if value.mode == MODE_REPORT else "主从"
        kind_name = "直线位置" if value.dev_type == TYPE_POSITION else "角度编码器"
        self._emit("log", f"设备 {value.dev_id}  {kind_name}  固件 {value.fw >> 8}.{value.fw & 0xFF}  {mode}  {value.report_ms} ms")
        if value.dev_type == TYPE_POSITION:
            cal = self._transact(CMD_GET_CAL, b"", 0.3)
            if cal is not None and cal.payload and cal.payload[0] == 0:
                self._after_cal(cal)

    def _set_stream(self, mode: str, period_ms: int) -> None:
        if mode == "off":
            self._streaming = False
            self._emit("streaming", False)
            self._emit("log", "停止采集")
            return
        period_ms = max(0, period_ms)
        if mode == "report":
            period_ms = max(5, period_ms)
            if not self._apply_mode(MODE_REPORT, period_ms, quiet=True):
                self._emit("streaming", False)
                return
            self._stream_mode = "report"
            self._streaming = True
            self._emit("streaming", True)
            self._emit("log", f"主动上报，周期 {period_ms} ms")
            return
        if self._device_mode == MODE_REPORT:
            if not self._apply_mode(MODE_POLL, max(period_ms, 5), quiet=True):
                self._emit("streaming", False)
                return
        self._stream_mode = "poll"
        self._gap_s = max(0.0, period_ms / 1000.0)
        self._streaming = True
        self._emit("streaming", True)
        gap = "尽快" if period_ms <= 0 else f"间隔 {period_ms} ms"
        self._emit("log", f"主从查询，{gap}")

    def _apply_mode(self, mode: int, period_ms: int, quiet: bool) -> bool:
        period_ms = min(60000, max(5, period_ms))
        if self._device_mode == mode and self._report_ms == period_ms:
            return True
        payload = bytes((mode & 0xFF,)) + struct.pack("<H", period_ms)
        frame = self._transact(CMD_SET_MODE, payload, 0.4)
        if frame is None:
            self._emit("log", "设置上传模式超时")
            return False
        if not frame.payload or frame.payload[0] != 0:
            code = frame.payload[0] if frame.payload else 0
            self._emit("log", f"设置上传模式失败：{status_text(code)}")
            return False
        self._device_mode = mode
        self._report_ms = period_ms
        if not quiet:
            name = "主动上报" if mode == MODE_REPORT else "主从"
            self._emit("log", f"上传模式已改为{name}，周期 {period_ms} ms")
        self._emit("mode", mode, period_ms)
        return True

    def _checked(self, title: str, frame: Frame | None, follow) -> None:
        if frame is None:
            self._emit("log", f"{title}超时")
            return
        if not frame.payload or frame.payload[0] != 0:
            code = frame.payload[0] if frame.payload else 0
            self._emit("log", f"{title}失败：{status_text(code)}")
            return
        self._emit("log", f"{title}完成")
        if follow is not None:
            follow(frame)

    def _after_zero(self, frame: Frame) -> None:
        if len(frame.payload) >= 3:
            fmt = "<h" if self._device_kind == TYPE_POSITION else "<H"
            zero = struct.unpack_from(fmt, frame.payload, 1)[0]
            self._emit("zero", zero)

    def _after_cal(self, frame: Frame) -> None:
        payload = frame.payload
        # 结束校准是 13 字节：状态 + 3 个 int16 + 3 个 uint16，成功时没有标志字节。
        # 读取校准是 14 字节：状态、标志，然后才是偏置和增益。
        if len(payload) >= 14:
            flags = payload[1]
            offset_at = 2
            gain_at = 8
        elif len(payload) >= 13:
            flags = 0x02
            offset_at = 1
            gain_at = 7
        else:
            return
        offsets = struct.unpack_from("<hhh", payload, offset_at)
        gains = struct.unpack_from("<HHH", payload, gain_at)
        self._emit("cal", offsets, gains, flags)

    def _set_profile(self, dev_type: int) -> None:
        if dev_type not in (TYPE_ENCODER, TYPE_POSITION):
            self._emit("log", "测量模式只能是角度或直线")
            return
        frame = self._transact(CMD_SET_PROFILE, bytes((dev_type,)), 0.4)
        if frame is None or not frame.payload or frame.payload[0] != 0:
            code = frame.payload[0] if frame and frame.payload else 0
            self._emit("log", f"切换测量模式失败：{status_text(code) if frame else '超时'}")
            return
        self._dev_type = dev_type
        name = "直线位置" if dev_type == TYPE_POSITION else "角度编码器"
        self._emit("log", f"已切换为{name}，原来的零点和校准已清除")
        self._read_info()

    def _set_sigma(self, sigma_um: int) -> None:
        frame = self._transact(CMD_SET_SIGMA, struct.pack("<H", int(sigma_um) & 0xFFFF), 0.4)
        if frame is None or not frame.payload or frame.payload[0] != 0:
            code = frame.payload[0] if frame and frame.payload else 0
            self._emit("log", f"写敏感宽度失败：{status_text(code) if frame else '超时'}")
            return
        got = struct.unpack_from("<H", frame.payload, 1)[0] if len(frame.payload) >= 3 else sigma_um
        self._emit("sigma", got)
        self._emit("log", f"敏感宽度已写成 {got / 1000:.1f} mm")

    def _set_id(self, new_id: int) -> None:
        if not 1 <= new_id <= 254:
            self._emit("log", "设备号要在 1 到 254")
            return
        frame = self._transact(CMD_SET_ID, bytes((new_id,)), 0.4)
        if frame is None or not frame.payload or frame.payload[0] != 0:
            code = frame.payload[0] if frame and frame.payload else 0
            self._emit("log", f"修改设备号失败：{status_text(code) if frame else '超时'}")
            return
        self._dev_id = frame.payload[1]
        self._emit("id", self._dev_id)
        self._emit("log", f"设备号已改为 {self._dev_id}")

    def _set_baud(self, index: int) -> None:
        if not 0 <= index < len(BAUD_TABLE):
            return
        frame = self._transact(CMD_SET_BAUD, bytes((index,)), 0.4)
        if frame is None or not frame.payload or frame.payload[0] != 0:
            code = frame.payload[0] if frame and frame.payload else 0
            self._emit("log", f"修改波特率失败：{status_text(code) if frame else '超时'}")
            return
        baud = BAUD_TABLE[frame.payload[1]]
        port = self._ser.port if self._ser is not None else ""
        self._close()
        time.sleep(0.05)
        try:
            self._ser = serial.Serial(port, baudrate=baud, bytesize=8, parity="N", stopbits=1, timeout=0.02)
        except serial.SerialException as exc:
            self._emit("log", f"设备已改到 {baud}，本机重新打开失败：{exc}")
            self._emit("connected", False)
            return
        self._parser.reset()
        self._emit("baud", baud)
        self._emit("log", f"波特率已改为 {baud}")
        self._read_info()

    def _factory(self) -> None:
        frame = self._transact(CMD_FACTORY, b"", 0.5)
        if frame is None or not frame.payload or frame.payload[0] != 0:
            code = frame.payload[0] if frame and frame.payload else 0
            self._emit("log", f"恢复出厂失败：{status_text(code) if frame else '超时'}")
            return
        new_id = frame.payload[1] if len(frame.payload) > 1 else 1
        index = frame.payload[2] if len(frame.payload) > 2 else 4
        baud = BAUD_TABLE[index] if 0 <= index < len(BAUD_TABLE) else 115200
        port = self._ser.port if self._ser is not None else ""
        current = self._ser.baudrate if self._ser is not None else baud
        self._dev_id = new_id
        self._device_mode = MODE_POLL
        self._report_ms = 20
        self._streaming = False
        if current != baud and port:
            self._close()
            time.sleep(0.05)
            try:
                self._ser = serial.Serial(port, baudrate=baud, bytesize=8, parity="N", stopbits=1, timeout=0.02)
            except serial.SerialException as exc:
                self._emit("log", f"设备已恢复出厂，本机重新打开失败：{exc}")
                self._emit("connected", False)
                return
            self._parser.reset()
        self._dev_type = TYPE_ANY
        self._emit("factory", new_id, baud)
        self._emit("streaming", False)
        self._emit("log", f"已恢复出厂：设备号 {new_id}，{baud}，主从，角度编码器")
        self._read_info()
