# Version 2 hardware design

Version 2 replaces the original hand-assembled controller with a board intended
for complete JLCPCB assembly. Component selections and their LCSC identifiers
are stored in the schematic and exported into the manufacturing BOM.

## Controller and USB

The controller is an ESP32-S3-WROOM-1-N8R8 module. Its native USB interface is
connected to the USB-C receptacle through U2, a USBLC6-2SC6 USB ESD-protection
device. This removes the separate USB-to-serial IC used by the older design.

The module antenna intentionally extends beyond the PCB edge. Keep copper,
components, and enclosure material away from its antenna region; do not pull
the module inward merely to fit its body within the outline.

U3, an AP2112K-3.3 regulator, generates the 3.3 V rail from USB 5 V. The ESP32
module does not contain a regulator that accepts USB 5 V directly.

Each USB-C CC pin has the required 5.1 kOhm sink resistor. R9-R12 divide the CC1
and CC2 voltages for ESP32 ADC inputs. These readings indicate whether the
source advertises default current, 1.5 A, or 3 A; they do not measure the
board's actual current consumption. C42 and C43 provide local ADC filtering.

## LED power and data

The addressable LEDs operate from `LED_5V`. U5, a TPS22975 load switch, connects
USB `+5V` to `LED_5V` with controlled rise time. C40 programs its rise time,
C41 is its local input bypass capacitor, and C6 supplies bulk capacitance on the
switched LED rail.

U4, a 74AHCT1G125 powered from `LED_5V`, buffers and level-shifts the ESP32's
3.3 V LED data output. R7 is the series resistor between the buffer and the LED
chain. R8 holds the ESP32-side data signal low during reset or startup.

Each LED has a local 100 nF bypass capacitor. The firmware should still limit
brightness and total LED current to what the connected USB-C source advertises.

## Layout and assembly

The PCB is a two-layer design with filled GND zones on F.Cu and B.Cu and GND
stitching vias. The large routed openings and the external outline are part of
the mechanical design. The USB-C connector and ESP32 antenna placements are
edge-dependent and intentional.

All fitted components are on the top side. TP1-TP5 are bare test pads and are
excluded from the JLCPCB BOM and CPL. The project-local symbol library contains
the TPS22975 symbol required to open the schematic without external libraries.

Generate release files with:

```sh
make check
make manufacturing
```

The upload-ready outputs are written to `manufacturing/jlcpcb/`. The v2.0.0
release design is recorded by Git tag `v2.0.0`.
