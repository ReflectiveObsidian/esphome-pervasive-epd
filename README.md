# esphome-pervasive-epd

ESPHome external component for **Pervasive Displays iTC e-paper panels**:
small black/white (film C) and black/white/red "Spectra" (film J) screens.
These panels are common in electronic shelf labels, and ESPHome's built-in
`epaper_spi` and `waveshare_epaper` drivers don't support them.

It was written for an ESP32-S3 price-tag board (silkscreen "epaper 2.13br",
board ID A0726) fitted with an **SE2213JS0E3** panel: 2.13", 212 x 104,
black/white/red.

## Test Hardware

<img width="2864" height="1653" alt="IMG_20261020_192707839 (1)" src="https://github.com/user-attachments/assets/affe3356-d6ad-45d3-85f3-119dae03603c" />
<img width="3499" height="1985" alt="IMG_20261020_192721555 (1)" src="https://github.com/user-attachments/assets/2b7aac51-79cf-40e2-a03c-38c5e4fd060d" />
Sourced from: https://item.taobao.com/item.htm?id=1084689286790

## Usage

```yaml
external_components:
  - source: github://ReflectiveObsidian/esphome-pervasive-epd@main
    components: [pervasive_epd]

spi:
  clk_pin: GPIO12
  mosi_pin: GPIO11

display:
  - platform: pervasive_epd
    id: epd
    model: 2.13in
    cs_pin: GPIO10
    dc_pin: GPIO13
    reset_pin: GPIO14
    busy_pin: GPIO15
    rotation: 90
    update_interval: never
    lambda: |-
      it.fill(Color::WHITE);
      it.filled_rectangle(0, 0, it.get_width(), 24, Color(255, 0, 0));
      it.print(6, 12, id(my_font), Color::WHITE, TextAlign::CENTER_LEFT, "Hello");
```

A full config is in [`example/epaper-pm25.yaml`](example/epaper-pm25.yaml).
It shows a PM2.5 reading from Home Assistant with its EPA category, and turns
the number red above a threshold. It redraws only when:

- the reading moves by more than 5%, at most once every 5 minutes, or
- Home Assistant has been unreachable, or the sensor unavailable, for
  10 minutes. It then shows "No data since HH:MM" once, and clears it when
  data returns.

> **Fonts:** setting `glyphs:` on a font *replaces* ESPHome's default
> character set. Add `glyphsets: [GF_Latin_Kernel]` as well, or every
> character not in your `glyphs:` list is drawn as a box.

## Options

| Option | Default | Notes |
| --- | --- | --- |
| `model` | (required) | `1.54in`, `2.13in`, `2.66in`, `2.71in`, `2.87in`, `3.70in`, `4.17in`, `4.37in` |
| `cs_pin`, `dc_pin`, `reset_pin` | (required) | |
| `busy_pin` | none | LOW = busy on these panels. Without it, fixed delays are used (~20 s per refresh). |
| `rotation` | `0` | Native orientation is portrait. Use `90` or `270` for landscape. |
| `mirror_x`, `mirror_y` | `false` | Use if the image comes out mirrored. |
| `psr` | per model | Panel settings register, two bytes. 2.13" default: `[0xCF, 0x8D]`. |
| `temperature` | `25` | Ambient °C, used by the panel to pick its refresh waveform. |
| `update_interval` | `5min` | Use `never` and trigger refreshes with `component.update`. |

**Colours:**
- Red is drawn where red dominates (e.g. `Color(255, 0, 0)`).
- White is drawn for light colours, black for everything else.
- Always pass a colour explicitly (`Color::BLACK`), because ESPHome's default drawing colour is white.

**Refresh behaviour:**
- A tri-colour refresh takes about 15 s and flashes the panel.
- The driver doesn't block while it waits. An update requested mid-refresh is queued and runs once the current refresh finishes.

## Troubleshooting (from the logs)

A good refresh logs `Panel refresh took ~15000 ms`.

| Log / symptom | Meaning and fix |
| --- | --- |
| `Refresh finished in only N ms` | BUSY isn't being seen. Check the pin, or set `inverted: true` on `busy_pin`. |
| `Panel still reports busy after timeout` | BUSY reads the wrong way round. Set `inverted: true` on `busy_pin`. |
| Mirrored / upside down | Use `mirror_x` / `mirror_y` / `rotation`. |
| Garbled or striped | The panel settings (`psr`) may differ on your panel revision. |

## Protocol

This follows Pervasive Displays' published driver for small iTC screens
([PDLS_EXT3_Basic_Global](https://github.com/PervasiveDisplays/PDLS_EXT3_Basic_Global),
`COG_SmallCJ_*`):

1. Hard reset, then soft reset (`0x00 0x0E`).
2. Temperature (`0xE5`, `0xE0 0x02`), then PSR (`0x00`).
3. Black frame (`0x10`), then red frame (`0x13`).
4. Power on (`0x04`), refresh (`0x12`), DC/DC off (`0x02`).

That library lists revision `JS0C` for the 2.13" Spectra panel. The `JS0E`
panel this was written for is assumed to use the same sequence.
