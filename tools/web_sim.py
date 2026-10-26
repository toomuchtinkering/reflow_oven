#!/usr/bin/env python3
"""Local preview/simulator for the reflow-oven tuning web UI.

Serves the real src/web/ SPA and mocks the device HTTP API (/telemetry, /profile,
/stop, /suggest-gains) from a simple first-order-plus-dead-time thermal model, so
the browser UI can be exercised on a computer before flashing hardware. It does
NOT touch firmware; it only stands in for the device's HTTP contract.

Drive it from the terminal (type a letter + Enter):
  s   start an open-loop step test (starts from ambient)
  r   start a normal reflow run (verification overlay)
  f   trip a thermocouple fault on the active run (auto-clears to idle)
  c   confirm pending suggested gains (stands in for the touchscreen)
  q   quit  (Ctrl-C also works)

Then open http://localhost:8000/ in a browser. Zero dependencies (stdlib only).
It listens on 127.0.0.1 only; pass --bind 0.0.0.0 to open it from another
device, which in proxy mode also exposes the oven's API through this computer.

Time runs accelerated (--speed, default 4x) so a step test reaches the ceiling
in ~1 min and a reflow completes in ~2 min of wall-clock; elapsed_ms and the
chart stay simulated-second-accurate.

PROXY MODE (--device HOST):
  Instead of the thermal model, forward the small API calls (/telemetry,
  /profile, /stop, /suggest-gains) to a REAL reflow oven, while still serving
  the SPA from this computer. This sidesteps the device's inability to serve
  its own ~11 KB web page (it only ever sees the small requests that work),
  so you can watch live telemetry and tune PID gains against real hardware.
  Start runs from the oven's TOUCHSCREEN; observe + suggest gains in the browser.
      python tools/web_sim.py --device 192.168.1.42
"""
import argparse, json, random, sys, threading, time
import urllib.error, urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

WEB_DIR = Path(__file__).resolve().parent.parent / "src" / "web"
API_VERSION = 3
AMBIENT_C = 25.0

# Default reflow profile — mirrors the include/config.h *_DEFAULT values.
PROFILE = dict(first_heatup=1.2, soak_temp=150, soak_time=90,
               second_heatup=1.0, reflow_temp=220, reflow_time=45, cool_rate=1.0)


def _ramp(frm, to, rate):
    """1 s samples ramping frm→to at `rate` °C/s (excludes the start point)."""
    n = max(1, int(round(abs(to - frm) / rate)))
    return [frm + (to - frm) * (i + 1) / n for i in range(n)]


def gen_profile_curve():
    """Piecewise HEAT→SOAK→HEAT→REFLOW→COOL setpoint curve at 1 s spacing.
    Returns (step_ms, temp_c[list], phase_end_ms[6])."""
    p = PROFILE
    seg1 = _ramp(AMBIENT_C, p['soak_temp'], p['first_heatup'])
    soak = [float(p['soak_temp'])] * p['soak_time']
    seg2 = _ramp(p['soak_temp'], p['reflow_temp'], p['second_heatup'])
    refl = [float(p['reflow_temp'])] * p['reflow_time']
    cool = _ramp(p['reflow_temp'], AMBIENT_C, p['cool_rate'])
    temps = seg1 + soak + seg2 + refl + cool
    # Cumulative phase-end marks (ms): [NONE, HEAT1, SOAK, HEAT2, REFLOW, COOL].
    e1 = len(seg1); e2 = e1 + len(soak); e3 = e2 + len(seg2)
    e4 = e3 + len(refl); e5 = e4 + len(cool)
    phase_end_ms = [0, e1 * 1000, e2 * 1000, e3 * 1000, e4 * 1000, e5 * 1000]
    return 1000, [round(x, 1) for x in temps], phase_end_ms


_STEP_MS, _CURVE, _PHASE_END = gen_profile_curve()


class Sim:
    """Thread-safe simulated oven. tick(dt_sim_s) advances the model; the HTTP
    handlers read telemetry()/profile under the same lock."""

    # First-order-plus-dead-time model params (chosen for a clean, quick demo).
    L = 15.0          # step-test dead time (s)
    TAU_STEP = 200.0  # step-test time constant (s) → max slope ~1 °C/s
    K = 4.3           # step-test process gain (°C per % duty, steady state)
    TAU_COOL = 90.0   # idle/fault passive cool-down (s)
    NOISE = 0.3       # published temperature noise (°C, 1σ)
    # Reflow tracking: achieved temp follows the planned setpoint with a lag set
    # by the ACTIVE Kp (TRACK_BASE/Kp, clamped). Higher Kp → tighter tracking and
    # less cool-down overshoot, so confirming better gains visibly improves the
    # verification overlay. A simplified visualisation of the confirmed gains
    # being applied, not a literal plant; Ki/Kd are carried for display.
    TRACK_BASE = 48.0
    TRACK_TAU_MIN, TRACK_TAU_MAX = 5.0, 90.0
    DEFAULT_GAINS = dict(kp=0.5, ki=0.0, kd=0.0,  # under-tuned (laggy) until you confirm a set
                         k_hold=0.19, k_rate=71.0)

    def __init__(self, duty, ceiling, max_ms):
        self.lock = threading.Lock()
        self.cfg_duty, self.cfg_ceiling, self.cfg_max_ms = duty, float(ceiling), max_ms
        self.mode = 'idle'
        self.temp = AMBIENT_C
        self.setpoint = 0.0
        self.duty = 0
        self.elapsed_ms = 0
        self.fault = 0
        self.run_id = 0
        self.step = dict(active=False, duty=0, ceiling_c=0.0, max_ms=0, end_reason='none')
        self.pending = None
        self.gains = dict(self.DEFAULT_GAINS)   # active gains (updated by confirm())
        self._run_gains = dict(self.gains)      # snapshot for the running reflow
        self._sim_clock = 0.0
        self._fault_clear_at = None

    # ---- simulation step ----
    def tick(self, dt):
        with self.lock:
            self._sim_clock += dt
            if self.mode == 'step_test':
                self.elapsed_ms += int(dt * 1000)
                if self.elapsed_ms / 1000.0 > self.L:            # past the dead time
                    t_eq = AMBIENT_C + self.K * self.step['duty']
                    self.temp += (t_eq - self.temp) * (dt / self.TAU_STEP)
                self.duty = self.step['duty']
                if self.temp >= self.step['ceiling_c']:
                    self._end_step('ceiling')
                elif self.elapsed_ms >= self.step['max_ms']:
                    self._end_step('max_duration')
            elif self.mode == 'running':
                self.elapsed_ms += int(dt * 1000)
                idx = self.elapsed_ms // 1000
                if idx >= len(_CURVE):
                    self.mode, self.setpoint, self.duty = 'idle', 0.0, 0
                else:
                    self.setpoint = _CURVE[idx]
                    kp = max(0.2, self._run_gains['kp'])
                    tau = min(self.TRACK_TAU_MAX, max(self.TRACK_TAU_MIN, self.TRACK_BASE / kp))
                    self.temp += (self.setpoint - self.temp) * (dt / tau)
                    self.duty = max(0, min(100, int((self.setpoint - self.temp) * 5)))
            elif self.mode == 'fault':
                self.duty = 0
                self.temp += (AMBIENT_C - self.temp) * (dt / self.TAU_COOL)
                if self._fault_clear_at is not None and self._sim_clock >= self._fault_clear_at:
                    self.mode, self.fault, self._fault_clear_at = 'idle', 0, None
            else:  # idle
                self.duty, self.setpoint = 0, 0.0
                self.temp += (AMBIENT_C - self.temp) * (dt / self.TAU_COOL)

    def _end_step(self, reason):
        self.step['active'] = False
        self.step['end_reason'] = reason
        self.mode, self.duty = 'idle', 0

    # ---- commands (return (ok, message)) ----
    def start_step(self):
        with self.lock:
            if self.mode != 'idle':
                return False, f'busy ({self.mode}) — stop or wait first'
            self.run_id += 1
            self.elapsed_ms, self.fault, self.temp = 0, 0, AMBIENT_C
            self.step = dict(active=True, duty=self.cfg_duty, ceiling_c=self.cfg_ceiling,
                             max_ms=self.cfg_max_ms, end_reason='none')
            self.mode = 'step_test'
            return True, (f'step test #{self.run_id}: duty {self.cfg_duty}%, '
                          f'ceiling {self.cfg_ceiling:.0f}°C')

    def start_reflow(self):
        with self.lock:
            if self.mode != 'idle':
                return False, f'busy ({self.mode}) — stop or wait first'
            self.run_id += 1
            self.elapsed_ms, self.fault, self.temp = 0, 0, AMBIENT_C
            self.step = dict(active=False, duty=0, ceiling_c=0.0, max_ms=0, end_reason='none')
            self._run_gains = dict(self.gains)   # the next run uses the latest confirmed gains
            self.mode = 'running'
            g = self._run_gains
            return True, (f'reflow run #{self.run_id} using Kp {g["kp"]:.2f} Ki {g["ki"]:.2f} '
                          f'Kd {g["kd"]:.2f} — watch planned-vs-achieved')

    def trip_fault(self):
        with self.lock:
            if self.mode not in ('step_test', 'running'):
                return False, 'no active run to fault'
            if self.mode == 'step_test':
                self.step['active'], self.step['end_reason'] = False, 'fault'
            self.fault, self.mode, self.duty = 1, 'fault', 0   # 1 = FAULT_TC_OPEN
            self._fault_clear_at = self._sim_clock + 5.0
            return True, 'TC fault tripped (auto-clears to idle in ~5 s sim time)'

    def stop(self):
        with self.lock:
            if self.mode == 'step_test':
                self.step['active'], self.step['end_reason'] = False, 'manual_stop'
            self.mode, self.duty = 'idle', 0

    def suggest(self, kp, ki, kd, k_hold, k_rate):
        with self.lock:
            self.pending = dict(kp=kp, ki=ki, kd=kd, k_hold=k_hold, k_rate=k_rate)

    def confirm(self):
        with self.lock:
            had, self.pending = self.pending, None
            if had:
                self.gains = dict(had)   # applied to the NEXT reflow run
            return had

    def telemetry(self):
        with self.lock:
            t = self.temp + random.gauss(0, self.NOISE)
            return dict(
                api=API_VERSION, mode=self.mode, temp_c=round(t, 2),
                setpoint_c=round(self.setpoint, 2), duty=self.duty,
                elapsed_ms=self.elapsed_ms, fault=self.fault, run_id=self.run_id,
                steptest=dict(active=self.step['active'], duty=self.step['duty'],
                              ceiling_c=self.step['ceiling_c'], max_ms=self.step['max_ms'],
                              end_reason=self.step['end_reason']),
                # v3 drying fields. The sim has no drying mode to exercise —
                # a dry is started from the touchscreen only, so there is no
                # endpoint to drive one — but claiming api=3 while omitting the
                # v3 shape would make the sim lie about its own version.
                dry_active=False, dry_stage='none', dry_target_c=0.0,
                dry_hold_ms=0, dry_hold_elapsed_ms=0, dry_end='none',
                pending_pid=self.pending)


SIM = None           # simulated oven; None in proxy mode
PROXY_TARGET = None  # device host (e.g. "192.168.1.42"); None in sim mode
PROXY_TIMEOUT = 5.0  # per device request (s); > the device's quick small-response time


def proxy_request(method, path, headers=None, body=None):
    """Forward one request to the real device. Returns (status, content_type, body_bytes).
    Device 4xx/5xx (e.g. 401 bad token, 409 not idle) are relayed verbatim; an
    unreachable device / timeout / reset becomes a 502 so the browser shows
    'connection lost' and keeps polling (auto-recovers when the device returns)."""
    url = f"http://{PROXY_TARGET}{path}"
    req = urllib.request.Request(url, data=body, method=method)
    for k, v in (headers or {}).items():
        req.add_header(k, v)
    try:
        with urllib.request.urlopen(req, timeout=PROXY_TIMEOUT) as r:
            return r.status, r.headers.get('Content-Type', 'application/json'), r.read()
    except urllib.error.HTTPError as e:
        return e.code, e.headers.get('Content-Type', 'application/json'), e.read()
    except Exception as e:
        return 502, 'application/json', json.dumps(dict(error=f'device unreachable: {e}')).encode()


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):   # keep the terminal clean for the REPL
        pass

    def _send(self, code, ctype, body):
        self.send_response(code)
        self.send_header('Content-Type', ctype)
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _json(self, obj, code=200):
        self._send(code, 'application/json', json.dumps(obj).encode())

    def _static(self, name, ctype):
        f = WEB_DIR / name
        if not f.is_file():
            return self._send(404, 'text/plain', b'not found')
        self._send(200, ctype, f.read_bytes())

    def do_GET(self):
        path = self.path.split('?', 1)[0]
        if path == '/':
            self._static('tuning.html', 'text/html; charset=utf-8')
        elif path == '/app.js':
            self._static('app.js', 'text/javascript')
        elif path == '/fit.js':
            self._static('fit.js', 'text/javascript')
        elif path == '/align.js':
            self._static('align.js', 'text/javascript')
        # uPlot, vendored into src/web and inlined into the device page by
        # build_web_asset.py. Served as separate files here because the dev page
        # references them as a <link> and a <script src>, the same way it does
        # for fit.js and app.js.
        elif path == '/uplot.iife.min.js':
            self._static('uplot.iife.min.js', 'text/javascript')
        elif path == '/uplot.min.css':
            self._static('uplot.min.css', 'text/css')
        elif path in ('/telemetry', '/profile', '/version'):
            if PROXY_TARGET:
                self._send(*proxy_request('GET', path))
            elif path == '/version':
                # Stub so the SPA's fw-version archive field works off-hardware.
                self._json(dict(version='sim', slot='sim', status='ok'))
            elif path == '/telemetry':
                self._json(SIM.telemetry())
            else:  # /profile
                with SIM.lock:
                    g = dict(SIM.gains)
                self._json(dict(api=API_VERSION, gains=g, ambient_ref_c=AMBIENT_C,
                                step_ms=_STEP_MS, len=len(_CURVE),
                                phase_end_ms=_PHASE_END, temp_c=_CURVE))
        else:
            self._send(404, 'text/plain', b'not found')

    def do_POST(self):
        path = self.path.split('?', 1)[0]
        n = int(self.headers.get('Content-Length', 0) or 0)
        body = self.rfile.read(n) if n else b''
        if PROXY_TARGET and path in ('/stop', '/suggest-gains'):
            # Pass through the token + content-type the browser supplied; the
            # device authenticates these writes with X-OTA-Token.
            hdrs = {h: self.headers[h] for h in ('X-OTA-Token', 'Content-Type')
                    if self.headers.get(h)}
            self._send(*proxy_request('POST', path, hdrs, body))
        elif path == '/stop':
            SIM.stop()
            self._json(dict(status='stopping'))
        elif path == '/suggest-gains':
            try:
                g = json.loads(body.decode() or '{}')
                SIM.suggest(float(g['kp']), float(g['ki']), float(g['kd']),
                            float(g['k_hold']), float(g['k_rate']))
                self._json(dict(status='pending_confirm'))
            except Exception as e:
                self._json(dict(error=str(e)), 400)
        else:
            self._send(404, 'text/plain', b'not found')


def _ticker(speed, stop_evt):
    last = time.monotonic()
    while not stop_evt.is_set():
        time.sleep(0.05)
        now = time.monotonic()
        dt = now - last
        last = now
        SIM.tick(dt * speed)


BANNER = """\
reflow-oven web UI simulator  (real src/web/ SPA + mock device API)
  time scale {speed}x   step duty {duty}%   ceiling {ceiling:.0f}°C

  ▶  open in your browser:   http://localhost:{port}/

commands (letter + Enter):
  s = step test    r = reflow run    f = trip fault
  c = confirm suggested gains (touchscreen stand-in)    q = quit
"""

PROXY_BANNER = """\
reflow-oven web UI  ->  PROXYING TO REAL DEVICE  {device}
  {devinfo}

  >  open in your browser:   http://localhost:{port}/

  The SPA is served from this computer; only the small API calls
  (/telemetry, /profile, /stop, /suggest-gains) reach the oven — so the
  device never serves its own page and can't hit the heap crash.

  Start runs from the oven's TOUCHSCREEN (step test / reflow); watch the
  live curve + fit here, push gains with "suggest" (enter the OTA token
  when prompted), then CONFIRM the gains on the touchscreen.

  q = quit  (Ctrl-C also works)
"""


def main():
    ap = argparse.ArgumentParser(description="Reflow-oven tuning web UI simulator / real-device proxy")
    ap.add_argument('--port', type=int, default=8000)
    ap.add_argument('--bind', default='127.0.0.1', metavar='ADDR',
                    help='listen address (default 127.0.0.1: this computer only). 0.0.0.0 makes the '
                         'simulator reachable from the network and, with --device, relays the oven API '
                         'to anyone who can reach this computer.')
    ap.add_argument('--device', metavar='HOST',
                    help='PROXY mode: forward API calls to a real oven at this host/IP '
                         '(e.g. 192.168.1.42) instead of simulating. The SPA is still served '
                         'locally, so the device only handles the small requests it can.')
    ap.add_argument('--speed', type=float, default=4.0, help='simulated-time scale (default 4x; sim mode only)')
    ap.add_argument('--duty', type=int, default=50, help='step-test duty %% (default 50; sim mode only)')
    ap.add_argument('--ceiling', type=float, default=180.0, help='step-test ceiling °C (default 180; sim mode only)')
    ap.add_argument('--max-min', type=float, default=10.0, help='step-test max duration, minutes (sim mode only)')
    args = ap.parse_args()

    global SIM, PROXY_TARGET
    PROXY_TARGET = args.device

    stop_evt = threading.Event()
    httpd = ThreadingHTTPServer((args.bind, args.port), Handler)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()

    if PROXY_TARGET:
        # Confirm the oven is reachable with a small, safe request before the
        # browser connects; surface its version + heap so you know the state.
        code, _, body = proxy_request('GET', '/version')
        if code == 200:
            try:
                v = json.loads(body)
                fh = v.get('free_heap')
                heap = f"{fh} B free heap" if fh is not None else "(old fw — no free_heap field)"
                devinfo = f"device OK: version {v.get('version','?')}, slot {v.get('slot','?')}, {heap}"
            except Exception:
                devinfo = "device reachable (couldn't parse /version)"
        else:
            devinfo = f"WARNING: device not reachable yet (/version -> {code}); will retry on each poll"
        print(PROXY_BANNER.format(device=PROXY_TARGET, devinfo=devinfo, port=args.port))
    else:
        SIM = Sim(args.duty, args.ceiling, int(args.max_min * 60000))
        threading.Thread(target=_ticker, args=(args.speed, stop_evt), daemon=True).start()
        print(BANNER.format(port=args.port, speed=args.speed, duty=args.duty, ceiling=args.ceiling))

    try:
        for line in sys.stdin:
            cmd = line.strip().lower()
            if cmd in ('q', 'quit', 'exit'):
                break
            elif cmd == '':
                continue
            elif PROXY_TARGET:
                print('  proxy mode: start/stop runs and confirm gains on the oven TOUCHSCREEN. '
                      '(q to quit)')
            elif cmd == 's':
                print('  ' + SIM.start_step()[1])
            elif cmd == 'r':
                print('  ' + SIM.start_reflow()[1])
            elif cmd == 'f':
                print('  ' + SIM.trip_fault()[1])
            elif cmd == 'c':
                g = SIM.confirm()
                print('  applied Kp %.2f Ki %.2f Kd %.2f' % (g['kp'], g['ki'], g['kd'])
                      if g else '  no pending suggestion')
            else:
                print('  ? commands: s r f c q')
    except KeyboardInterrupt:
        pass
    finally:
        stop_evt.set()
        httpd.shutdown()
        print('\nbye')


if __name__ == '__main__':
    main()
