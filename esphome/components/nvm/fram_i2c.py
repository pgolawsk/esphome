"""I2C FRAM platform for NVM component.

This provides I2C FRAM (Ferroelectric RAM) support as an NVM platform.

Example configuration:

    nvm:
      - platform: fram_i2c
        id: my_fram
        address: 0x50
        model: MB85RC256
        partitions:
          - id: preferences
            type: preferences
            size: 4KB
          - id: sensor_cache
            type: raw
            size: 12KB
"""

import esphome.codegen as cg
from esphome.components import i2c
import esphome.config_validation as cv
from esphome.const import CONF_ADDRESS, CONF_ID, CONF_MODEL

from . import NVM_PLATFORM_SCHEMA, NvmPlatform, nvm_ns, register_nvm_platform

CODEOWNERS = ["@pawelo"]
DEPENDENCIES = ["i2c"]

# FRAM I2C platform class
FramI2cPlatform = nvm_ns.class_("FramI2cPlatform", NvmPlatform, i2c.I2CDevice)

# FRAM model definitions (size in bytes, address width)
FRAM_MODELS = {
    "MB85RC64": 8 * 1024,  # 64 Kbit = 8 KB
    "MB85RC128": 16 * 1024,  # 128 Kbit = 16 KB
    "MB85RC256": 32 * 1024,  # 256 Kbit = 32 KB
    "MB85RC512": 64 * 1024,  # 512 Kbit = 64 KB
    "MB85RC1M": 128 * 1024,  # 1 Mbit = 128 KB
}

# FRAM I2C platform schema
CONFIG_SCHEMA = NVM_PLATFORM_SCHEMA.extend(
    {
        cv.GenerateID(): cv.declare_id(FramI2cPlatform),
        cv.Optional(CONF_ADDRESS, default=0x50): cv.i2c_address,
        cv.Required(CONF_MODEL): cv.one_of(*FRAM_MODELS.keys(), upper=True),
    }
).extend(i2c.i2c_device_schema(None))


async def to_code(config):
    """Generate code for FRAM I2C platform."""
    # Create FRAM platform
    var = cg.new_Pvariable(config[CONF_ID])

    # Set I2C address
    cg.add(var.set_address(config[CONF_ADDRESS]))

    # Set FRAM model
    model_size = FRAM_MODELS[config[CONF_MODEL]]
    cg.add(var.set_model(model_size))

    # Register I2C device
    await i2c.register_i2c_device(var, config)

    # Register partitions
    register_nvm_platform(var, config)

    # Register as component
    await cg.register_component(var, config)
