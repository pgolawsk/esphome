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
from esphome.const import CONF_ADDRESS, CONF_ID, CONF_OFFSET, CONF_SIZE, CONF_TYPE

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
PartitionType = nvm_ns.enum("PartitionType", is_class=True)

# Partition type enum values
PARTITION_TYPE_PREFERENCES = PartitionType.PREFERENCES
PARTITION_TYPE_RAW = PartitionType.RAW
PARTITION_TYPE_KEY_VALUE = PartitionType.KEY_VALUE

# Configuration keys
CONF_PARTITIONS = "partitions"


def _validate_partition_id(config):
    """Validate and set the correct ID type based on partition type."""
    partition_type = config[CONF_TYPE]
    if partition_type == "preferences":
        config[CONF_ID] = cv.declare_id(PreferencesPartition)(config[CONF_ID])
    elif partition_type == "raw":
        config[CONF_ID] = cv.declare_id(RawPartition)(config[CONF_ID])
    elif partition_type == "key_value":
        config[CONF_ID] = cv.declare_id(KeyValuePartition)(config[CONF_ID])
    return config


# Partition configuration schema
PARTITION_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.Required(CONF_ID): cv.use_id(NvmPartition),
            cv.Required(CONF_TYPE): cv.one_of(
                "preferences", "raw", "key_value", lower=True
            ),
            cv.Required(CONF_SIZE): cv.All(cv.validate_bytes, cv.Range(min=1)),
            cv.Optional(CONF_OFFSET, default=0): cv.All(
                cv.positive_int, cv.Range(min=0)
            ),
        }
    ),
    _validate_partition_id,
)


async def register_nvm_platform(platform_var, config):
    """Register an NVM platform with its partitions.

    Note: PreferencesPartition is registered as a Component in C++ via App.register_component()
    in add_partition(), so we don't need to register it here.
    """
    # Add partitions
    for partition_config in config.get(CONF_PARTITIONS, []):
        partition_type = partition_config[CONF_TYPE]
        partition_size = partition_config[
            CONF_SIZE
        ]  # Already converted to int by cv.validate_bytes
        partition_offset = partition_config[CONF_OFFSET]

        # Determine partition type enum and target class
        if partition_type == "preferences":
            partition_type_enum = PARTITION_TYPE_PREFERENCES
            target_class = PreferencesPartition
        elif partition_type == "raw":
            partition_type_enum = PARTITION_TYPE_RAW
            target_class = RawPartition
        elif partition_type == "key_value":
            partition_type_enum = PARTITION_TYPE_KEY_VALUE
            target_class = KeyValuePartition
        else:
            raise cv.Invalid(f"Unknown partition type: {partition_type}")

        # Create partition config struct
        partition_id_obj = partition_config[CONF_ID]
        partition_config_struct = cg.StructInitializer(
            nvm_ns.class_("PartitionConfig"),
            ("id", partition_id_obj.id),
            ("type", partition_type_enum),
            ("offset", partition_offset),
            ("size", partition_size),
        )

        # Create partition via platform and register it as a variable
        # Pvariable both declares the global variable AND registers it with CORE
        # so it can be accessed via id() in lambdas
        # Note: add_partition() returns NvmPartition*, we cast to the derived type
        cg.Pvariable(
            partition_id_obj,
            cg.RawExpression(
                f"static_cast<{target_class}*>"
                f"({platform_var.add_partition(partition_config_struct)})"
            ),
        )
        # Note: PreferencesPartition is registered with App.register_component() in C++
        # add_partition(), so setup() will be called automatically by Application


# Track preferences partition count globally (for MULTI_CONF validation)
# Using a list to avoid global statement issues
_preferences_partition_count = [0]


def reset_preferences_partition_count():
    """Reset the global preferences partition counter. Used in tests."""
    _preferences_partition_count[0] = 0


# Track NVM I2C addresses globally (for MULTI_CONF validation)
# Key: (i2c_id, address) tuple, Value: config_id
_nvm_i2c_devices = {}


def reset_nvm_i2c_devices():
    """Reset the global NVM I2C device tracking. Used in tests."""
    global _nvm_i2c_devices  # noqa: PLW0603
    _nvm_i2c_devices = {}


def validate_nvm_i2c_address(config):
    """Validate that I2C address is unique per I2C bus across all NVM devices.

    This prevents multiple NVM configurations from accessing the same
    physical device on the same bus, which would cause data corruption.

    Same address on different I2C buses is allowed (different physical devices).
    """
    # Get address from config (may be None for platforms without I2C)
    address = config.get(CONF_ADDRESS)
    if address is None:
        return config

    # Get I2C bus ID (defaults to None for default bus)
    from esphome.const import CONF_I2C_ID

    i2c_id = config.get(CONF_I2C_ID)
    # Convert i2c_id to a hashable key (it might be an ID object)
    if i2c_id is not None and hasattr(i2c_id, "id"):
        i2c_id = i2c_id.id

    # Create a unique key for this (bus, address) combination
    device_key = (i2c_id, address)

    # Get config ID for error messages
    config_id = config.get(CONF_ID)
    if hasattr(config_id, "id"):
        config_id = config_id.id

    # Check for duplicate
    if device_key in _nvm_i2c_devices:
        existing_id = _nvm_i2c_devices[device_key]
        bus_desc = f" on I2C bus '{i2c_id}'" if i2c_id else " on default I2C bus"
        raise cv.Invalid(
            f"Duplicate NVM I2C address 0x{address:02X}{bus_desc}. "
            f"Already used by '{existing_id}'. "
            f"Each NVM device must have a unique I2C address on the same bus."
        )

    _nvm_i2c_devices[device_key] = config_id
    return config


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
