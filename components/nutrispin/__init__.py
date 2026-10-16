"""Local PF006 pairing and control on Raspberry Pi Pico W, ESPHome 2026.9+."""
import esphome.codegen as cg
import esphome.config_validation as cv
import esphome.final_validate as fv
from esphome.components import (binary_sensor, ble_device_base, bluetooth_connection,
                               button, number, rp2040_ble, select, sensor,
                               switch, text, text_sensor, time)
from esphome.const import CONF_ID, CONF_NAME, CONF_MAC_ADDRESS, CONF_TIME_ID

DEPENDENCIES = ["rp2", "rp2_ble_tracker", "time"]
AUTO_LOAD = ["bluetooth_connection", "md5", "json", "binary_sensor", "button",
             "number", "select", "sensor", "switch", "text", "text_sensor"]
MULTI_CONF = True
ns = cg.esphome_ns.namespace("nutrispin")
NutriSpin = ns.class_("NutriSpin", cg.Component, ble_device_base.ESPBTDeviceListener)
FeederButton = ns.class_("FeederButton", button.Button)
FeederSwitch = ns.class_("FeederSwitch", switch.Switch)
FeederNumber = ns.class_("FeederNumber", number.Number)
FeederText = ns.class_("FeederText", text.Text)
FeederSelect = ns.class_("FeederSelect", select.Select)

BUTTONS = [("pair", "Pair", "mdi:bluetooth-connect"), ("next", "Next Compartment", "mdi:skip-next"),
           ("previous", "Previous Compartment", "mdi:skip-previous"),
           ("refresh", "Refresh Data", "mdi:refresh"), ("sync_time", "Sync Clock", "mdi:clock-check"),
           ("factory_reset", "Factory Reset", "mdi:restore")]
SENSORS = [("battery", "Battery", "%", "battery"), ("rssi", "Bluetooth Signal", "dBm", "signal_strength"),
           ("feed_report", "Feed Report DP15", None, None),
           ("history_count", "History Record Count", None, None),
           ("last_feed_code", "Last Feeding Code", None, None)]
TEXT_SENSORS = [("status", "Status"), ("mac", "MAC"), ("firmware", "Firmware"),
                ("hardware", "Hardware"), ("protocol", "Protocol"),
                ("last_feeding", "Last Feeding"),
                ("history_1", "Raw History 1"), ("history_2", "Raw History 2"),
                ("schedule_readback", "Raw Schedule")]

def expand(config):
    config = dict(config)
    name = config.get(CONF_NAME, "Feeder")
    for key, label, _icon in BUTTONS:
        config.setdefault(key, {CONF_NAME: f"{name} {label}"})
    for key, label, *_ in SENSORS + TEXT_SENSORS:
        config.setdefault(key, {CONF_NAME: f"{name} {label}"})
    for key, label in [("connected", "Connected"), ("paired", "Paired"),
                       ("pairing_device", "Pairing Device"), ("pairing_devices", "Pairing Devices"),
                       ("manual_enabled", "Manual Buttons"), ("voice_times", "Sound Repetitions"),
                       ("language", "Language"), ("schedule_write", "Set Raw Schedule")]:
        config.setdefault(key, {CONF_NAME: f"{name} {label}"})
    config.setdefault("meals", [{"index": i} for i in range(1, 6)])
    meals = []
    for meal in config["meals"]:
        meal = dict(meal)
        for key, label in [("enabled", "Enabled"), ("at", "Time HH:MM"),
                           ("weekdays", "Weekday Mask"), ("value", "Raw Count Field")]:
            meal.setdefault(key, {CONF_NAME: f"{name} Meal {meal['index']} {label}"})
        meals.append(meal)
    config["meals"] = meals
    return config

SCHEMA = {
    cv.GenerateID(): cv.declare_id(NutriSpin), cv.Required(CONF_NAME): cv.string,
    cv.Optional(CONF_MAC_ADDRESS): cv.mac_address,
    cv.GenerateID(CONF_TIME_ID): cv.use_id(time.RealTimeClock),
    cv.Optional("credentials", default=""): cv.string,
    cv.Optional("poll_interval", default="60s"): cv.positive_time_period_milliseconds,
    cv.Optional("clock_sync_interval", default="30min"): cv.positive_time_period_milliseconds,
    cv.Optional("connected"): binary_sensor.binary_sensor_schema(device_class="connectivity"),
    cv.Optional("paired"): binary_sensor.binary_sensor_schema(),
    cv.Optional("manual_enabled"): switch.switch_schema(FeederSwitch),
    cv.Optional("voice_times"): number.number_schema(FeederNumber, icon="mdi:volume-high"),
    cv.Optional("language"): select.select_schema(FeederSelect),
    cv.Optional("pairing_device"): select.select_schema(FeederSelect),
    cv.Optional("pairing_devices"): text_sensor.text_sensor_schema(),
    cv.Optional("schedule_write"): text.text_schema(FeederText, mode="text"),
    cv.Optional("meals"): cv.All(cv.ensure_list(cv.Schema({
        cv.Required("index"): cv.int_range(min=1, max=5),
        cv.Required("enabled"): switch.switch_schema(FeederSwitch),
        cv.Required("at"): text.text_schema(FeederText, mode="text"),
        cv.Required("weekdays"): number.number_schema(FeederNumber).extend({cv.Optional("disabled_by_default", default=True): cv.boolean}),
        cv.Required("value"): number.number_schema(FeederNumber).extend({cv.Optional("disabled_by_default", default=True): cv.boolean}),
    })), cv.Length(min=5, max=5)),
}
for i, (key, label, icon) in enumerate(BUTTONS):
    schema = button.button_schema(FeederButton, icon=icon)
    if key == "factory_reset":
        schema = schema.extend({cv.Optional("disabled_by_default", default=True): cv.boolean,
                                cv.Optional("entity_category", default="config"): cv.entity_category})
    SCHEMA[cv.Optional(key)] = schema
for key, label, unit, device in SENSORS:
    kwargs = {"accuracy_decimals": 0}
    if unit: kwargs["unit_of_measurement"] = unit
    if device: kwargs["device_class"] = device
    schema = sensor.sensor_schema(**kwargs)
    if key == "feed_report": schema = schema.extend({cv.Optional("disabled_by_default", default=True): cv.boolean})
    SCHEMA[cv.Optional(key)] = schema
for key, label in TEXT_SENSORS:
    kwargs = {"device_class": "timestamp"} if key == "last_feeding" else {}
    if key in ("history_1", "history_2", "schedule_readback", "hardware", "protocol"):
        disabled = True
    else: disabled = False
    schema = text_sensor.text_sensor_schema(**kwargs)
    if disabled: schema = schema.extend({cv.Optional("disabled_by_default", default=True): cv.boolean})
    SCHEMA[cv.Optional(key)] = schema

def validate(config):
    if len({m["index"] for m in config["meals"]}) != 5:
        raise cv.Invalid("Exactly one configuration for each meal 1..5 is required")
    return rp2040_ble.consume_connection_slots(1, "nutrispin")(config)

CONFIG_SCHEMA = cv.All(expand, cv.Schema(SCHEMA).extend(cv.COMPONENT_SCHEMA)
    .extend(ble_device_base.BLE_DEVICE_SCHEMA).extend(bluetooth_connection.gatt_client_schema("rp2")), validate)

def final_validate(config):
    feeders = fv.full_config.get().get("nutrispin", [])
    addresses = [str(item[CONF_MAC_ADDRESS]) for item in feeders if CONF_MAC_ADDRESS in item]
    if len(addresses) != len(set(addresses)):
        raise cv.Invalid("Each feeder requires a different MAC address")
    return config

FINAL_VALIDATE_SCHEMA = final_validate

async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await ble_device_base.register_ble_device(var, config)
    backend = await bluetooth_connection.new_gatt_backend(config)
    cg.add(var.set_backend(backend))
    cg.add(backend.set_listener(var))
    cg.add(var.set_slot_id(config[CONF_ID].id))
    if CONF_MAC_ADDRESS in config:
        cg.add(var.set_mac(config[CONF_MAC_ADDRESS].as_hex))
    cg.add(var.set_label(config[CONF_NAME]))
    cg.add(var.set_initial_credentials(config["credentials"]))
    cg.add(var.set_poll_interval(config["poll_interval"].total_milliseconds))
    cg.add(var.set_clock_sync_interval(config["clock_sync_interval"].total_milliseconds))
    clock = await cg.get_variable(config[CONF_TIME_ID]); cg.add(var.set_clock(clock))
    for i, (key, *_rest) in enumerate(BUTTONS):
        entity = await button.new_button(config[key]); cg.add(entity.bind(var, i, -1))
    for i, (key, *_rest) in enumerate(SENSORS):
        entity = await sensor.new_sensor(config[key]); cg.add(var.set_sensor(i, entity))
    for i, (key, *_rest) in enumerate(TEXT_SENSORS):
        entity = await text_sensor.new_text_sensor(config[key]); cg.add(var.set_text_sensor(i, entity))
    for i, key in enumerate(("connected", "paired")):
        entity = await binary_sensor.new_binary_sensor(config[key]); cg.add(var.set_binary_sensor(i, entity))
    entity = await switch.new_switch(config["manual_enabled"]); cg.add(entity.bind(var, 0, -1)); cg.add(var.set_manual(entity))
    entity = await number.new_number(config["voice_times"], min_value=0, max_value=2, step=1)
    cg.add(entity.bind(var, 0, -1)); cg.add(var.set_voice(entity))
    entity = await select.new_select(config["language"], options=["Simplified Chinese", "Traditional Chinese", "English", "Japanese", "Korean"])
    cg.add(entity.bind(var)); cg.add(var.set_language(entity))
    entity = await select.new_select(config["pairing_device"], options=["Automatic"] + [f"Device {i}" for i in range(1, 7)])
    cg.add(entity.bind_pairing(var)); cg.add(var.set_pairing_select(entity))
    entity = await text_sensor.new_text_sensor(config["pairing_devices"])
    cg.add(var.set_pairing_devices(entity))
    entity = await text.new_text(config["schedule_write"], min_length=50, max_length=50, pattern="[0-9a-fA-F]{50}")
    cg.add(entity.bind(var, 0, -1)); cg.add(var.set_schedule_text(entity))
    for meal in config["meals"]:
        index = meal["index"] - 1
        entity = await switch.new_switch(meal["enabled"]); cg.add(entity.bind(var, 1, index)); cg.add(var.set_meal_switch(index, entity))
        entity = await text.new_text(meal["at"], min_length=5, max_length=5, pattern="([01][0-9]|2[0-3]):[0-5][0-9]")
        cg.add(entity.bind(var, 1, index)); cg.add(var.set_meal_text(index, entity))
        for kind, (key, maximum) in enumerate((("weekdays", 127), ("value", 255)), 1):
            entity = await number.new_number(meal[key], min_value=0, max_value=maximum, step=1)
            cg.add(entity.bind(var, kind, index)); cg.add(var.set_meal_number(index, kind-1, entity))
