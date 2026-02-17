"""NVM Preferences component for ESPHome.

This component integrates NVM partitions with ESPHome's preferences system,
allowing global variables and other preferences to be stored in external NVM
(FRAM/EEPROM) instead of internal flash.

Example configuration:

    nvm:
      - platform: fram_i2c
        id: my_fram
        address: 0x50
        model: MB85RC256
        partitions:
          - id: pref_store
            type: preferences
            size: 4KB

    nvm_pref:
      partition: pref_store
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_PARTITION

from . import PreferencesPartition, nvm_ns

CODEOWNERS = ["@pgolawsk"]

# C++ classes
NvmPreferences = nvm_ns.class_("NvmPreferences", cg.Component)

# Configuration schema
CONFIG_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_ID): cv.declare_id(NvmPreferences),
        cv.Required(CONF_PARTITION): cv.use_id(PreferencesPartition),
    }
)


async def to_code(config):
    """Generate code for NVM preferences."""
    var = cg.new_Pvariable(config[CONF_ID])

    # Get the partition
    partition = await cg.get_variable(config[CONF_PARTITION])
    cg.add(var.set_partition(partition))

    # Register the component
    await cg.register_component(var, config)
