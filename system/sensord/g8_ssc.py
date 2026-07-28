import os
import socket
import struct
import time
from collections import Counter

import cereal.messaging as messaging
from cereal import log
from openpilot.common.swaglog import cloudlog

AF_QIPCRTR = getattr(socket, "AF_QIPCRTR", 42)
QRTR_PORT_CTRL = 0xFFFFFFFE
QRTR_TYPE_NEW_SERVER = 4
QRTR_TYPE_NEW_LOOKUP = 10

SNS_SERVICE = 400
SNS_VERSION = 1
SNS_INSTANCE = 0
SNS_CLIENT_REQ_MSG_ID = 0x20
SNS_SUID_REQ = 512
SNS_SUID_EVENT = 768
SNS_STD_SENSOR_CONFIG = 513
SNS_STD_SENSOR_EVENT = 1025
LOOKUP_SUID = 0xABABABABABABABAB
SSC_TICK_HZ = 19_200_000.0
SAMPLE_RATE_HZ = float(os.getenv("G8_SSC_IMU_RATE", "100"))

EXPECTED_SUIDS = {
  "accel": (int.from_bytes(b"invensen", "little"), int.from_bytes(b"seicmacc", "little")),
  "gyro": (int.from_bytes(b"invensen", "little"), int.from_bytes(b"seicmgyr", "little")),
}


def _enc_varint(n):
  out = bytearray()
  while True:
    b = n & 0x7F
    n >>= 7
    if n:
      out.append(b | 0x80)
    else:
      out.append(b)
      return bytes(out)


def _key(field, wire):
  return _enc_varint((field << 3) | wire)


def _fld_varint(field, value):
  return _key(field, 0) + _enc_varint(value)


def _fld_fixed32(field, value):
  return _key(field, 5) + struct.pack("<I", value)


def _fld_float(field, value):
  return _key(field, 5) + struct.pack("<f", float(value))


def _fld_fixed64(field, value):
  return _key(field, 1) + struct.pack("<Q", value)


def _fld_bytes(field, value):
  return _key(field, 2) + _enc_varint(len(value)) + value


def _make_suid(low, high):
  return _fld_fixed64(1, low) + _fld_fixed64(2, high)


def _make_client_request(low, high, msg_id, payload):
  std_req = _fld_bytes(2, payload)
  susp = _fld_varint(1, 1) + _fld_varint(2, 0)
  return (
    _fld_bytes(1, _make_suid(low, high))
    + _fld_fixed32(2, msg_id)
    + _fld_bytes(3, susp)
    + _fld_bytes(4, std_req)
  )


def _make_suid_query(datatype):
  req = _fld_bytes(1, datatype.encode("ascii")) + _fld_varint(2, 0) + _fld_varint(3, 0)
  return _make_client_request(LOOKUP_SUID, LOOKUP_SUID, SNS_SUID_REQ, req)


def _make_sensor_config(low, high, rate):
  return _make_client_request(low, high, SNS_STD_SENSOR_CONFIG, _fld_float(1, rate))


def _make_qmi_request(txn, pb):
  value = struct.pack("<H", len(pb)) + pb
  tlv1 = b"\x01" + struct.pack("<H", len(value)) + value
  return struct.pack("<BHHH", 0x00, txn, SNS_CLIENT_REQ_MSG_ID, len(tlv1)) + tlv1


def _read_varint(buf, pos):
  value = 0
  shift = 0
  while pos < len(buf):
    b = buf[pos]
    pos += 1
    value |= (b & 0x7F) << shift
    if not (b & 0x80):
      return value, pos
    shift += 7
    if shift > 70:
      raise ValueError("varint too long")
  raise ValueError("truncated varint")


def _pb_fields(buf):
  pos = 0
  while pos < len(buf):
    tag, pos = _read_varint(buf, pos)
    field = tag >> 3
    wire = tag & 7
    if wire == 0:
      value, pos = _read_varint(buf, pos)
    elif wire == 1:
      if pos + 8 > len(buf):
        raise ValueError("truncated fixed64")
      value = buf[pos:pos + 8]
      pos += 8
    elif wire == 2:
      ln, pos = _read_varint(buf, pos)
      if pos + ln > len(buf):
        raise ValueError("truncated bytes")
      value = buf[pos:pos + ln]
      pos += ln
    elif wire == 5:
      if pos + 4 > len(buf):
        raise ValueError("truncated fixed32")
      value = buf[pos:pos + 4]
      pos += 4
    else:
      raise ValueError(f"unsupported protobuf wire={wire}")
    yield field, wire, value


def _parse_qmi(pkt):
  if len(pkt) < 7:
    return None
  flags, txn, msg_id, msg_len = struct.unpack_from("<BHHH", pkt, 0)
  end = min(len(pkt), 7 + msg_len)
  pos = 7
  tlvs = []
  while pos + 3 <= end:
    typ = pkt[pos]
    ln = struct.unpack_from("<H", pkt, pos + 1)[0]
    pos += 3
    if pos + ln > end:
      break
    tlvs.append((typ, pkt[pos:pos + ln]))
    pos += ln
  return flags, txn, msg_id, tlvs


def _extract_client_event(pkt):
  parsed = _parse_qmi(pkt)
  if not parsed or parsed[0] != 0x04:
    return None
  for typ, val in parsed[3]:
    if typ == 0x02 and len(val) >= 2:
      ln = struct.unpack_from("<H", val, 0)[0]
      if ln <= len(val) - 2:
        return val[2:2 + ln]
  return None


def _outer_event_suid(client_event):
  low = high = None
  for field, wire, value in _pb_fields(client_event):
    if field != 1 or wire != 2:
      continue
    for sf, sw, sv in _pb_fields(value):
      if sf == 1 and sw == 1:
        low = struct.unpack("<Q", sv)[0]
      elif sf == 2 and sw == 1:
        high = struct.unpack("<Q", sv)[0]
    break
  return (low, high) if low is not None and high is not None else None


def _parse_client_events(client_event):
  for field, wire, value in _pb_fields(client_event):
    if field != 2 or wire != 2:
      continue
    msg_id = timestamp = payload = None
    for ef, ew, ev in _pb_fields(value):
      if ef == 1 and ew == 5:
        msg_id = struct.unpack("<I", ev)[0]
      elif ef == 2 and ew == 1:
        timestamp = struct.unpack("<Q", ev)[0]
      elif ef == 3 and ew == 2:
        payload = ev
    yield msg_id, timestamp, payload


def _parse_suid_event(payload):
  datatype = None
  suids = []
  for field, wire, value in _pb_fields(payload):
    if field == 1 and wire == 2:
      datatype = value.decode("ascii", errors="replace")
    elif field == 2 and wire == 2:
      low = high = None
      for sf, sw, sv in _pb_fields(value):
        if sf == 1 and sw == 1:
          low = struct.unpack("<Q", sv)[0]
        elif sf == 2 and sw == 1:
          high = struct.unpack("<Q", sv)[0]
      if low is not None and high is not None:
        suids.append((low, high))
  return datatype, suids


def _parse_std_sensor_event(payload):
  data = []
  status = None
  for field, wire, value in _pb_fields(payload):
    if field == 1 and wire == 5:
      data.append(struct.unpack("<f", value)[0])
    elif field == 1 and wire == 2 and len(value) % 4 == 0:
      for off in range(0, len(value), 4):
        data.append(struct.unpack_from("<f", value, off)[0])
    elif field == 2 and wire == 0:
      status = int(value)
  return data, status


def _qrtr_lookup(service, wanted_version, wanted_instance, timeout=3.0):
  sock = socket.socket(AF_QIPCRTR, socket.SOCK_DGRAM)
  sock.settimeout(0.5)
  try:
    local_node, _ = sock.getsockname()
    sock.sendto(struct.pack("<IIIII", QRTR_TYPE_NEW_LOOKUP, service, 0, 0, 0),
                (local_node, QRTR_PORT_CTRL))
    deadline = time.monotonic() + timeout
    found = []
    while time.monotonic() < deadline:
      try:
        data, _ = sock.recvfrom(256)
      except socket.timeout:
        continue
      if len(data) < 20:
        continue
      cmd, srv, instver, node, port = struct.unpack_from("<IIIII", data, 0)
      if cmd != QRTR_TYPE_NEW_SERVER:
        continue
      if srv == 0 and instver == 0 and node == 0 and port == 0:
        break
      if srv != service:
        continue
      version = instver & 0xFF
      instance = instver >> 8
      found.append((version, instance, node, port))
    for version, instance, node, port in found:
      if version == wanted_version and instance == wanted_instance:
        return node, port
    raise RuntimeError(f"SNS service {service} not found; candidates={found}")
  finally:
    sock.close()


class G8SSCConnection:
  def __init__(self):
    self.sock = None
    self.suids = {}
    self.name_by_suid = {}
    self.best_clock_offset_ns = None
    self.last_event_ts_ns = {"accel": 0, "gyro": 0}
    self.dropped_status = Counter()

  def close(self):
    if self.sock is not None:
      try:
        self.sock.close()
      except OSError:
        pass
      self.sock = None

  def connect(self):
    node, port = _qrtr_lookup(SNS_SERVICE, SNS_VERSION, SNS_INSTANCE)
    cloudlog.info(f"G8 SSC sensord: SNS endpoint {node}:{port}")
    self.sock = socket.socket(AF_QIPCRTR, socket.SOCK_DGRAM)
    self.sock.settimeout(1.0)
    self.dest = (node, port)

    txn = 1
    for datatype in ("accel", "gyro"):
      self.sock.sendto(_make_qmi_request(txn, _make_suid_query(datatype)), self.dest)
      deadline = time.monotonic() + 3.0
      while time.monotonic() < deadline and datatype not in self.suids:
        try:
          raw, _ = self.sock.recvfrom(65535)
        except socket.timeout:
          continue
        client_event = _extract_client_event(raw)
        if client_event is None:
          continue
        for msg_id, _, payload in _parse_client_events(client_event):
          if msg_id != SNS_SUID_EVENT or payload is None:
            continue
          got_datatype, found = _parse_suid_event(payload)
          if got_datatype == datatype and found:
            self.suids[datatype] = found[0]
      txn += 1

    for datatype in ("accel", "gyro"):
      got = self.suids.get(datatype)
      expected = EXPECTED_SUIDS[datatype]
      if got != expected:
        raise RuntimeError(f"unexpected {datatype} SUID: got={got}, expected={expected}")

    self.name_by_suid = {self.suids["accel"]: "accel", self.suids["gyro"]: "gyro"}

    self.sock.sendto(_make_qmi_request(
      txn, _make_sensor_config(*self.suids["accel"], SAMPLE_RATE_HZ)), self.dest)
    txn += 1
    self.sock.sendto(_make_qmi_request(
      txn, _make_sensor_config(*self.suids["gyro"], SAMPLE_RATE_HZ)), self.dest)

    cloudlog.info(f"G8 SSC sensord: accel+gyro configured at {SAMPLE_RATE_HZ:.1f} Hz")

  def recv_event(self):
    raw, _ = self.sock.recvfrom(65535)
    client_event = _extract_client_event(raw)
    if client_event is None:
      return []

    sensor = self.name_by_suid.get(_outer_event_suid(client_event))
    if sensor is None:
      return []

    out = []
    for msg_id, ssc_ticks, payload in _parse_client_events(client_event):
      if msg_id != SNS_STD_SENSOR_EVENT or ssc_ticks is None or payload is None:
        continue

      values, status = _parse_std_sensor_event(payload)
      if len(values) < 3:
        continue

      # Qualcomm status 0 is unreliable. The LG G8's first gyro status-0 frame
      # is saturated at roughly +/-34.9 rad/s, so never publish status 0.
      if status is None or status <= 0:
        self.dropped_status[(sensor, status)] += 1
        continue

      ssc_ns = int((ssc_ticks * 1_000_000_000) / SSC_TICK_HZ)
      rx_mono_ns = time.monotonic_ns()
      observed_offset_ns = rx_mono_ns - ssc_ns

      if self.best_clock_offset_ns is None or observed_offset_ns < self.best_clock_offset_ns:
        self.best_clock_offset_ns = observed_offset_ns

      event_ts_ns = ssc_ns + self.best_clock_offset_ns
      if event_ts_ns <= self.last_event_ts_ns[sensor]:
        event_ts_ns = self.last_event_ts_ns[sensor] + 1
      self.last_event_ts_ns[sensor] = event_ts_ns

      out.append((sensor, event_ts_ns, values[:3], status))

    return out


def _phone_to_openpilot(v):
  # SSC +X = portrait-right, +Y = earpiece/top, +Z = out through screen.
  # Mounted landscape facing cabin, earpiece on car-left, USB on car-right.
  # openpilot device frame is +X forward, +Y right, +Z down.
  x, y, z = v
  return [-z, -y, -x]


def run_g8_ssc_sensord():
  pm = messaging.PubMaster(["accelerometer", "gyroscope"])
  cloudlog.info(
    "G8 SSC sensord starting: direct Qualcomm SNS over AF_QIPCRTR, "
    f"rate={SAMPLE_RATE_HZ:.1f} Hz, transform=[-z,-y,-x]"
  )

  while True:
    conn = G8SSCConnection()
    try:
      conn.connect()
      last_data = time.monotonic()

      while True:
        try:
          events = conn.recv_event()
        except socket.timeout:
          if time.monotonic() - last_data > 3.0:
            raise RuntimeError("no LG G8 SSC IMU samples for >3 seconds")
          continue

        if events:
          last_data = time.monotonic()

        for sensor, timestamp_ns, values, _status in events:
          mapped = _phone_to_openpilot(values)
          evt = log.SensorEventData.new_message()
          evt.timestamp = timestamp_ns
          evt.source = log.SensorEventData.SensorSource.android

          if sensor == "accel":
            vec = evt.init("acceleration")
            vec.v = mapped
            msg = messaging.new_message("accelerometer", valid=True)
            msg.accelerometer = evt
            pm.send("accelerometer", msg)
          elif sensor == "gyro":
            vec = evt.init("gyroUncalibrated")
            vec.v = mapped
            msg = messaging.new_message("gyroscope", valid=True)
            msg.gyroscope = evt
            pm.send("gyroscope", msg)

    except Exception:
      cloudlog.exception("G8 SSC sensord connection failed; retrying")
      time.sleep(1.0)
    finally:
      conn.close()
