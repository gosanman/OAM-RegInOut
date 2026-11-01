"""Python-Anbindung an den Simulatorkern (build/iosim.exe).

Zwei Bausteine, beide ohne Fremdpakete:

  ParamImage  der ETS-Parameterspeicher: Werte per Parametername setzen, das
              Abbild wird genau so kodiert wie von der ETS (Big Endian, Bitlage
              ab MSB). Grundlage sind die Vorgabewerte aus meta.json.
  Core        startet iosim.exe, schickt Befehle, sammelt die Ereignisse.

Beispiel (ohne Oberflaeche, etwa fuer einen Regressionstest):

    meta = load_meta()
    img = ParamImage(meta)
    img.set('SWA_Ch1Active', 1)
    core = Core()
    core.cmd('load ' + img.hex())
    core.cmd('bus 1')
    core.cmd('ko 110 1.1 1')      # Relais 1: Schalten Ein
    core.cmd('run 100')
    print(core.state()['sim']['relays'])
"""

import json
import os
import struct
import subprocess
import threading

HERE = os.path.dirname(os.path.abspath(__file__))
BUILD = os.path.join(HERE, 'build')


def load_meta(path=None):
    with open(path or os.path.join(BUILD, 'meta.json'), encoding='utf-8') as f:
        return json.load(f)


class ParamImage:
    def __init__(self, meta):
        self.meta = meta
        self.params = meta['params']
        self.image = bytearray.fromhex(meta['defaults'])
        self.values = {}   # nur geaenderte Werte: Name -> Wert

    def default(self, name):
        return self.params[name]['d']

    def get(self, name):
        return self.values.get(name, self.params[name]['d'])

    def set(self, name, value, clamp=True):
        """clamp=False laesst Werte ausserhalb des ETS-Bereichs durch - fuer Tests,
        die ein beschaedigtes Parameterabbild nachstellen."""
        p = self.params[name]
        if p['k'] == 'text':
            value = str(value)
        elif p['k'] == 'color':
            value = ('#' + str(value).lstrip('#')).upper()
        elif p['k'] == 'float':
            value = float(value)
        else:
            value = int(float(value))
            if clamp and 'min' in p and p['k'] in ('uint', 'int'):
                value = max(int(p['min']), min(int(p['max']), value))
        if value == (p['d'].upper() if p['k'] == 'color' else p['d']):
            self.values.pop(name, None)
        else:
            self.values[name] = value
        self._encode(p, value)

    def reset(self):
        self.image = bytearray.fromhex(self.meta['defaults'])
        self.values = {}

    def load_values(self, values):
        self.reset()
        for name, value in values.items():
            if name in self.params:
                self.set(name, value)

    def hex(self):
        return self.image.hex()

    def _encode(self, p, value):
        if p['o'] is None:
            return          # reiner ETS-Wert (Kanalname, Bild): nicht im Geraet
        o, b, s = p['o'], p['b'], p['s']
        if p['k'] == 'text':
            raw = value.encode('latin-1', 'replace')[: s // 8]
            self.image[o: o + s // 8] = raw.ljust(s // 8, b'\0')
            return
        if p['k'] == 'color':
            self.image[o: o + 3] = int(value.lstrip('#') or '0', 16).to_bytes(3, 'big')
            return
        if p['k'] == 'float':
            self.image[o: o + 4] = struct.pack('>f', value)
            return
        v = value & ((1 << s) - 1)
        for i in range(s):
            pos = o * 8 + b + i
            mask = 0x80 >> (pos % 8)
            if (v >> (s - 1 - i)) & 1:
                self.image[pos // 8] |= mask
            else:
                self.image[pos // 8] &= ~mask & 0xFF


class Core:
    """Ein laufender Simulatorkern. Thread-sicher fuer einen Aufrufer je Befehl."""

    def __init__(self, exe=None, on_event=None):
        exe = exe or os.environ.get('IOSIM_EXE') or os.path.join(BUILD, 'iosim.exe')
        if not os.path.exists(exe):
            raise FileNotFoundError(f'{exe} fehlt - zuerst sim/build.ps1 ausfuehren')
        self.proc = subprocess.Popen([exe], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT, text=True, encoding='utf-8',
                                     bufsize=1)
        self.lock = threading.Lock()
        self.on_event = on_event
        self.hello = self._read_until_ok(first=True)

    def _read_until_ok(self, first=False):
        events = []
        while True:
            line = self.proc.stdout.readline()
            if not line:
                raise RuntimeError('Simulatorkern beendet')
            line = line.strip()
            if not line:
                continue
            try:
                ev = json.loads(line)
            except json.JSONDecodeError:
                ev = {'ev': 'raw', 'text': line}
            if ev.get('ev') == 'ok':
                return events
            if self.on_event:
                self.on_event(ev)
            events.append(ev)
            if first and ev.get('ev') == 'hello':
                return events

    def cmd(self, line):
        """Einen Befehl schicken; liefert alle Ereignisse bis zum ok."""
        with self.lock:
            self.proc.stdin.write(line.replace('\n', ' ') + '\n')
            self.proc.stdin.flush()
            return self._read_until_ok()

    def state(self):
        for ev in self.cmd('state'):
            if ev.get('ev') == 'state':
                return ev
        return None

    def close(self):
        try:
            self.proc.stdin.write('quit\n')
            self.proc.stdin.flush()
        except OSError:
            pass
        self.proc.wait(timeout=5)


# ------------------------------------------------------------------ ETS-Sichtbarkeit
# Dieselbe Auswertung wie web/app.js (evalTest, condOk, effectiveValues,
# visibleKos), damit der Server ohne Browser weiss, welche Objekte die ETS
# einblendet - etwa fuer den Selbsttest.

def eval_test(v, test):
    import re
    test = str(test).strip()
    try:
        x = float(v)
    except (TypeError, ValueError):
        x = None
    m = re.match(r'^(<=|>=|!=|<|>|=)\s*(-?[\d.]+)$', test)
    if m:
        if x is None:
            return False
        y = float(m.group(2))
        op = m.group(1)
        return {'<': x < y, '>': x > y, '<=': x <= y, '>=': x >= y, '!=': x != y}.get(op, x == y)
    for t in test.split():
        try:
            if x is not None and float(t) == x:
                return True
        except ValueError:
            pass
        if t == str(v):
            return True
    return False


def cond_ok(conds, vals):
    for c in conds:
        v = vals.get(c[0])
        if c[1] is None:
            if any(eval_test(v, t) for t in (c[2] if len(c) > 2 else [])):
                return False
        elif not eval_test(v, c[1]):
            return False
    return True


def effective_values(meta, changed):
    vals = {n: p['d'] for n, p in meta['params'].items()}
    vals.update(changed)
    for _ in range(2):
        for pg in meta['pages']:
            if not cond_ok(pg['cond'], vals):
                continue
            for it in pg['items']:
                if it['type'] == 'a' and it.get('target') and cond_ok(it['cond'], vals):
                    vals[it['target']] = vals.get(it['source']) if it.get('source') else float(it['value'])
    return vals


def visible_kos(meta, vals):
    """Objektnummer -> DPT der sichtbaren Variante."""
    out = {}
    for pg in meta['pages']:
        for it in pg['items']:
            if it['type'] == 'k' and it['nr'] not in out and cond_ok(it['cond'], vals):
                out[it['nr']] = it.get('dpt', '')
    return out
