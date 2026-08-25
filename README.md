
# User Guide

## Hardware Installation

1.) You'll need a USB-C power source, either your Mac or a USB-C wall charger
that can provide at least 1.5A. I've had good results with this one from Anker:
https://www.amazon.com/dp/B07GWN4PGL.

2.) You'll also need a USB-C cable, intially for configuration of the WiFi
information, after that just for power. You should tie a knot into the cable
on the inside of the base for strain relief as shown in the following photo.

![Knot in USB-C cable for strain relief](docs/readme-resources/IMG_2356.jpg)

3.) Connect the USB-C connector to the plug on the board as shown
in the following photo.

![USB-C cable plugged in](docs/readme-resources/IMG_3976.jpg)

Put the translucent cover on top and put your object
to be illuminated on top of the cover.

## Wi-Fi Configuration

On first use, the lamp pulses amber and creates a temporary Wi-Fi network named
`ColorLamp-Setup-XXXX`.

1. Connect a phone or computer to that network.
2. The setup page should open automatically. If it does not, open
   `http://192.168.4.1` in a browser.
3. Select the home Wi-Fi network, enter its password, and choose **Connect**.
4. The lamp pulses blue while testing the connection. A green confirmation means
   the credentials were saved and the lamp will restart. Red means the connection
   failed; correct the password and try again.

To move the lamp to another network, let it finish starting and then hold the
**BOOT** button for five seconds. The lamp turns red, erases only its Wi-Fi
credentials, and restarts in setup mode. Do not hold BOOT while applying power or
pressing RESET, because BOOT also selects the ESP32 download mode during reset.


# How it's Made

## Enclosure

CAD model: https://a360.co/36blYHf

... more to come

## PCB

... more to come

## Software

The firmware targets the ESP32-S3-WROOM-1-N8R8 and ESP-IDF 6.0.2. Activate
that ESP-IDF installation, then build and flash over the board's native USB
connection:

    . "$HOME/esp/esp-idf-v6.0.2/export.sh"
    idf.py build
    idf.py flash monitor

At startup the firmware measures the USB-C CC1 and CC2 voltages using GPIO4
and GPIO5. It classifies the source's advertised current as USB default,
1.5 A, or 3 A and limits the 32 LEDs accordingly. If ADC calibration is not
available, it uses the conservative USB-default limit.
