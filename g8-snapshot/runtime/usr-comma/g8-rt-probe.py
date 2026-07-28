#!/usr/bin/env python3
import os
import resource

print("===G8_RT_PROBE_BEGIN===")
print(f"PID={os.getpid()} UID={os.getuid()} GID={os.getgid()} GROUPS={os.getgroups()}")

for name in ("RLIMIT_RTPRIO", "RLIMIT_RTTIME", "RLIMIT_NICE"):
  key = getattr(resource, name, None)
  if key is not None:
    print(f"{name}={resource.getrlimit(key)}")

with open("/proc/self/status", "r") as f:
  for line in f:
    if line.startswith(("CapInh:", "CapPrm:", "CapEff:", "CapBnd:", "CapAmb:", "NoNewPrivs:", "Seccomp:")):
      print(line.strip())

with open("/proc/self/cgroup", "r") as f:
  print("CGROUP=" + " | ".join(line.strip() for line in f if line.strip()))

try:
  print(f"SCHED_BEFORE policy={os.sched_getscheduler(0)} prio={os.sched_getparam(0).sched_priority}")
  os.sched_setscheduler(0, os.SCHED_FIFO, os.sched_param(53))
  print(f"SCHED_FIFO_53=PASS policy={os.sched_getscheduler(0)} prio={os.sched_getparam(0).sched_priority}")
  os.sched_setscheduler(0, os.SCHED_OTHER, os.sched_param(0))
  print("SCHED_RESTORE=PASS")
except OSError as e:
  print(f"SCHED_FIFO_53=FAIL errno={e.errno} repr={e!r}")

print("===G8_RT_PROBE_END===")
