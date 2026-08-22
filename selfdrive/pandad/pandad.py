#!/usr/bin/env python3
# simple pandad wrapper that updates the panda first
import os
import usb1
import time
import signal
import subprocess

from panda import Panda, PandaDFU, PandaProtocolMismatch, McuType, FW_PATH
from openpilot.common.basedir import BASEDIR
from openpilot.common.params import Params
from openpilot.system.hardware import HARDWARE
from openpilot.common.swaglog import cloudlog

from openpilot.sunnypilot.selfdrive.pandad.rivian_long_flasher import flash_rivian_long


BLACK_PANDA_FW = os.path.join(BASEDIR, "g8/black_panda/panda.bin.signed")


def get_expected_firmware(panda_type: bytes) -> str:
  if panda_type == Panda.HW_TYPE_BLACK:
    return BLACK_PANDA_FW
  return os.path.join(FW_PATH, McuType.H7.config.app_fn)


def get_expected_signature(panda_type: bytes) -> bytes:
  return Panda.get_signature_from_firmware(get_expected_firmware(panda_type))

def flash_panda(panda_serial: str):
  panda = Panda(panda_serial)
  panda_type = panda.get_type()

  # skip flashing if the detected panda is not supported
  if panda_type not in Panda.SUPPORTED_DEVICES:
    cloudlog.warning(f"Panda {panda_serial} is not supported (hw_type: {panda_type}), skipping flash...")
    panda.close()
    return

  fw_fn = get_expected_firmware(panda_type)
  fw_signature = get_expected_signature(panda_type)
  internal_panda = panda.is_internal()

  panda_version = "bootstub" if panda.bootstub else panda.get_version()
  panda_signature = b"" if panda.bootstub else panda.get_signature()
  cloudlog.warning(f"Panda {panda_serial} connected, version: {panda_version}, signature {panda_signature.hex()[:16]}, expected {fw_signature.hex()[:16]}")

  if panda.bootstub or panda_signature != fw_signature:
    cloudlog.info("Panda firmware out of date, update required")
    panda.flash(fn=fw_fn)
    cloudlog.info("Done flashing")

  if panda.bootstub:
    if panda_type == Panda.HW_TYPE_BLACK:
      raise RuntimeError("Black Panda firmware did not boot; refusing automatic bootstub/DFU recovery")
    bootstub_version = panda.get_version()
    cloudlog.info(f"Flashed firmware not booting, flashing development bootloader. {bootstub_version=}, {internal_panda=}")
    if internal_panda:
      HARDWARE.recover_internal_panda()
    panda.recover(reset=(not internal_panda))
    cloudlog.info("Done flashing bootstub")

  if panda.bootstub:
    cloudlog.info("Panda still not booting, exiting")
    raise AssertionError

  panda_signature = panda.get_signature()
  if panda_signature != fw_signature:
    cloudlog.info("Version mismatch after flashing, exiting")
    raise AssertionError

  panda.close()


def check_panda_support(panda_serials: list[str]) -> list[str]:
  spi_serials = set(Panda.spi_list())
  for serial in panda_serials:
    if serial in spi_serials:
      return [serial]

  for serial in panda_serials:
    panda = Panda(serial)
    is_internal = panda.is_internal()
    panda.close()
    if is_internal:
      return [serial]

  # LG G8 port has no built-in panda; use the sole external USB panda.
  if len(panda_serials) == 1:
    return panda_serials

  return []


def main() -> None:
  # signal pandad to close the relay and exit
  def signal_handler(signum, frame):
    cloudlog.info(f"Caught signal {signum}, exiting")
    nonlocal do_exit
    do_exit = True
    if process is not None:
      process.send_signal(signal.SIGINT)

  process = None
  do_exit = False
  signal.signal(signal.SIGINT, signal_handler)

  # check health for lost heartbeat
  try:
    for s in Panda.list():
      with Panda(s) as p:
        health = p.health()
        if p.is_internal() and health["heartbeat_lost"]:
          Params().put_bool("PandaHeartbeatLost", True, block=True)
          cloudlog.event("heartbeat lost", deviceState=health)
  except Exception:
    cloudlog.exception("pandad.uncaught_exception")

  count = 0
  while not do_exit:
    try:
      cloudlog.event("pandad.flash_and_connect", count=count)
      if (count % 2) == 0:
        HARDWARE.reset_internal_panda()
      else:
        HARDWARE.recover_internal_panda()
      count += 1

      # Flash all Pandas in DFU mode
      for serial in PandaDFU.list():
        cloudlog.info(f"Panda in DFU mode found, flashing recovery {serial}")
        PandaDFU(serial).recover()
        time.sleep(1)

      panda_serials = Panda.list()
      if len(panda_serials):
        # custom flasher for xnor's Rivian Longitudinal Upgrade Kit
        flash_rivian_long(panda_serials)
        # find the internal supported panda (e.g. skip external Black Panda)
        panda_serials = check_panda_support(panda_serials)

        assert len(panda_serials) == 1
        cloudlog.info(f"{len(panda_serials)} panda found, connecting - {panda_serials}")
        flash_panda(panda_serials[0])

        # run real pandad
        os.environ['MANAGER_DAEMON'] = 'pandad'
        child_env = os.environ.copy()
        if HARDWARE.__class__.__module__.endswith(".g8.hardware"):
          child_env["G8_AGNOS"] = "1"
        process = subprocess.Popen(["./pandad", panda_serials[0]], cwd=os.path.join(BASEDIR, "selfdrive/pandad"), env=child_env)
        process.wait()
    # TODO: wrap all panda exceptions in a base panda exception
    except (usb1.USBErrorNoDevice, usb1.USBErrorPipe):
      # a panda was disconnected while setting everything up. let's try again
      cloudlog.exception("Panda USB exception while setting up")
    except PandaProtocolMismatch:
      cloudlog.exception("pandad.protocol_mismatch")
    except Exception:
      cloudlog.exception("pandad.uncaught_exception")

    # G8 has no internal Panda reset/recovery delay; prevent a hot retry loop.
    if HARDWARE.__class__.__module__.endswith(".g8.hardware"):
      time.sleep(1)

if __name__ == "__main__":
  main()
