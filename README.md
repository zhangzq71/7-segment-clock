# 7-segment-clock

This is my version of leonardlee's great 3D printed 7-Segment Clock with two major changes.

1. I had trouble with the FastLED driver getting interrupted during data transmission (resulting in flickering) and with interrupts disabled the WDT would error and reboot the system everytime the clock synchronized with the NTP server.  To fix this I switched to the NeoPixelBus library which supports DMA transfer.  Now interrupts can remain enabled and the transmission won't be affected.

1. I also incorporated a photoresistor to allow the clock to dim itself when the room is dark.  I found the light to be too bright when I was going to bed and got tired of changing the brightness every night.

The changes WILL NOT work with leonardlee's PCB.  I found that I do not need a level shifer IC for my LED strip so the wiring is easy/clean enough without a board to mount on.  

# Hardware Changes from Original
1. Photoresistor installed next to the power jack (barrel plug) with one end connected to 3.3V and the other end connected to A0 with a pull-down resistor to GND.  Choose the resistor value that gives you an analog reading of about 50% during the day (or lights on) for your particular photoresistor.

1. Convert the two individual strips into one by running a wire from data out of the last pixel of the hours side (far left) to the data in of the first pixel of the minutes side (center).  Just follow the path of the other wires creating the digits and use a little glue if necessary to keep the wire from laying on top of any pixels.  The pin of the Wemos D1 Mini used for the data line must be the RX pin, as this is the only pin that supports DMA transfer.  The power lines can be kept as is since it is fine to power the strip from multiple points.

# Software Changes from Original
1. Auto dim setting (Moon Icon) added which allows you to adjust the brightness of the room that will trigger the auto dimming feature.  Moving this slider up will make it more sensitive to light and cause the clock to dim sooner, and moving it down will make it less sensitive and dim only in darker conditions.  If you move the slider all the way down, the auto dimming feature will be disabled.

1. Reset WiFi Settings button added which will forget your WiFi settings and force the ESP8266 to boot as an access point so that you can connect to it and reconfigure the network settings. 

# Installation
1. Install the [Arduino IDE](https://www.arduino.cc/en/Main/Software)
1. Install the [CH340G driver](https://wiki.wemos.cc/tutorials:get_started:get_started_in_arduino)
1. In the preferences dialog of the Arduino IDE, add http://arduino.esp8266.com/stable/package_esp8266com_index.json to the Additional Boards Manager URLs
1. Open Boards Manager (under Tools), search for and install the latest version of esp8266.
1. Install libraries using the Library Manager (Sketch > Include Library > Manage Libraries):
    - [WiFiManager](https://github.com/tzapu/WiFiManager)
    - [ArduinoJSON](https://arduinojson.org) (version 5.x, not 6.x!)
    - [NeoPixelBus](https://github.com/Makuna/NeoPixelBus)
1. Install [PaulStoffregen's Time library](https://github.com/PaulStoffregen/Time):
    - Download the library as a zip (master branch) https://github.com/PaulStoffregen/Time/archive/master.zip
    - Sketch > Include Library > Add .ZIP library...
    - Open the Time-master directory in your sketchbook location (find it under Preferences > Sketchbook location)
    - rename Time.h to \_Time.h (this is to avoid a [conflict with time.h](https://github.com/mikalhart/IridiumSBD/issues/16) on some OSs)
1. Install the [Arduino ESP8266 filesystem uploader plugin](https://github.com/esp8266/arduino-esp8266fs-plugin) (requires restart of IDE)
1. Connect your D1 mini to the computer using a micro USB cable
1. Under Tools, select "LOLIN(WEMOS) D1 R2 & mini" as the board
1. Open [clock.ino](Arduino/clock.ino)
1. Sketch > Upload
1. Upload the sketch data files (the [data](Arduino/data) directory in the sketch folder) using Tools > ESP8266 Sketch Data Upload.

# Initial Configuration
1. When the sketch runs for the first time, the D1 mini will try to connect to your wifi and fail (because it doesn't have any previously saved wifi credentials). This will cause it to start up an access point, which serves up a captive configuration portal (thanks to [WiFiManager](https://github.com/tzapu/WiFiManager).
1. You can connect to this access point to select and enter credentials for your network
1. You will also enter your [ipstack.com](https://ipstack.com/) API key and [Google Time Zone](https://developers.google.com/maps/documentation/timezone/get-api-key)/[Google Maps Javascript](https://developers.google.com/maps/documentation/javascript/get-api-key) API key on this screen
1. Save (the D1 mini will restart and connect to your wifi)

# Runtime Configuration
1. While connected to the same network as the D1 mini, open a browser and go to http://\<IP of D1 mini> (you can find the D1 mini's IP address using your router).

![web UI](web.png)
