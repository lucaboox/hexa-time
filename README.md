# Hexa Time

A fast Pebble watchface: big time in the middle, AM/PM under it, and up to 6
configurable complications (3 on top, 3 on the bottom).

Platforms: basalt (Time), chalk (Time Round), diorite (Pebble 2), emery (Time 2),
flint (Pebble 2 Duo), gabbro (Round 2).

## Complications

| Option | Shows |
| --- | --- |
| Date stacked | `SAT` / `10` / `OCT` |
| Day + date | `SAT` / `10` |
| Date | `10 OCT` |
| Weather: icon + temp | (optionally colored) sun / moon / partly cloudy / cloud / rain / snow / thunder / fog icon, temp under it |
| Weather: icon only / temp only / high-low | |
| Battery: icon + %, icon only, % only | fill level, charging bolt, optional colors: green, orange at 50%, red at 20% |
| Steps, Distance, Sleep, Active calories, Heart rate, Active time | Pebble Health (today's totals) |
| Bluetooth | rune, crossed out (red) when disconnected |

Set a slot to **None** and the others in that row spread out to fill it. If a whole
row is None, the time stays centered and the other row sits at the screen edge (or,
with "Group time with complications" on, the two re-center together in the middle).

## Settings (phone app, gear icon)

- Time font (Bold, Light, Digital, Digital bold, Large), leading zero on hours, AM/PM on/off
- Colors: background, time, AM/PM, complication text, complication icons
- 12h/24h follows the watch's system setting
- Celsius/Fahrenheit, km/miles, weather update interval, optional city instead of GPS
- Vibrate on Bluetooth disconnect

Weather comes from [Open-Meteo](https://open-meteo.com) (free, no API key).

## Why it's fast

- Redraws once a minute only (no seconds), on a single layer
- Every string is formatted when its data changes, never inside the draw call
- Health/battery/weather updates only redraw when a displayed value actually changed
- Icons are drawn with vector primitives (no bitmaps to load), ~10 KB RAM total

## Build

```bash
pebble build
pebble install --emulator basalt
```

`src/pkjs/clay.js` is the Clay config library (MIT, see `CLAY_LICENSE`), bundled
directly because the npm package doesn't list the newer platforms.
