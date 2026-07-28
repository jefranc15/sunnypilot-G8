#!/usr/bin/env python3
import ctypes
import glob
import os
import sys

label = sys.argv[1] if len(sys.argv) > 1 else "COMMA"
scan_only = "--scan-only" in sys.argv

print(f"===G8_DRM_PROBE_{label}_BEGIN===")
print(f"PID={os.getpid()} UID={os.getuid()} GID={os.getgid()} GROUPS={os.getgroups()}")

for p in ("/dev/dri/card0", "/dev/dri/renderD128"):
  try:
    st = os.stat(p)
    print(f"NODE {p} mode={oct(st.st_mode & 0o777)} uid={st.st_uid} gid={st.st_gid} rdev={os.major(st.st_rdev)}:{os.minor(st.st_rdev)}")
  except OSError as e:
    print(f"NODE {p} FAIL errno={e.errno} repr={e!r}")

for p in sorted(glob.glob("/sys/class/drm/card0-*/status")):
  try:
    status = open(p).read().strip()
    base = os.path.dirname(p)
    enabled = open(os.path.join(base, "enabled")).read().strip() if os.path.exists(os.path.join(base, "enabled")) else "NA"
    modes = ",".join(open(os.path.join(base, "modes")).read().split()) if os.path.exists(os.path.join(base, "modes")) else "NA"
    print(f"CONNECTOR {os.path.basename(base)} status={status} enabled={enabled} modes={modes}")
  except OSError as e:
    print(f"CONNECTOR {p} FAIL errno={e.errno} repr={e!r}")

holders = []
for f in glob.glob("/proc/[0-9]*/fd/*"):
  try:
    t = os.readlink(f)
    if t != "/dev/dri/card0":
      continue
    pid = f.split("/")[2]
    try:
      comm = open(f"/proc/{pid}/comm").read().strip()
    except OSError:
      comm = "?"
    holders.append((int(pid), comm, f))
  except OSError:
    pass
if holders:
  for pid, comm, f in sorted(holders):
    print(f"HOLDER pid={pid} comm={comm} fd={f}")
else:
  print("HOLDER none-visible")

if not scan_only:
  fd = -1
  try:
    fd = os.open("/dev/dri/card0", os.O_RDWR | os.O_CLOEXEC)
    print(f"OPEN_CARD0=PASS fd={fd}")

    libdrm = ctypes.CDLL("libdrm.so.2", use_errno=True)
    libdrm.drmIsMaster.argtypes = [ctypes.c_int]
    libdrm.drmIsMaster.restype = ctypes.c_int
    ctypes.set_errno(0)
    is_master = libdrm.drmIsMaster(fd)
    print(f"DRM_IS_MASTER={is_master} errno={ctypes.get_errno()}")

    magic = ctypes.c_uint32(0)
    libdrm.drmGetMagic.argtypes = [ctypes.c_int, ctypes.POINTER(ctypes.c_uint32)]
    libdrm.drmGetMagic.restype = ctypes.c_int
    ctypes.set_errno(0)
    rc = libdrm.drmGetMagic(fd, ctypes.byref(magic))
    print(f"DRM_GETMAGIC_RC={rc} errno={ctypes.get_errno()} magic=0x{magic.value:x}")

    libdrm.drmModeGetResources.argtypes = [ctypes.c_int]
    libdrm.drmModeGetResources.restype = ctypes.c_void_p
    libdrm.drmModeFreeResources.argtypes = [ctypes.c_void_p]
    res = libdrm.drmModeGetResources(fd)
    print(f"DRM_RESOURCES_PASS={bool(res)} errno={ctypes.get_errno()}")
    if res:
      libdrm.drmModeFreeResources(res)

    gbm = ctypes.CDLL("libgbm.so.1", use_errno=True)
    gbm.gbm_create_device.argtypes = [ctypes.c_int]
    gbm.gbm_create_device.restype = ctypes.c_void_p
    gbm.gbm_device_get_backend_name.argtypes = [ctypes.c_void_p]
    gbm.gbm_device_get_backend_name.restype = ctypes.c_char_p
    gbm.gbm_device_destroy.argtypes = [ctypes.c_void_p]
    ctypes.set_errno(0)
    dev = gbm.gbm_create_device(fd)
    if dev:
      name = gbm.gbm_device_get_backend_name(dev)
      print(f"GBM_CREATE=PASS backend={(name or b'?').decode(errors='replace')}")
      gbm.gbm_device_destroy(dev)
    else:
      print(f"GBM_CREATE=FAIL errno={ctypes.get_errno()}")
  except OSError as e:
    print(f"OPEN_OR_PROBE_FAIL errno={e.errno} repr={e!r}")
  except Exception as e:
    print(f"PROBE_EXCEPTION type={type(e).__name__} repr={e!r}")
  finally:
    if fd >= 0:
      os.close(fd)

print(f"===G8_DRM_PROBE_{label}_END===")
