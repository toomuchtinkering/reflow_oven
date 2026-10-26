# Web tuning UI

Source files:
- `tuning.html` — dashboard markup + CSS (layout C)
- `app.js` — polling, run-lifecycle, canvas chart, archive, verification overlay, drag-to-refine
- `fit.js` — pure ZN reaction-curve fit math (node-tested: `node --test test/web/`)
- `align.js` — aligns run traces onto uPlot's shared x axis (node-tested)
- `html.js` — `escapeHtml` for archive rows built from imported files (node-tested)

## Build (automatic)

`tools/build_web_asset.py` inlines `html.js`, `align.js`, `fit.js` and `app.js` into `tuning.html`, gzips
the result, and emits `tuning_html_gz.c` — the gzipped page as a `const unsigned
char[]` + `_len`. That `.c` is compiled in via the normal `src/*.c` glob, and
`ota_server.c` serves it at `GET /` with `Content-Encoding: gzip`.

A PlatformIO **pre-build hook** (`extra_scripts = pre:tools/pio_gen_web_asset.py`
in `platformio.ini`) runs the generator before every `pio run`, so editing any
file here can't ship a stale embedded page. The generator only rewrites the `.c`
when its bytes change, so an unchanged page costs no recompile.

To regenerate manually (e.g. to check the size):

```
python3 tools/build_web_asset.py
```

It prints the uncompressed and gzipped sizes. Keep the gzipped size well under
the app-slot headroom (see spec §6 — currently ~7.6 KB gz against ~66 KB free).

## Local preview (no hardware)

`tools/web_sim.py` serves these files and mocks the device HTTP API from a
simple thermal model, so the UI can be exercised in a browser before flashing:

```
python3 tools/web_sim.py        # then open http://localhost:8000/
```

Drive runs from the terminal (letter + Enter): `s` step test, `r` reflow run,
`f` trip a fault, `c` confirm suggested gains (touchscreen stand-in), `q` quit.
Time runs at `--speed` (default 4×); `--duty` / `--ceiling` / `--port` override
the defaults. It listens on `127.0.0.1` only; `--bind 0.0.0.0` opens it to the
network, and with `--device` that relays the oven API too. Stdlib-only; it does
not touch firmware.

> Note: this project embeds the page as a generated C array rather than CMake
> `EMBED_FILES` / `board_build.embed_files`, because both toolchain embed paths
> are unreliable under PlatformIO's ESP-IDF integration. The generated
> `tuning_html_gz.c` and the `tuning.html.built` debug artifact are gitignored.
