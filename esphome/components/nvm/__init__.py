"""NVM (Non-Volatile Memory) component for ESPHome.

This component provides a unified interface for NVM devices like FRAM and EEPROM,
with support for partitions that can be used for preferences, raw data, or key-value storage.

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
          - id: config
            type: key_value
            size: 2KB
"""

import esphome.codegen as cg
from esphome.components import i2c
import esphome.config_validation as cv
from esphome.const import CONF_ADDRESS, CONF_ID, CONF_OFFSET, CONF_SIZE, CONF_TYPE

CODEOWNERS = ["@pawelo"]

# NVM namespace
nvm_ns = cg.esphome_ns.namespace("nvm")

# C++ classes
NvmPlatform = nvm_ns.class_("NvmPlatform", cg.Component)
NvmPartition = nvm_ns.class_("NvmPartition")
PreferencesPartition = nvm_ns.class_("PreferencesPartition", NvmPartition)
RawPartition = nvm_ns.class_("RawPartition", NvmPartition)
KeyValuePartition = nvm_ns.class_("KeyValuePartition", NvmPartition)
PartitionType = nvm_ns.enum("PartitionType")

# Partition type enum values
PARTITION_TYPE_PREFERENCES = PartitionType.PREFERENCES
PARTITION_TYPE_RAW = PartitionType.RAW
PARTITION_TYPE_KEY_VALUE = PartitionType.KEY_VALUE

# Configuration keys
CONF_PARTITIONS = "partitions"
CONF_PLATFORM = "platform"

# Partition configuration
PARTITION_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_ID): cv.declare_id(NvmPartition),
        cv.Required(CONF_TYPE): cv.one_of(
            "preferences", "raw", "key_value", lower=True
        ),
        cv.Required(CONF_SIZE): cv.size,
        cv.Optional(CONF_OFFSET, default=0): cv.uint32_t,
    }
)


# Size parsing helper
def parse_size(value):
    """Parse size string like '4KB' or '32768' to bytes."""
    if isinstance(value, int):
        return value
    if isinstance(value, str):
        value = value.strip().upper()
        multipliers = {
            "B": 1,
            "KB": 1024,
            "MB": 1024 * 1024,
        }
        for suffix, mult in multipliers.items():
            if value.endswith(suffix):
                return int(value[: -len(suffix)]) * mult
        return int(value)
    raise cv.Invalid(f"Invalid size: {value}")


# NVM platform base schema
NVM_PLATFORM_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_ID): cv.declare_id(NvmPlatform),
        cv.Optional(CONF_PARTITIONS, default=[]): cv.ensure_list(PARTITION_SCHEMA),
    }
).extend(cv.COMPONENT_SCHEMA)


async def register_nvm_platform(config, platform_var):
    """Register an NVM platform with its partitions.

    This should be called by platform implementations (fram, eeprom, etc.)
    after creating their platform variable.
    """
    for partition_config in config.get(CONF_PARTITIONS, []):
        partition_type = partition_config[CONF_TYPE]
        partition_size = parse_size(partition_config[CONF_SIZE])
        partition_offset = partition_config[CONF_OFFSET]

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
        partition_config_struct = cg.StructInitializer(
            "PartitionConfig",
            ("id", partition_config[CONF_ID].id),
            ("type", partition_type_enum),
            ("offset", partition_offset),
            ("size", partition_size),
        )

        # Add partition to platform
        cg.add(platform_var.add_partition(partition_config_struct))

    await cg.register_component(platform_var, config)


# FRAM platform (will be moved to separate file later)
CONF_FRAM_TYPE = "fram_type"

FRAM_MODELS = {
    "MB85RC64": (64 * 1024 // 8, 16),  # 64 Kbit = 8 KB, 16-bit addressing
    "MB85RC128": (128 * 1024 // 8, 16),  # 128 Kbit = 16 KB, 16-bit addressing
    "MB85RC256": (256 * 1024 // 8, 16),  # 256 Kbit = 32 KB, 16-bit addressing
    "MB85RC512": (512 * 1024 // 8, 17),  # 512 Kbit = 64 KB, 17-bit addressing
    "MB85RC1M": (1024 * 1024 // 8, 18),  # 1 Mbit = 128 KB, 18-bit addressing
}


def validate_fram_type(value):
    """Validate FRAM model type."""
    if value not in FRAM_MODELS:
        raise cv.Invalid(
            f"Unknown FRAM type: {value}. Valid types: {list(FRAM_MODELS.keys())}"
        )
    return value


# Placeholder for FRAM platform schema
# This will be replaced by proper platform implementation
FRAM_PLATFORM_SCHEMA = NVM_PLATFORM_SCHEMA.extend(
    {
        cv.Required(CONF_ADDRESS): cv.i2c_address,
        cv.Optional(CONF_FRAM_TYPE, default="MB85RC256"): validate_fram_type,
    }
).extend(i2c.i2c_device_schema(0x50))


# Configuration validation
CONFIG_SCHEMA = cv.Schema({})


async def to_code(config):
    """Generate code for NVM component."""
    # The NVM component itself doesn't generate code
    # Platform implementations (fram, eeprom) will call register_nvm_platform
    pass
