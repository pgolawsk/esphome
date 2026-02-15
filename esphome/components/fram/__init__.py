import esphome.codegen as cg
from esphome.components import i2c
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_MODEL

CODEOWNERS = ["@esphome"]
DEPENDENCIES = ["i2c"]
MULTI_CONF = True

fram_ns = cg.esphome_ns.namespace("fram")
FramComponent = fram_ns.class_("Fram", cg.Component, i2c.I2CDevice)

# Define fram_pref types here to avoid circular import
fram_pref_ns = cg.esphome_ns.namespace("fram_pref")
FramPrefComponent = fram_pref_ns.class_(
    "FramPref", cg.Component, cg.esphome_ns.class_("ESPPreferences")
)

# FRAM model definitions
FRAM_MODELS = {
    "MB85RC64": (8192, 2),  # 8KB, 2-byte address
    "MB85RC256": (32768, 2),  # 32KB, 2-byte address
    "MB85RC512": (65536, 2),  # 64KB, 2-byte address
    "MB85RC1M": (131072, 2),  # 128KB, 2-byte address
}

PREFERENCES_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(FramPrefComponent),
        cv.Optional("pool_size", default="1kB"): cv.validate_bytes,
    }
).extend(cv.COMPONENT_SCHEMA)

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(FramComponent),
            cv.Optional(CONF_MODEL): cv.one_of(*FRAM_MODELS, upper=True),
            cv.Optional("size"): cv.validate_bytes,
            cv.Optional("preferences"): PREFERENCES_SCHEMA,
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(i2c.i2c_device_schema(0x50))
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await i2c.register_i2c_device(var, config)

    if CONF_MODEL in config:
        size, addr_width = FRAM_MODELS[config[CONF_MODEL]]
        cg.add(var.set_size_bytes(size))
        cg.add(var.set_address_width(addr_width))
    elif "size" in config:
        cg.add(var.set_size_bytes(config["size"]))

    if "preferences" in config:
        conf = config["preferences"]
        pref_var = cg.new_Pvariable(conf[CONF_ID], var)
        await cg.register_component(pref_var, conf)
        cg.add(pref_var.set_pool_size(conf["pool_size"]))
