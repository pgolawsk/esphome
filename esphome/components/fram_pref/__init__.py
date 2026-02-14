import esphome.codegen as cg
from esphome.components import fram
import esphome.config_validation as cv
from esphome.const import CONF_ID

CODEOWNERS = ["@esphome"]
DEPENDENCIES = ["fram"]

fram_pref_ns = cg.esphome_ns.namespace("fram_pref")
FramPrefComponent = fram_pref_ns.class_(
    "FramPref", cg.Component, cg.esphome_ns.class_("ESPPreferences")
)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(FramPrefComponent),
        cv.GenerateID("fram_id"): cv.use_id(fram.FramComponent),
        cv.Optional("pool_size", default="1KB"): cv.validate_bytes,
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID], await cg.get_variable(config["fram_id"]))
    await cg.register_component(var, config)
    cg.add(var.set_pool_size(config["pool_size"]))
