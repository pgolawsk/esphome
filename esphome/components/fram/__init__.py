import esphome.codegen as cg
from esphome.components import i2c
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_SIZE

from . import fram_pref

CODEOWNERS = ["@esphome"]
DEPENDENCIES = ["i2c"]
MULTI_CONF = True

fram_ns = cg.esphome_ns.namespace("fram")
FramComponent = fram_ns.class_("Fram", cg.Component, i2c.I2CDevice)

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(FramComponent),
            cv.Optional(CONF_SIZE): cv.validate_bytes,
            cv.Optional("preferences"): fram_pref.CONFIG_SCHEMA,
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(i2c.i2c_device_schema(0x50))
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await i2c.register_i2c_device(var, config)

    if CONF_SIZE in config:
        cg.add(var.set_size_bytes(config[CONF_SIZE]))

    if "preferences" in config:
        conf = config["preferences"]
        conf["fram_id"] = var
        await fram_pref.to_code(conf)
