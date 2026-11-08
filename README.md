# Stavvy

Stavvy is a small desk voice assistant with a cute animated face. Say "Stavrobot", ask
something, and Stavvy shows what it heard, shows the answer as text, and speaks it.

It runs on a [Waveshare ESP32-C6-Touch-AMOLED-2.16](https://www.waveshare.com/esp32-c6-touch-amoled-2.16.htm)
and is the voice and face of [Stavrobot](https://github.com/skorokithakis/stavrobot).


## What it does

- Listens for the wake word "Stavrobot" on the device. Nothing leaves the device until
  it hears the wake word.
- Streams your question to the server, shows the transcript and the reply on screen, and
  plays the spoken reply.
- Idles with a drawn face that breathes, blinks and sometimes looks around.
- Looks surprised when you pick it up and relieved when you put it down.
- Dims the screen after 30 seconds of idle.


## How it fits together

```
Stavvy  <-- ESPHome API -->  havpe-server  <-- webhook -->  Stavrobot
```

- [havpe-server](https://github.com/skorokithakis/havpe-server) does the voice
  pipeline: speech-to-text, text-to-speech (both ElevenLabs) and voice shortcuts.
- Stavrobot is the assistant that answers.
- Stavvy is only the microphone, speaker and screen. It has no API keys.


## Buttons

| Button | Action |
| --- | --- |
| KEY | Mute or unmute the microphone. The face shows when it is muted. |
| BOOT | Change the screen brightness: 40, 60, 80, 100%. Stavvy remembers it. |
| PWR | Short press: on. Long press: off. |

Touching the screen also redraws it, if the screen ever goes black.


## Setup

1. Set up Stavrobot and havpe-server, with havpe-server's webhook pointing at Stavrobot.
2. Build and flash Stavvy (below), with your Wi-Fi details.
3. Start havpe-server. It finds Stavvy on the network by itself, or you can set
   `DEVICE_HOST` to Stavvy's IP address.

Notes:

- Stavvy and havpe-server must be able to reach each other both ways. havpe-server
  connects to Stavvy on port 6053, and Stavvy downloads the spoken reply from
  havpe-server. A VLAN that blocks one direction breaks the replies.
- Stavvy presents itself as a Home Assistant Voice PE, because that is what
  havpe-server looks for. Keep only one such device on the network, or havpe-server
  will not start.
- The connection is not encrypted. Use it on a network you trust.


## Build and flash

You need Docker. Nothing else has to be installed.

Set your Wi-Fi network and password (in "Wi-Fi configuration"):

```sh
docker run --rm -it -v "$PWD":/project -w /project -u "$(id -u):$(id -g)" -e HOME=/tmp \
    espressif/idf:v5.5.3 idf.py menuconfig
```

Connect the board over USB, then build and flash:

```sh
docker run --rm -it --device=/dev/ttyACM0 --group-add dialout -v "$PWD":/project -w /project \
    -u "$(id -u):$(id -g)" -e HOME=/tmp espressif/idf:v5.5.3 \
    idf.py -p /dev/ttyACM0 flash monitor
```

The Wi-Fi details stay in `sdkconfig`, which git ignores.


## Licence

AGPL-3.0. See `LICENSE`.
