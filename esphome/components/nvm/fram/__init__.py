"""FRAM platform for NVM component.

This provides FRAM (Ferroelectric RAM) support as an NVM platform.

Example configuration:

    nvm:
      - platform: fram
        id: my_fram
        address: 0x50
        type: MB85RC256
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
from esphome.const import CONF_ADDRESS, CONF_ID, CONF_OFFSET, CONF_SIZE, CONF_TYPE

# Import from parent nvm component
from .. import (
    PARTITION_TYPE_KEY_VALUE,
    PARTITION_TYPE_PREFERENCES,
    PARTITION_TYPE_RAW,
    NvmPartition,
    NvmPlatform,
    nvm_ns,
    parse_size,
)

CODEOWNERS = ["@pawelo"]

# FRAM namespace
fram_ns = nvm_ns.namespace("fram")

# C++ classes
FramPlatform = fram_ns.class_("FramPlatform", NvmPlatform, i2c.I2CDevice)

# FRAM model definitions (size in bytes, address width)
FRAM_MODELS = {
    "MB85RC64": (8 * 1024, 2),  # 64 Kbit = 8 KB, 16-bit addressing
    "MB85RC128": (16 * 1024, 2),  # 128 Kbit = 16 KB, 16-bit addressing
    "MB85RC256": (32 * 1024, 2),  # 256 Kbit = 32 KB, 16-bit addressing
    "MB85RC512": (64 * 1024, 3),  # 512 Kbit = 64 KB, 17-bit addressing
    "MB85RC1M": (128 * 1024, 3),  # 1 Mbit = 128 KB, 18-bit addressing
}

# Configuration keys
CONF_PARTITIONS = "partitions"


def validate_fram_type(value):
    """Validate FRAM model type."""
    value = cv.string(value)
    if value not in FRAM_MODELS:
        raise cv.Invalid(
            f"Unknown FRAM type: {value}. Valid types: {list(FRAM_MODELS.keys())}"
        )
    return value


def validate_partitions(config):
    """Validate partition configuration for FRAM."""
    total_size = 0
    for partition in config.get(CONF_PARTITIONS, []):
        size = parse_size(partition[CONF_SIZE])
        offset = partition.get(CONF_OFFSET, 0)
        total_size = max(total_size, offset + size)

    fram_type = config.get(CONF_TYPE, "MB85RC256")
    fram_size = FRAM_MODELS[fram_type][0]

    if total_size > fram_size:
        raise cv.Invalid(
            f"Total partition size ({total_size} bytes) exceeds FRAM size ({fram_size} bytes)"
        )

    return config


# Partition schema with optional offset
FRAM_PARTITION_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_ID): cv.declare_id(NvmPartition),
        cv.Required(CONF_TYPE): cv.one_of(
            "preferences", "raw", "key_value", lower=True
        ),
        cv.Required(CONF_SIZE): cv.All(cv.string, parse_size),
        cv.Optional(CONF_OFFSET, default=0): cv.uint32_t,
    }
)


# FRAM platform configuration schema
CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(FramPlatform),
            cv.Optional(CONF_ADDRESS, default=0x50): cv.i2c_address,
            cv.Optional(CONF_TYPE, default="MB85RC256"): validate_fram_type,
            cv.Optional(CONF_PARTITIONS, default=[]): cv.ensure_list(
                FRAM_PARTITION_SCHEMA
            ),
        }
    )
    .extend(i2c.i2c_device_schema(0x50))
    .extend(cv.COMPONENT_SCHEMA),
    validate_partitions,
)


async def to_code(config):
    """Generate code for FRAM platform."""
    # Create FRAM platform variable
    var = cg.new_Pvariable(config[CONF_ID])

    # Set I2C address
    cg.add(var.set_address(config[CONF_ADDRESS]))

    # Set FRAM model
    fram_type = config[CONF_TYPE]
    size_bytes, address_width = FRAM_MODELS[fram_type]
    cg.add(var.set_model_config(size_bytes, address_width))

    # Register with I2C bus
    await i2c.register_i2c_device(var, config)

    # Register partitions
    for partition_config in config.get(CONF_PARTITIONS, []):
        partition_type = partition_config[CONF_TYPE]
        partition_size = parse_size(partition_config[CONF_SIZE])
        partition_offset = partition_config[CONF_OFFSET]

        # Determine partition class based on type
        # Determine partition type enum
        if partition_type == "preferences":
            partition_type_enum = PARTITION_TYPE_PREFERENCES
        elif partition_type == "raw":
            partition_type_enum = PARTITION_TYPE_RAW
        elif partition_type == "key_value":
            partition_type_enum = PARTITION_TYPE_KEY_VALUE
        else:
            raise cv.Invalid(f"Unknown partition type: {partition_type}")

        # Create partition config struct
        partition_id = partition_config[CONF_ID].id
        partition_config_struct = cg.StructInitializer(
            "PartitionConfig",
            ("id", partition_id),
            ("type", partition_type_enum),
            ("offset", partition_offset),
            ("size", partition_size),
        )

        # Add partition to platform
        cg.add(var.add_partition(partition_config_struct))

    # Register as component
    await cg.register_component(var, config)
