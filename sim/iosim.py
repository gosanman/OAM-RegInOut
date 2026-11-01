"""RegInOut-Simulator - Python-App mit grafischer Oberflaeche im Browser.

Startet den Simulatorkern (build/iosim.exe, der UNVERAENDERTE Firmware-Code
der Module OFM-BinaryInput und OFM-SwitchActuator), treibt die
Simulationszeit und stellt die Oberflaeche unter http://127.0.0.1:8767 bereit.
Nur Python-Standardbibliothek.

    python sim/iosim.py [--port 8767] [--no-browser] [--project "Grundaufbau"]

Was die App tut:
  - Parameter wie in der ETS setzen und "programmieren" (ETS-Download mit
    Neustart, genau wie am Geraet),
  - Eingangsobjekte senden, auf Wunsch zyklisch,
  - Kontakte an den Binaereingaengen schalten,
  - KNX-Bus und Relaisversorgung schalten, Busdip, Funktionstaste,
  - Relais, Eingaenge und Pins anzeigen,
  - Telegramme, Konsole und Invariantenwaechter mitschreiben.
"""

import argparse
import json
import os
import re
import threading
import time
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, unquote, urlparse

from iocore import BUILD, HERE, Core, ParamImage, load_meta

WEB = os.path.join(HERE, 'web')
PROJECTS = os.path.join(HERE, 'projects')
TICK_S = 0.1          # Takt der Echtzeit
MAX_EVENTS = 6000
MAX_RELAY = 200000


def safe_name(name):
    return re.sub(r'[^\w äöüÄÖÜß.+()-]', '_', name).strip() or 'Projekt'


class Simulator:
    def __init__(self, project):
        self.meta = load_meta()
        self.meta_json = json.dumps(self.meta, ensure_ascii=False).encode('utf-8')
        self.lock = threading.RLock()
        self.speed = 1.0
        self.events = []        # (seq, ev)
        self.seq = 0
        self.relay = []         # [t, k1, k2, ...]
        self.kos = {}           # nr -> {val, raw, t, dir, n, dpt}
        self.kos_version = 0
        self.cyclic = {}        # nr -> {dpt, value, period, next}
        self.state = None
        self.hello = {}
        self.pending = ParamImage(self.meta)
        self.active_values = {}
        self.hw = self.default_hw()
        self.project = ''
        self.core = None
        self._start_core()
        self.load_project(project)

    @staticmethod
    def default_hw():
        return {'supply': True, 'contacts': [False, False]}

    # ------------------------------------------------------------ Kern
    def _on_event(self, ev):
        kind = ev.get('ev')
        if kind == 'relay':
            self.relay.append([ev['t']] + ev['on'])
            if len(self.relay) > MAX_RELAY:
                del self.relay[: MAX_RELAY // 4]
            return
        if kind == 'state':
            self.state = ev
            return
        if kind == 'hello':
            self.hello = ev
        if kind in ('ko', 'rx'):
            k = self.kos.setdefault(ev['nr'], {'n': 0})
            k.update(val=ev['val'], raw=ev.get('raw', ''), t=ev['t'],
                     dir='tx' if kind == 'ko' else 'rx', dpt=ev['dpt'])
            k['n'] += 1
            self.kos_version += 1
        self.seq += 1
        self.events.append((self.seq, ev))
        if len(self.events) > MAX_EVENTS:
            del self.events[: MAX_EVENTS // 4]

    def _start_core(self):
        if self.core:
            try:
                self.core.close()
            except Exception:
                pass
        self.relay.clear()
        self.core = Core(on_event=self._on_event)
        self.kos.clear()
        self.kos_version += 1
        self.cyclic.clear()

    def cmd(self, line):
        with self.lock:
            return self.core.cmd(line)

    def run(self, ms):
        """Zeit fortschreiben; zyklische Telegramme zur rechten Zeit einstreuen."""
        with self.lock:
            now = self.state['t'] if self.state else 0
            end = now + int(ms)
            while now < end:
                due = [c for c in self.cyclic.values() if c['next'] <= now]
                for c in due:
                    self.core.cmd(f"ko {c['nr']} {c['dpt']} {c['value']}")
                    c['next'] = now + c['period']
                nxt = min([c['next'] for c in self.cyclic.values()] + [end])
                step = max(1, min(nxt, end) - now)
                self.core.cmd(f'run {step}')
                now += step
            self.core.cmd('state')

    def tick_loop(self):
        while True:
            t0 = time.time()
            if self.speed > 0:
                try:
                    self.run(TICK_S * 1000 * self.speed)
                except Exception as e:  # Kern abgestuerzt: melden, neu starten
                    self._on_event({'ev': 'error', 't': 0, 'text': f'Kern: {e} - Neustart des Simulators'})
                    with self.lock:
                        self._start_core()
                        self.load_project(self.project)
            time.sleep(max(0.0, TICK_S - (time.time() - t0)))

    # ------------------------------------------------------------ Hardware
    def apply_hw(self):
        self.core.cmd(f"supply {1 if self.hw['supply'] else 0}")
        for i, closed in enumerate(self.hw['contacts'], 1):
            self.core.cmd(f'bi {i} {1 if closed else 0}')

    # ------------------------------------------------------------ Projekte
    def project_path(self, name):
        return os.path.join(PROJECTS, safe_name(name) + '.json')

    def list_projects(self):
        if not os.path.isdir(PROJECTS):
            return []
        return sorted(f[:-5] for f in os.listdir(PROJECTS) if f.endswith('.json'))

    def load_project(self, name):
        """Projekt laden: Parameter setzen und das Geraet frisch starten -
        als waere es mit dieser Parametrierung programmiert und eingeschaltet."""
        data = {'values': {}}
        if name and os.path.exists(self.project_path(name)):
            with open(self.project_path(name), encoding='utf-8') as f:
                data = json.load(f)
        with self.lock:
            self._start_core()
            self.project = name or ''
            self.hw = self.default_hw()
            self.hw.update(data.get('hw') or {})
            self.pending.load_values(data.get('values', {}))
            self.active_values = dict(self.pending.values)
            self.apply_hw()
            self.core.cmd('load ' + self.pending.hex())
            self.core.cmd('bus 1')
            self.core.cmd('run 500')
            self.core.cmd('state')

    def save_project(self, name):
        os.makedirs(PROJECTS, exist_ok=True)
        with open(self.project_path(name), 'w', encoding='utf-8') as f:
            json.dump({'name': name, 'values': self.pending.values, 'hw': self.hw},
                      f, ensure_ascii=False, indent=1)
        self.project = name

    # ------------------------------------------------------------ Aktionen
    def action(self, a):
        op = a.get('action')
        if op == 'speed':
            self.speed = float(a['value'])
        elif op == 'step':
            self.run(int(a['ms']))
        elif op == 'power':
            if a['which'] == 'bus':
                self.cmd(f"bus {1 if a['on'] else 0}")
            else:
                self.hw['supply'] = bool(a['on'])
                self.cmd(f"supply {1 if a['on'] else 0}")
            self.cmd('state')
        elif op == 'busdip':
            self.cmd(f"busdip {int(a.get('ms', 200))}")
            self.cmd('state')
        elif op == 'contact':
            no = int(a['no'])
            self.hw['contacts'][no - 1] = bool(a['closed'])
            self.cmd(f"bi {no} {1 if a['closed'] else 0}")
            self.cmd('state')
        elif op == 'ko':
            nr, dpt, value = int(a['nr']), a['dpt'], str(a['value']).strip()
            self.cmd(f'ko {nr} {dpt} {value}')
            period = float(a.get('cyclic') or 0)
            with self.lock:
                if period > 0:
                    now = self.state['t'] if self.state else 0
                    self.cyclic[nr] = dict(nr=nr, dpt=dpt, value=value, period=int(period * 1000),
                                           next=now + int(period * 1000))
                else:
                    self.cyclic.pop(nr, None)
            self.cmd('state')
        elif op == 'cyclic_off':
            with self.lock:
                self.cyclic.pop(int(a['nr']), None)
        elif op == 'btn':
            self.cmd(f"btn {a['kind']}")
            self.cmd('state')
        elif op == 'console':
            self.cmd(('diag ' if a.get('diag') else 'cmd ') + a['text'])
            self.cmd('state')
        elif op == 'param':
            with self.lock:
                self.pending.set(a['name'], a['value'])
        elif op == 'params_revert':
            with self.lock:
                self.pending.load_values(self.active_values)
        elif op == 'params_defaults':
            with self.lock:
                self.pending.reset()
        elif op == 'program':
            with self.lock:
                self.core.cmd('pimg ' + self.pending.hex())
                self.core.cmd(f"ets {int(a.get('ms', 3000))}")
                self.active_values = dict(self.pending.values)
                self.core.cmd('state')
        elif op == 'restart':
            self.cmd('restart')
            self.cmd('state')
        elif op == 'unload':
            self.cmd('unload')
            self.cmd('state')
        elif op == 'project_load':
            self.load_project(a['name'])
        elif op == 'project_save':
            self.save_project(a['name'])
        elif op == 'reset':
            self.load_project(self.project)
        elif op == 'clear':
            with self.lock:
                self.events.clear()
        elif op == 'fp_raw':
            # ETS-Skript (Schaltflaeche in der ETS-Ansicht): Die simulierten
            # Module haben keine Function Properties - die Schaltflaeche meldet das.
            return {'ok': True, 'handled': False, 'res': [],
                    'text': f"Function Property {a.get('obj')}/{a.get('prop')}: "
                            f"die simulierten Module (Binäreingang, Schaltaktor) haben keine"}
        else:
            raise ValueError(f'unbekannte Aktion {op}')
        return {'ok': True}

    def poll(self, since, relay_since, kos_version):
        with self.lock:
            ev = [e for s, e in self.events if s > since] if since >= 0 else [e for _, e in self.events[-500:]]
            ev = ev[-1500:]
            relay_since = max(0, min(relay_since, len(self.relay)))
            out = dict(seq=self.seq, events=ev, state=self.state, speed=self.speed, hello=self.hello,
                       relay=self.relay[relay_since:], relayIdx=len(self.relay), hw=self.hw,
                       project=self.project, projects=self.list_projects(),
                       pending=self.pending.values, active=self.active_values,
                       dirty=self.pending.values != self.active_values,
                       cyclic=[dict(nr=c['nr'], dpt=c['dpt'], value=c['value'],
                                    period=c['period'] / 1000) for c in self.cyclic.values()],
                       kosVersion=self.kos_version)
            if kos_version != self.kos_version:
                out['kos'] = self.kos
            return out


SIM = None


class Server(ThreadingHTTPServer):
    # Unter Windows erlaubt SO_REUSEADDR zwei Server am selben Port - der
    # zweite Start soll aber mit "Port belegt" scheitern, nicht still mitlaufen.
    allow_reuse_address = False
    daemon_threads = True


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def _send(self, code, body, ctype='application/json; charset=utf-8'):
        if isinstance(body, (dict, list)):
            body = json.dumps(body, ensure_ascii=False).encode('utf-8')
        elif isinstance(body, str):
            body = body.encode('utf-8')
        self.send_response(code)
        self.send_header('Content-Type', ctype)
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Cache-Control', 'no-store')
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        url = urlparse(self.path)
        if url.path == '/api/meta':
            return self._send(200, SIM.meta_json)
        if url.path == '/api/poll':
            q = parse_qs(url.query)
            return self._send(200, SIM.poll(int(q.get('since', ['-1'])[0]),
                                            int(q.get('relay', ['0'])[0]),
                                            int(q.get('kv', ['-1'])[0])))
        name = 'index.html' if url.path in ('/', '') else url.path.lstrip('/')
        base = WEB
        if name.startswith('baggage/'):       # Bilder, Icons, Hilfetexte aus der knxprod
            base = os.path.join(BUILD, 'baggage')
            name = name[len('baggage/'):]
        path = os.path.normpath(os.path.join(base, unquote(name)))
        if not path.startswith(base) or not os.path.isfile(path):
            return self._send(404, {'error': 'nicht gefunden'})
        ctype = {'.html': 'text/html; charset=utf-8', '.js': 'text/javascript; charset=utf-8',
                 '.css': 'text/css; charset=utf-8', '.svg': 'image/svg+xml', '.png': 'image/png',
                 '.txt': 'text/plain; charset=utf-8'}.get(
            os.path.splitext(path)[1], 'application/octet-stream')
        with open(path, 'rb') as f:
            self._send(200, f.read(), ctype)

    def do_POST(self):
        if urlparse(self.path).path != '/api/action':
            return self._send(404, {'error': 'nicht gefunden'})
        try:
            data = json.loads(self.rfile.read(int(self.headers.get('Content-Length', 0))) or b'{}')
            self._send(200, SIM.action(data))
        except Exception as e:
            self._send(400, {'ok': False, 'error': str(e)})


def main():
    global SIM
    ap = argparse.ArgumentParser(description='RegInOut-Simulator (Binäreingänge und Schaltaktor)')
    ap.add_argument('--port', type=int, default=8767)
    ap.add_argument('--no-browser', action='store_true')
    ap.add_argument('--project', default='Grundaufbau')
    args = ap.parse_args()

    if not os.path.exists(os.environ.get('IOSIM_EXE') or os.path.join(BUILD, 'iosim.exe')):
        raise SystemExit('build/iosim.exe fehlt - zuerst: pwsh -File sim/build.ps1')
    try:
        server = Server(('127.0.0.1', args.port), Handler)
    except OSError:
        raise SystemExit(f'Port {args.port} ist belegt - läuft der Simulator schon? '
                         f'Sonst mit --port einen anderen wählen.')
    SIM = Simulator(args.project)
    threading.Thread(target=SIM.tick_loop, daemon=True).start()
    url = f'http://127.0.0.1:{args.port}/'
    print(f'RegInOut-Simulator läuft: {url}   (Beenden mit Strg+C)')
    if not args.no_browser:
        webbrowser.open(url)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        SIM.core.close()


if __name__ == '__main__':
    main()
