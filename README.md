Protogen Head Controller w/ ESP32
Most of this project is just ChatGPT code because I'm a lazy fuck, but it seems to work perfect.
Uses 16x MAX7219 LED Matrixes and a MAX4466 microphone module for animation activation.
Has dropdowns for matrix reassignment/rotation/mirroring so it shouldn't matter in which order the matrixes are wired.
Has a WebUI at 192.168.1.1:8080 with an animation editor, import/export support, and ability for many different animations
Made for use with the M16 MK3 Protogen head but it should work with anything with 4x mouth matrixes, 2x eyes, 1x nose and 1x ear

Wiring:
ESP32 GPIO13 -> DIN module 0
ESP32 GPIO14 -> CLK all modules
ESP32 GPIO15 -> CS/LOAD all modules
ESP32 GPIO4  -> Mic Signal
