"""Local, single-client INMP441 HTTP WAV stream for ESPHome 2026.10.0."""
import ipaddress
import re

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import microphone, sensor, socket
from esphome.components.esp32 import include_builtin_idf_component
from esphome.const import CONF_ID

DEPENDENCIES = ["esp32", "wifi", "microphone"]
AUTO_LOAD = ["sensor", "display", "socket"]
ns = cg.esphome_ns.namespace("lan_microphone")
LanMicrophone = ns.class_("LanMicrophone", cg.Component)


def token(value):
    value = cv.string_strict(value)
    if not re.fullmatch(r"[A-Za-z0-9_-]{32,128}", value) or "REPLACE" in value.upper():
        raise cv.Invalid("Use a private random URL-safe token of 32–128 characters in secrets.yaml")
    return value


def subnet(value):
    try:
        net = ipaddress.ip_network(cv.string_strict(value), strict=True)
    except ValueError as exc:
        raise cv.Invalid("Use an IPv4 LAN network such as 192.168.1.0/24") from exc
    private = ("10.0.0.0/8", "172.16.0.0/12", "192.168.0.0/16")
    if not isinstance(net, ipaddress.IPv4Network) or not any(net.subnet_of(ipaddress.ip_network(n)) for n in private):
        raise cv.Invalid("Only RFC1918 private IPv4 subnets are permitted")
    return str(net)


METRICS = {
    "input_rms": ("dBFS", 1),
    "output_rms": ("dBFS", 1),
    "output_peak": ("dBFS", 1),
    "clipping": ("%", 2),
    "dropped_blocks": ("blocks", 0),
    "samples_per_second": ("samples/s", 0),
    "zero_samples": ("%", 2),
    "input_near_rail": ("%", 2),
    "connections": ("connections", 0),
}

CONFIG_SCHEMA = cv.All(
    cv.Schema({
        cv.GenerateID(): cv.declare_id(LanMicrophone),
        cv.Required("microphone_id"): cv.use_id(microphone.Microphone),
        cv.Required("token"): token,
        cv.Required("allowed_subnet"): subnet,
        cv.Optional("port", default=8081): cv.port,
        **{cv.Optional(key): sensor.sensor_schema(unit_of_measurement=unit, accuracy_decimals=accuracy,
                                                 entity_category="diagnostic")
           for key, (unit, accuracy) in METRICS.items()},
    }).extend(cv.COMPONENT_SCHEMA),
    cv.only_on_esp32,
    cv.only_with_framework("esp-idf"),
    socket.consume_sockets(3, "lan_microphone"),
    socket.consume_sockets(1, "lan_microphone", socket.SocketType.TCP_LISTEN),
    socket.consume_sockets(1, "lan_microphone", socket.SocketType.UDP),  # esp_http_server control socket
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    mic = await cg.get_variable(config["microphone_id"])
    cg.add(var.set_microphone(mic))
    cg.add(var.set_token(config["token"]))
    network = ipaddress.ip_network(config["allowed_subnet"])
    cg.add(var.set_subnet(int(network.network_address), int(network.netmask)))
    cg.add(var.set_port(config["port"]))
    for key in METRICS:
        if key in config:
            sens = await sensor.new_sensor(config[key])
            cg.add(getattr(var, "set_" + key + "_sensor")(sens))
    include_builtin_idf_component("esp_http_server")
