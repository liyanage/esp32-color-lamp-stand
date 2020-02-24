
# Stand Base Model

https://a360.co/36blYHf

# Initial WiFi Configuration

Follow these steps to configure the WiFi network information.

1.) Plug in the USB-C cable to your Mac.

2.) Find out the serial device path with

    ls -l /dev/cu.usbserial*
    
That gives you something like "/dev/cu.usbserial-1410"

3.) Run the following "screen" command to connect to the lamp's serial console:

    screen /dev/cu.usbserial-1410 115200

4.) Immediately after typing that command the microcontroller will reboot when the
serial connection opens. Immediately press any key a few times to enter the
configuration menu. If you missed it, disconnect the screen session as shown in
step 7 and try again.

5.) Once you're in the configuration menu, choose "w" to set the Wifi settings.
Enter your WiFi name and password when prompted.

6.) Choose "r" from the main menu to reboot. Check the log messages after the
reboot to ensure the device can connect to WiFi successfully.

7.) Disconnect the screen session with Ctrl-a followed by Ctrl-\ .

![This screen video shows an example](docs/readme-resources/esp32-color-lamp-stand-config-menu.gif)