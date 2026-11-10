# Sentinel Glance – GHL

A four-tile, at-a-glance display for GHL aquarium gear on an M5Stack Core2. It reads up to six devices over your local Wi-Fi:

- **ProfiLux controllers:** ProfiLux Mini, ProfiLux Light Wi-Fi and ProfiLux 4. They share one Wi-Fi protocol.
- **GHL Master Doser 2.1:** container levels per pump.

It's read-only. It sends register reads only and never writes to a controller. Nothing goes to the cloud.

Tested on a ProfiLux Mini (firmware 1.23) and one Master Doser 2.1. The ProfiLux 4 and Light follow the same register layout per GHL Connect's code, but haven't been tested yet. Reports are welcome.

## Screens
- **Home:** clock, hamburger menu, an alarm bell with a count, and two pages that alternate every 10 s.
  - **Page 1:** tiles 1 and 2 (temperature and pH by default), with one graph below spanning both. Tile 1 uses the right scale and tile 2 the left, each in its tile's colour. Tap the graph to switch between 1 h, 6 h and 24 h. Tap a tile for its own graph.
  - **Page 2 (Doser):** every pump you mark Show, across all dosers, up to six rows. Each row shows % full and a bar, and turns red below its low %.
  - Tap the dots under the clock bar to switch pages. Any touch pauses the alternation for a minute.
- **Graph:** 1 h / 6 h / 24 h / 7 d / 90 d, with low / high / average and dashed limit lines.
- **Menu:** Alarms, Display settings, Alarm settings, Network & setup, About. The left touch button under the screen also works as Back.
- **Display settings** (tap a row to change it): screen brightness, page change (Off / 5 / 10 / 15 / 30 / 60 s), 12/24-hour clock, night mode (Off / Dim / Screen off), night hours, and Delete history (tap twice).
- **Alarm settings:** red glow on alarm, amber glow when a controller stops answering, glow brightness, beep on new alarm.

## First setup
1. Install firmware with one of these:
   - **From source:** plug the Core2 into a Mac or Linux PC and run `./build_and_flash.sh`. The first run installs PlatformIO into `./.venv`.
   - **Prebuilt:** download `sentinel-glance-full.bin` from the latest [release](../../releases/latest) and flash it at address `0x0` with [esptool](https://github.com/espressif/esptool) or the [ESP Web Tool](https://espressif.github.io/esptool-js/).
2. The Core2 opens a Wi-Fi hotspot, **GHL-Glance-xxxx**. Join it and open **192.168.8.1**.
3. Pick your network and save. It restarts and joins it.
4. Open `http://<glance-ip>/` (the IP is under Menu → Network & setup) and add your controllers. For each one, set:
   - **Type:** ProfiLux or Doser 2.1.
   - **Name.**
   - **IP address:** give each device a fixed IP in your router.
   Then set up the tiles.

The defaults are one ProfiLux (tiles Temp and pH) and one Doser (tiles Pump 1 and Pump 2), with the addresses left blank. The time zone is US Eastern. The setup page lists other common zones.

### Tiles
Each tile reads one value from one controller:
- **pH** and **Temperature °F / °C:** sensor 1 and sensor 2 on a ProfiLux Mini.
- **Sensor N:** sensor N's live value on any ProfiLux, at register 10000 + 8 × (N − 1). Set its scale under Divide raw by: pH 100, temperature 10, most others 10.
- **Wi-Fi signal**, or a **custom register**.
- **Doser pump % full.**

**Alarm sensors** lists the sensor numbers whose controller alarm turns the tile red, for example `1` or `1,3`. The controller's sensor alarm mask (register 10700) has one bit per sensor. You can also set **local limits** (low/high) on any non-doser tile.

Tiles 3 and 4 aren't drawn. They're recorded to the history CSV and can raise alarms.

## Alarms
A tile turns red, the bell counts it and the glow pulses red when either:
- the controller's sensor alarm mask has a bit set for one of the tile's alarm sensors, or
- the reading is outside the tile's local low/high limits, if those are on.

Other alarms that show in the alarm list:
- **Level / leak alarms** (register 10701).
- **Doser pumps below their low %.**

A controller that hasn't answered for three of its turns (at least 3 minutes) greys its tiles and turns the glow amber. Settings can switch the amber glow off.

## Firmware updates (OTA)
On the setup page, under **Firmware updates**, there are two ways to update:
- **Check GitHub / Install update:** reads this repo's latest release, then downloads its `firmware.bin` over verified HTTPS and installs it. Root certificates are in `src/ota_certs.h`.
- **Upload and install:** installs a `firmware.bin` from your computer. Use it with a local build or if GitHub can't be reached.

Settings and history survive an update. Installs only happen when you press a button. The optional daily check just shows "Update available" under About.

The setup page has no password. Anyone on your LAN can change settings or install firmware, so keep the Glance on a trusted network.

**Publishing a release:** bump `FW_VERSION` in `src/main.cpp`, commit, then tag it (`git tag v0.5.1 && git push --tags`). GitHub Actions builds the firmware and attaches these files to the release:
- `firmware.bin`, which OTA uses.
- `sentinel-glance-full.bin`, the full image for blank boards.
- `SHA256SUMS`.

The build fails if the tag doesn't match `FW_VERSION`.

## Alarm light (glow back)
The Core2's stock battery bottom is replaced by `case/glance_back.stl`, printed in natural or translucent PETG. A DIYmall WS2812 Jewel x7 (23 mm) sits inside, LEDs facing out, and the whole back glows onto the wall behind the stand:
- red breathing pulse on an alarm,
- steady amber when a controller stops answering,
- off otherwise.

Power comes in through the back too, so nothing sticks out of the Core2's sides.

**Build**
1. Remove the four M3 countersunk screws on the back and lift off the stock bottom. Unplug the battery and lift it out of the stock bottom. It moves into the stand base (see Stand).
2. Solder three wires to the M-Bus pins. Check the pin numbers against M5Stack's Core2 M-Bus diagram before soldering:
   - pin 28 (5V) → Jewel **5V DC Power**
   - pin 1 (GND) → Jewel **GND**
   - pin 8 (G25) → Jewel **Data Input** (leave Data Out empty)
3. Power: cut the device end off a USB-A cable and plug it into any 5 V / 2 A USB charger.
   - Feed the cable in through the 4.4 mm entry hole in the corner of the bay.
   - Red (+5 V) → M-Bus pin 28, black (GND) → pin 1. These are the same points as the Jewel.
   - Cut back and insulate the white and green data wires.
   - Hot-glue or zip-tie the cable to the collar inside the hole for strain relief.
4. Battery: extend the battery's red and black leads by about 150 mm (26 AWG) so they reach from the stand base to the Core2.
   - Keep the original JST plug on the Core2 end.
   - Feed the plug end in through the 3.4 mm hole next to the power entry and plug it into the Core2's battery socket.
   - Heat-shrink each joint separately.
5. Pass the M-Bus wires through the 4 mm hole between the M-Bus pocket and the old battery bay. Drop the Jewel LEDs-down into its ring with a dab of hot glue on the rim. Fit the back and refit the four screws.
6. Firmware: "Power input" on the setup page defaults to **5 V into the back**, and in that mode the Core2 does not drive pin 28.
   - If you ever run it from the side USB-C instead, switch the setting to USB-C first, or the Jewel won't light.
   - Unplug the back power before plugging USB-C in for flashing.

Red at the highest glow setting draws about 140 mA from the Core2's 5 V rail.

WS2812 wants data close to its supply voltage, and the Core2 sends 3.3 V. If the Jewel flickers, shows wrong colours or stays dark, put a 1N4001 (or any silicon diode) in series on the 5 V wire, stripe toward the Jewel. That drops its supply to about 4.3 V, which makes 3.3 V data reliable.

**About the back:** it's built from M5Stack's published Core2 model, `M5_Hardware/Products/K010_Core2/Structures/Core2.stl` in [m5stack/M5_Hardware](https://github.com/m5stack/M5_Hardware).
- Unchanged from the stock bottom: the outline, screw holes, the rim that locates in the frame, the PCB pegs and the M-Bus pocket.
- Changed: the battery bay is emptied and its floor holes are closed, the floor under the Jewel is a 0.8 mm window, and there's a locating ring and the wire hole.

Printing:
- Floor-down, no supports.
- 0.15–0.2 mm layers.
- 2 perimeters and 15 % infill keep it translucent.

To regenerate it with `case/glance_back.py` (Python with trimesh and manifold3d), first export the stock bottom shell from that model as `case/core2_stock_bottom.stl`. M5Stack's file isn't redistributed here.

The NeoPixel output and the pixel count are on the setup page. The output is inside on M-Bus pin 8 (G25) by default, or on Port A (G32 / G33).

## Stand
`case/glance_stand.stl`, made by `case/glance_stand.py`. Edit the numbers at the top of the script and rerun it.
- Holds the Core2 at 15° back on a lipped shelf.
- The back plate is an open frame, so the glow back lights the wall and the power cable passes straight out through it. The cable drops behind the plate into a groove across the base and leaves through the rear notch.
- Sides are open for the power button and USB-C.
- The Core2's battery (YS 602535, 500 mAh, 6 × 25 × 35 mm plus its protection board) lives in a pocket under the base.
  - The pocket is closed by `case/glance_battery_cover.stl` with two M3 screws: 6–8 mm self-tapping, or the same countersunk M3s.
  - The battery leads come up through a hole into the cable groove and run up behind the plate with the power cable.
- 69 × 55 × 62 mm.
- Print as modelled (base down), PETG, no supports. The pocket roof is a 26 mm bridge, which PETG handles at normal bridge settings. Print the cover flat, countersinks up.
- Place it 5–15 cm from a light-coloured wall for an even halo.

## Protocol
**ProfiLux (Mini, Light Wi-Fi, ProfiLux 4)**
1. `GET http://<device>/api_wifi/v1/status` returns the MAC address and serial number.
2. The 8-byte token is the last four MAC bytes reversed, then the serial number as a little-endian uint32.
3. Reads go over a binary WebSocket at `ws://<device>/ws`. `src/ghl_codec.h` documents the frames.

Live registers:

| Register | Value |
| --- | --- |
| 10000 + 8(N−1) | sensor N |
| 10005 + 8(N−1) | sensor N in its alternate unit (°F for temperature) |
| 10628 | Wi-Fi dBm |
| 10700 | sensor alarm mask |
| 10701 | level alarm mask |

**Master Doser 2.1:** the Glance opens TCP 10001, sends a 13-byte request for each pump it shows, and decodes the echoed field's nibble-encoded mL value (variable width, up to 3,000 L). Two tries per pump. The doser has no alarm register the Glance reads, so doser tiles alarm only on their low %. The request frames were captured from one doser. If yours doesn't answer, open an issue with a capture.

**Polling:** one controller per 30 s slot, in turn: with two controllers each is read once a minute, with six every 3 minutes.
- 15 s reply watchdog.
- Reconnect backoff 2/5/10/15/30 s.
- Full re-identify after 2 minutes past offline.

`GET http://<glance-ip>/api/status` returns the tiles, controllers and alarm state as JSON, which is handy for Home Assistant's REST sensor.

`test/codec_test.cpp` checks the ProfiLux frame builder and parser, and the doser decoder. Run it with `g++ -std=c++17 test/codec_test.cpp && ./a.out`.

## Night mode
Night mode saves the backlight, the one part that wears from running 24/7. The IPS screen itself doesn't burn in.
- Between the night hours (default 22:00–07:00) the screen dims to about 5 % or turns off.
- A touch or button press wakes it for a minute. That first touch does nothing else.
- Any alarm brings it straight to full brightness.
- The glow back is not affected.

Mode and hours are in Settings. Exact hours can be typed on the setup page.

## History
- 7 days at 1-minute detail and 90 days of 15-minute averages for each tile, about 300 KB in memory.
- Saved to the Core2's internal flash (LittleFS on the 3.4 MB storage partition):
  - minute data every 10 minutes,
  - averages every 15 minutes.
  A power cut loses at most the last 10 minutes and shows as a gap.
- Nothing is recorded until the clock is set from the network, so every point has a real time.
- The log files are rewritten to their window size whenever they grow past it, so they never fill the flash.
- Download as CSV from the setup page (7-day 1-minute or 90-day 15-minute). Delete it from Display settings on the device (tap twice).
- Pointing a tile at a different reading on the setup page starts that tile's history fresh.

## Hardware cost
About $58 delivered for the Core2, the Jewel and the printed parts.

---
By TelegrahamTested. Not affiliated with GHL. GHL, ProfiLux and Master Doser are GHL Advanced Technology trademarks.
