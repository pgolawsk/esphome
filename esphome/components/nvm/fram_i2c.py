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
from esphome.const import CONF_ADDRESS, CONF_ID, CONF_MODEL, CONF_SIZE

from . import (
    CONF_PARTITIONS,
    NVM_PLATFORM_SCHEMA,
    NvmPlatform,
    nvm_ns,
    parse_size,
    register_nvm_platform,
    validate_preferences_partition_count,
)

CODEOWNERS = ["@pgolawsk"]
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


def validate_fram_config(config):
    """Validate that model or size is specified, and partitions fit within FRAM size."""
    # Check that either model or size is specified
    if CONF_MODEL not in config and CONF_SIZE not in config:
        raise cv.Invalid("Either 'model' or 'size' must be specified for FRAM device")

    # When using custom size, I2C address must be explicitly specified
    # (different FRAM devices have different default addresses)
    if CONF_SIZE in config and CONF_MODEL not in config and CONF_ADDRESS not in config:
        raise cv.Invalid(
            "I2C 'address' must be specified when using custom 'size' "
            "(different FRAM devices have different default addresses)"
        )

    # Determine FRAM size
    if CONF_MODEL in config:
        fram_size = FRAM_MODELS[config[CONF_MODEL]]
    else:
        fram_size = config[CONF_SIZE]

    # Validate partitions fit within FRAM size
    for partition in config.get(CONF_PARTITIONS, []):
        partition_size = parse_size(partition[CONF_SIZE])
        partition_offset = partition.get("offset", 0)
        partition_end = partition_offset + partition_size

        if partition_end > fram_size:
            raise cv.Invalid(
                f"Partition '{partition[CONF_ID]}' (offset={partition_offset}, "
                f"size={partition_size}) exceeds FRAM size ({fram_size} bytes). "
                f"Partition end: {partition_end}, FRAM size: {fram_size}"
            )

    return config


def validate_fram_address(config):
    """Set default I2C address for known models, require explicit address for custom size."""
    # If model is specified without address, use default 0x50 (standard for MB85RC series)
    if CONF_MODEL in config and CONF_ADDRESS not in config:
        config[CONF_ADDRESS] = 0x50
    # If custom size without address, validation in validate_fram_config will catch it
    return config


# FRAM I2C platform schema
CONFIG_SCHEMA = cv.All(
    NVM_PLATFORM_SCHEMA.extend(
        {
            cv.GenerateID(): cv.declare_id(FramI2cPlatform),
            cv.Optional(CONF_ADDRESS): cv.i2c_address,
            cv.Optional(CONF_MODEL): cv.one_of(*FRAM_MODELS.keys(), upper=True),
            cv.Optional(CONF_SIZE): cv.validate_bytes,
        }
    ).extend(i2c.i2c_device_schema(None)),
    validate_fram_address,
    validate_fram_config,
    validate_preferences_partition_count,
)


async def to_code(config):
    """Generate code for FRAM I2C platform."""
    # Create FRAM platform
    var = cg.new_Pvariable(config[CONF_ID])

    # Set I2C address
    cg.add(var.set_address(config[CONF_ADDRESS]))

    # Set FRAM size (from model or custom size)
    if CONF_MODEL in config:
        model_size = FRAM_MODELS[config[CONF_MODEL]]
        cg.add(var.set_model(model_size))
    elif CONF_SIZE in config:
        cg.add(var.set_model(config[CONF_SIZE]))

    # Register I2C device
    await i2c.register_i2c_device(var, config)

    # Register partitions
    await register_nvm_platform(var, config)

    # Register as component
    await cg.register_component(var, config)
