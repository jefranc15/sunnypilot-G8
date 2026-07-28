import os
from typing import cast

from openpilot.system.hardware.base import HardwareBase

G8 = os.path.isfile('/G8')
TICI = os.path.isfile('/TICI')
AGNOS = os.path.isfile('/AGNOS')
PC = not TICI and not G8

if G8:
  from openpilot.system.hardware.g8.hardware import G8
  HARDWARE = cast(HardwareBase, G8())
elif TICI:
  from openpilot.system.hardware.tici.hardware import Tici
  HARDWARE = cast(HardwareBase, Tici())
else:
  from openpilot.system.hardware.pc.hardware import Pc
  HARDWARE = cast(HardwareBase, Pc())
