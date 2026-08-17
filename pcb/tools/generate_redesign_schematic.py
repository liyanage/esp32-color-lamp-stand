#!/usr/bin/env python3
"""Generate the ESP32-S3/JLCPCB redesign schematic."""

from pathlib import Path
import os

import kicad_sch_api as ksa
import kicad_sch_api.library.cache as library_cache


PROJECT_ROOT = Path(__file__).resolve().parents[2]
OUTPUT = PROJECT_ROOT / "pcb" / "esp32-color-lamp-stand.kicad_sch"
SYMBOL_DIR = Path("/Applications/KiCad/KiCad.app/Contents/SharedSupport/symbols")
CACHE_DIR = Path("/tmp/esp32-color-lamp-kicad-cache")


def configure_libraries():
    os.environ["KICAD_SYMBOL_DIR"] = str(SYMBOL_DIR)
    cache = library_cache.SymbolLibraryCache(CACHE_DIR)
    cache.discover_libraries([SYMBOL_DIR])
    library_cache._global_cache = cache


def add_part(schematic, lib_id, reference, value, position, footprint, lcsc, manufacturer, mpn, rotation=0):
    part = schematic.components.add(
        lib_id=lib_id,
        reference=reference,
        value=value,
        position=position,
        footprint=footprint,
        rotation=rotation,
        **{
            "LCSC": lcsc,
            "Manufacturer": manufacturer,
            "Manufacturer Part Number": mpn,
        },
    )
    part._data.hidden_properties.update({"LCSC", "Manufacturer", "Manufacturer Part Number"})
    return part


def label_pin(schematic, reference, pin, net):
    schematic.add_label(text=net, pin=(reference, str(pin)))


def label_pins(schematic, reference, assignments):
    for pin, net in assignments.items():
        label_pin(schematic, reference, pin, net)


def mark_unused_pins(schematic, component, pins):
    seen = set()
    for pin in pins:
        pin_data = next(item for item in component._data.pins if item.number == str(pin))
        position = (
            component.position.x + pin_data.position.x,
            component.position.y - pin_data.position.y,
        )
        key = position
        if key not in seen:
            schematic.no_connects.add(position=position)
            seen.add(key)


def add_resistor(schematic, reference, value, position, lcsc, net1, net2, rotation=0):
    part = add_part(
        schematic,
        "Device:R",
        reference,
        value,
        position,
        "Resistor_SMD:R_0603_1608Metric",
        lcsc,
        "UNI-ROYAL",
        value,
        rotation,
    )
    label_pins(schematic, reference, {"1": net1, "2": net2})
    return part


def add_capacitor(schematic, reference, value, position, footprint, lcsc, net1, net2):
    part = add_part(
        schematic,
        "Device:C",
        reference,
        value,
        position,
        footprint,
        lcsc,
        "Samsung Electro-Mechanics",
        value,
    )
    label_pins(schematic, reference, {"1": net1, "2": net2})
    return part


def build_schematic():
    configure_libraries()
    schematic = ksa.create_schematic("esp32-color-lamp-stand")
    schematic._data["paper"] = "A3"

    esp = add_part(
        schematic,
        "RF_Module:ESP32-S3-WROOM-1",
        "U1",
        "ESP32-S3-WROOM-1-N8R8",
        (150, 75),
        "RF_Module:ESP32-S3-WROOM-1",
        "C2913201",
        "Espressif Systems",
        "ESP32-S3-WROOM-1-N8R8",
    )
    label_pins(
        schematic,
        "U1",
        {"1": "GND", "2": "+3V3", "3": "EN", "13": "USB_D-", "14": "USB_D+", "21": "LED_CPU", "27": "BOOT", "40": "GND", "41": "GND"},
    )
    mark_unused_pins(schematic, esp, [4, 5, 6, 7, 8, 9, 10, 11, 12, 15, 16, 17, 18, 19, 20, 22, 23, 24, 25, 26, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39])

    usb = add_part(
        schematic,
        "Connector:USB_C_Receptacle_USB2.0_16P",
        "J1",
        "TYPE-C-31-M-12",
        (35, 65),
        "Connector_USB:USB_C_Receptacle_HRO_TYPE-C-31-M-12",
        "C165948",
        "Korean Hroparts Elec",
        "TYPE-C-31-M-12",
    )
    label_pins(
        schematic,
        "J1",
        {
            "A1": "GND", "A4": "+5V", "A5": "CC1", "A6": "USB_CONN_D+", "A7": "USB_CONN_D-", "A9": "+5V", "A12": "GND",
            "B1": "GND", "B4": "+5V", "B5": "CC2", "B6": "USB_CONN_D+", "B7": "USB_CONN_D-", "B9": "+5V", "B12": "GND", "SH": "GND",
        },
    )
    mark_unused_pins(schematic, usb, ["A8", "B8"])

    add_resistor(schematic, "R1", "5.1k", (70, 48), "C23186", "CC1", "GND")
    add_resistor(schematic, "R2", "5.1k", (85, 48), "C23186", "CC2", "GND")

    esd = add_part(
        schematic,
        "Power_Protection:USBLC6-2SC6",
        "U2",
        "USBLC6-2SC6",
        (75, 72),
        "Package_TO_SOT_SMD:SOT-23-6",
        "C7519",
        "STMicroelectronics",
        "USBLC6-2SC6",
    )
    label_pins(schematic, "U2", {"1": "USB_CONN_D+", "2": "GND", "3": "USB_CONN_D-", "4": "USB_PROT_D-", "5": "+5V", "6": "USB_PROT_D+"})
    add_resistor(schematic, "R3", "22R", (100, 65), "C23345", "USB_PROT_D+", "USB_D+", rotation=90)
    add_resistor(schematic, "R4", "22R", (100, 80), "C23345", "USB_PROT_D-", "USB_D-", rotation=90)

    ldo = add_part(
        schematic,
        "Regulator_Linear:AP2112K-3.3",
        "U3",
        "AP2112K-3.3TRG1",
        (80, 112),
        "Package_TO_SOT_SMD:SOT-23-5",
        "C51118",
        "Diodes Incorporated",
        "AP2112K-3.3TRG1",
    )
    label_pins(schematic, "U3", {"1": "+5V", "2": "GND", "3": "+5V", "5": "+3V3"})
    mark_unused_pins(schematic, ldo, [4])
    add_capacitor(schematic, "C1", "10uF", (55, 112), "Capacitor_SMD:C_0805_2012Metric", "C15850", "+5V", "GND")
    add_capacitor(schematic, "C2", "10uF", (105, 112), "Capacitor_SMD:C_0805_2012Metric", "C15850", "+3V3", "GND")
    add_capacitor(schematic, "C3", "100nF", (125, 112), "Capacitor_SMD:C_0603_1608Metric", "C14663", "+3V3", "GND")
    add_capacitor(schematic, "C4", "10uF", (145, 112), "Capacitor_SMD:C_0805_2012Metric", "C15850", "+3V3", "GND")
    add_capacitor(schematic, "C5", "1uF", (165, 112), "Capacitor_SMD:C_0603_1608Metric", "C15849", "EN", "GND")
    add_capacitor(schematic, "C6", "220uF", (185, 112), "Capacitor_SMD:C_Elec_5x5.8", "C43320", "+5V", "GND")

    add_resistor(schematic, "R5", "10k", (125, 135), "C25804", "+3V3", "EN")
    add_resistor(schematic, "R6", "10k", (145, 135), "C25804", "+3V3", "BOOT")
    add_part(schematic, "Switch:SW_Push", "SW1", "RESET", (165, 132), "Button_Switch_SMD:SW_Push_1P1T_XKB_TS-1187A", "C318884", "XKB Connection", "TS-1187A-B-A-B")
    add_part(schematic, "Switch:SW_Push", "SW2", "BOOT", (185, 132), "Button_Switch_SMD:SW_Push_1P1T_XKB_TS-1187A", "C318884", "XKB Connection", "TS-1187A-B-A-B")
    label_pins(schematic, "SW1", {"1": "EN", "2": "GND"})
    label_pins(schematic, "SW2", {"1": "BOOT", "2": "GND"})

    add_resistor(schematic, "R7", "33R", (50, 152), "C23140", "LED_CPU", "LED_DIN", rotation=90)
    add_resistor(schematic, "R8", "10k", (70, 152), "C25804", "LED_DIN", "GND")

    for index in range(32):
        row, column = divmod(index, 16)
        x = 25 + column * 23
        y = 165 + row * 28
        reference = f"D{index + 1}"
        data_in = "LED_DIN" if index == 0 else f"LED_D{index}"
        data_out = f"LED_D{index + 1}"
        led = add_part(
            schematic,
            "LED:WS2812B",
            reference,
            "XL-5050RGBC-2812B-S",
            (x, y),
            "LED_SMD:LED_WS2812B_PLCC4_5.0x5.0mm_P3.2mm",
            "C22461793",
            "XINGLIGHT",
            "XL-5050RGBC-2812B-S",
        )
        led._data.hidden_properties.add("Value")
        label_pins(schematic, reference, {"1": "+5V", "3": "GND", "4": data_in})
        if index == 31:
            mark_unused_pins(schematic, led, [2])
        else:
            label_pin(schematic, reference, 2, data_out)
        add_capacitor(
            schematic,
            f"C{index + 7}",
            "100nF",
            (x + 9, y + 9),
            "Capacitor_SMD:C_0603_1608Metric",
            "C14663",
            "+5V",
            "GND",
        )

    schematic.components.add(lib_id="power:PWR_FLAG", reference="#FLG01", value="PWR_FLAG", position=(35, 112))
    label_pin(schematic, "#FLG01", 1, "+5V")
    schematic.components.add(lib_id="power:PWR_FLAG", reference="#FLG02", value="PWR_FLAG", position=(45, 112))
    label_pin(schematic, "#FLG02", 1, "GND")

    test_points = [("TP1", "+5V", 200, 48), ("TP2", "+3V3", 200, 58), ("TP3", "GND", 200, 68), ("TP4", "LED_DIN", 200, 78)]
    for reference, net, x, y in test_points:
        test_point = add_part(schematic, "Connector:TestPoint", reference, net, (x, y), "TestPoint:TestPoint_Pad_D1.5mm", "DNP", "DNP", "DNP")
        test_point._data.in_bom = False
        label_pin(schematic, reference, 1, net)

    schematic.save(OUTPUT, preserve_format=False)


if __name__ == "__main__":
    build_schematic()
