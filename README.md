# Alexa WEMO emulation for Mongoose OS

A library for Mongoose OS to emulate a WEMO UPnP plug in Amazon Alexa. It is
possible to emulate more than one plug per device.

## Usage
Add a new instance using
```C
bool alexa_wemo_add_instance(char *name, int relay_pin);
```
where `name` is the friendly name of the emulated device, and `relay_pin` the
GPIO pin to toggle. Returns _true_ if no error occurred. Each instance emulates
a separate device, and there is a limit of 16 instances per device, so a single
esp8266 can emulate max. 16 plugs to toggle max. 16 GPIO pins. Adding more than
two instances per device, however, can currently lead to unstable behaviour.

Due to limitations in Alexas UPnP implementation for WEMO devices, each added
instance will bind to a separate port, starting with the one specified in the
libraries config.

## Acknowledgments
This library is based on a blog post over at [makermusings.com](http://www.makermusings.com/2015/07/13/amazon-echo-and-home-automation/)
and code from [kakopappas Alexa WEMO switch](https://github.com/kakopappa/arduino-esp8266-alexa-wemo-switch).
