from esphome import pins
import esphome.codegen as cg
from esphome.components import display, spi
import esphome.config_validation as cv
from esphome.const import (
    CONF_BUSY_PIN,
    CONF_DC_PIN,
    CONF_ID,
    CONF_LAMBDA,
    CONF_MIRROR_X,
    CONF_MIRROR_Y,
    CONF_MODEL,
    CONF_PAGES,
    CONF_RESET_PIN,
    CONF_TEMPERATURE,
)

DEPENDENCIES = ["spi"]

CONF_PSR = "psr"

pervasive_epd_ns = cg.esphome_ns.namespace("pervasive_epd")
PervasiveEPD = pervasive_epd_ns.class_(
    "PervasiveEPD", cg.PollingComponent, spi.SPIDevice, display.DisplayBuffer
)

# Small iTC panels share one command sequence. Native geometry is
# (pixels per gate line, number of lines), i.e. portrait for most sizes.
# Default PSR values are the ones Pervasive's driver hard-codes for film C/J.
MODELS = {
    "1.54in": (152, 152, (0xCF, 0x8D)),
    "2.13in": (104, 212, (0xCF, 0x8D)),
    "2.66in": (152, 296, (0xCF, 0x8D)),
    "2.71in": (176, 264, (0xCF, 0x8D)),
    "2.87in": (128, 296, (0xCF, 0x8D)),
    "3.70in": (240, 416, (0x0F, 0x89)),
    "4.17in": (400, 300, (0x0F, 0x89)),
    "4.37in": (176, 480, (0x0F, 0x89)),
}

CONFIG_SCHEMA = cv.All(
    display.FULL_DISPLAY_SCHEMA.extend(
        {
            cv.GenerateID(): cv.declare_id(PervasiveEPD),
            cv.Required(CONF_MODEL): cv.one_of(*MODELS, lower=True),
            cv.Required(CONF_DC_PIN): pins.gpio_output_pin_schema,
            cv.Required(CONF_RESET_PIN): pins.gpio_output_pin_schema,
            cv.Optional(CONF_BUSY_PIN): pins.gpio_input_pin_schema,
            cv.Optional(CONF_TEMPERATURE, default=25): cv.int_range(min=0, max=50),
            cv.Optional(CONF_PSR): cv.All(
                cv.ensure_list(cv.hex_uint8_t), cv.Length(min=2, max=2)
            ),
            cv.Optional(CONF_MIRROR_X, default=False): cv.boolean,
            cv.Optional(CONF_MIRROR_Y, default=False): cv.boolean,
        }
    )
    .extend(cv.polling_component_schema("5min"))
    .extend(spi.spi_device_schema(cs_pin_required=True, default_data_rate=4e6)),
    cv.has_at_most_one_key(CONF_PAGES, CONF_LAMBDA),
)

FINAL_VALIDATE_SCHEMA = spi.final_validate_device_schema(
    "pervasive_epd", require_miso=False, require_mosi=True
)


async def to_code(config):
    width, height, default_psr = MODELS[config[CONF_MODEL]]
    var = cg.new_Pvariable(config[CONF_ID], config[CONF_MODEL], width, height)

    await display.register_display(var, config)
    await spi.register_spi_device(var, config, write_only=True)

    dc = await cg.gpio_pin_expression(config[CONF_DC_PIN])
    cg.add(var.set_dc_pin(dc))
    reset = await cg.gpio_pin_expression(config[CONF_RESET_PIN])
    cg.add(var.set_reset_pin(reset))
    if busy_pin := config.get(CONF_BUSY_PIN):
        busy = await cg.gpio_pin_expression(busy_pin)
        cg.add(var.set_busy_pin(busy))

    psr = config.get(CONF_PSR, default_psr)
    cg.add(var.set_psr(psr[0], psr[1]))
    cg.add(var.set_temperature(config[CONF_TEMPERATURE]))
    cg.add(var.set_mirror(config[CONF_MIRROR_X], config[CONF_MIRROR_Y]))

    if CONF_LAMBDA in config:
        lambda_ = await cg.process_lambda(
            config[CONF_LAMBDA], [(display.DisplayRef, "it")], return_type=cg.void
        )
        cg.add(var.set_writer(lambda_))
