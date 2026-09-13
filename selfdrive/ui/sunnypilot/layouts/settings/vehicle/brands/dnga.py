"""
DNGA-specific vehicle settings for the LG G8 sunnypilot port.
"""
from openpilot.selfdrive.ui.sunnypilot.layouts.settings.vehicle.brands.base import BrandSettings
from openpilot.selfdrive.ui.ui_state import ui_state
from openpilot.system.ui.lib.multilang import tr
from openpilot.system.ui.sunnypilot.widgets.list_view import toggle_item_sp


class DngaSettings(BrandSettings):
  def __init__(self):
    super().__init__()

    # ToggleSP reads BOOL params with get_bool(), which does not apply key defaults.
    # Materialize the safe default so a fresh install always starts with Gear Check enabled.
    if ui_state.params.get("DngaGearCheck") is None:
      ui_state.params.put_bool("DngaGearCheck", True, block=True)

    self.gear_check_toggle = toggle_item_sp(
      tr("Gear Check"),
      tr("Require the vehicle to be in Drive before allowing engagement. Disable to allow SET while in Park or Neutral. Reverse remains blocked."),
      param="DngaGearCheck",
    )

    self.items = [self.gear_check_toggle]

  def update_settings(self):
    offroad = ui_state.is_offroad()
    self.gear_check_toggle.action_item.set_enabled(offroad)

    description = tr(
      "Require the vehicle to be in Drive before allowing engagement. Disable to allow SET while in Park or Neutral. Reverse remains blocked."
    )
    if not offroad:
      description = (
        f'<b>{tr("Enable \"Always Offroad\" in Device panel, or turn vehicle off to toggle.")}</b><br><br>'
        f"{description}"
      )

    self.gear_check_toggle.set_description(description)
