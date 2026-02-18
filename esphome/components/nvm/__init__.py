"""NVM (Non-Volatile Memory) component for ESPHome.

This component provides a unified interface for NVM devices like FRAM and EEPROM,
with support for partitions that can be used for preferences, raw data, or key-value storage.

Example configuration:

    nvm:
      - platform: fram_i2c
        id: my_fram
        address: 0x50
        model: MB85RC256
        partitions:
          - id: preferences
            type: preferences
            size: 4kB
          - id: sensor_cache
            type: raw
            size: 12kB
          - id: config
            type: key_value
            size: 2kB
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_OFFSET, CONF_SIZE, CONF_TYPE

CODEOWNERS = ["@pgolawsk"]
MULTI_CONF = True
IS_PLATFORM_COMPONENT = True

# NVM namespace
nvm_ns = cg.esphome_ns.namespace("nvm")

# C++ classes
NvmPlatform = nvm_ns.class_("NvmPlatform", cg.Component)
NvmPartition = nvm_ns.class_("NvmPartition")
PreferencesPartition = nvm_ns.class_("PreferencesPartition", NvmPartition, cg.Component)
RawPartition = nvm_ns.class_("RawPartition", NvmPartition)
KeyValuePartition = nvm_ns.class_("KeyValuePartition", NvmPartition)
PartitionType = nvm_ns.enum("PartitionType")

# Partition type enum values
PARTITION_TYPE_PREFERENCES = PartitionType.PREFERENCES
PARTITION_TYPE_RAW = PartitionType.RAW
PARTITION_TYPE_KEY_VALUE = PartitionType.KEY_VALUE

# Configuration keys
CONF_PARTITIONS = "partitions"

# Partition configuration schema
PARTITION_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_ID): cv.declare_id(NvmPartition),
        cv.Required(CONF_TYPE): cv.one_of(
            "preferences", "raw", "key_value", lower=True
        ),
        cv.Required(CONF_SIZE): cv.All(cv.validate_bytes, cv.Range(min=1)),
        cv.Optional(CONF_OFFSET, default=0): cv.All(cv.positive_int, cv.Range(min=0)),
    }
)


async def register_nvm_platform(platform_var, config):
    """Register an NVM platform with its partitions.

    This function also automatically registers PreferencesPartition
    as a Component if a preferences partition is found.
    """
    preferences_partition_var = None

    # Add partitions
    for partition_config in config.get(CONF_PARTITIONS, []):
        partition_type = partition_config[CONF_TYPE]
        partition_size = partition_config[
            CONF_SIZE
        ]  # Already converted to int by cv.validate_bytes
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
        partition_id = partition_config[CONF_ID].id
        partition_config_struct = cg.StructInitializer(
            "PartitionConfig",
            ("id", partition_id),
            ("type", partition_type_enum),
            ("offset", partition_offset),
            ("size", partition_size),
        )

        # Add partition to platform
        partition_var = cg.add(platform_var.add_partition(partition_config_struct))

        # If this is a preferences partition, register it as a component
        if partition_type == "preferences":
            preferences_partition_var = partition_var

    # If a preferences partition was found, register it as a component
    if preferences_partition_var is not None:
        await cg.register_component(preferences_partition_var, {})


# Track preferences partition count globally (for MULTI_CONF validation)
# Using a list to avoid global statement issues
_preferences_partition_count = [0]


def reset_preferences_partition_count():
    """Reset the global preferences partition counter. Used in tests."""
    _preferences_partition_count[0] = 0


# Base NVM platform schema (platforms will extend this)
NVM_PLATFORM_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_ID): cv.declare_id(NvmPlatform),
        cv.Optional(CONF_PARTITIONS, default=[]): cv.ensure_list(PARTITION_SCHEMA),
    }
)


def validate_preferences_partition_count(config):
    """Validate that only one preferences partition exists across all NVM devices."""
    for partition in config.get(CONF_PARTITIONS, []):
        if partition[CONF_TYPE] == "preferences":
            _preferences_partition_count[0] += 1
            if _preferences_partition_count[0] > 1:
                raise cv.Invalid(
                    "Only one preferences partition is allowed across all NVM devices. "
                    "Multiple preferences partitions would conflict as they all replace "
                    "the global preferences backend."
                )

    return config
