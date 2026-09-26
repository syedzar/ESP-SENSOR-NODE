# ESP32 Wireless Sensor Node

An ESP32 project that reads a temperature/pressure sensor over I2C and sends the
readings to a REST API over Wi-Fi. It's written in C++ with FreeRTOS and built
with PlatformIO.

## What it does and why

The simple way to do this would be one loop: read the sensor, send the data,
wait, repeat. The problem is that sending data over Wi-Fi can be slow, and while
the loop is stuck waiting on the network, it isn't reading the sensor.

To fix that, I split the work into two FreeRTOS tasks running on the ESP32's two
cores:

- **sensorTask** reads the sensor every 5 seconds and puts the reading in a
  queue. It never waits on the network.
- **networkTask** takes readings off the queue, keeps Wi-Fi connected, and sends
  each one as JSON with an HTTPS POST.

The queue holds up to 10 readings, so if the network is slow for a bit, the
readings wait there instead of being lost.

![Architecture](docs/images/architecture.svg)

## Demo

I tested it in the Wokwi ESP32 simulator, which simulates the actual chip,
the I2C bus, and Wi-Fi with a real internet connection. The sliders let you
change the sensor values while it runs.

![Wokwi simulator](docs/images/simulator.png)

Serial output showing both tasks working:

![Serial output](docs/images/serial-output.png)

One of the readings received by the API (I used webhook.site for testing):

![Received request](docs/images/webhook-request.png)

## Results

In a 4-minute run, the ESP32 took 51 readings and 50 of them made it to the API
(98%). The one that didn't was a POST that failed with a TLS connection error.
The firmware logged the error and kept going, and the sensor kept reading on
schedule.

## How to run it

1. Install PlatformIO (I used the VS Code extension).
2. Copy `include/config.h.example` to `include/config.h` and fill in your Wi-Fi
   name, password, and the URL to send readings to.

To run it in the simulator, set the Wi-Fi name to `Wokwi-GUEST` with an empty
password, build with `pio run -e esp32dev-sim`, then press F1 in VS Code and
choose "Wokwi: Start Simulator". Wokwi doesn't have the BME280 sensor, so the
simulator version uses a BMP180 instead (no humidity reading).

To run it on a real ESP32 with a BME280, wire SDA to GPIO 21, SCL to GPIO 22,
VCC to 3V3, and GND to GND, then run `pio run --target upload` and
`pio device monitor`.

![Wiring](docs/images/wiring.png)

## Future improvements

- Retry failed POSTs instead of dropping them
- Test it on real hardware with a BME280
- Send the readings to my Engineering Test Data Platform project instead of a
  test webhook
- Verify the server's HTTPS certificate
