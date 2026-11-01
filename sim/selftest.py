"""Selbsttest des Simulators: Inbetriebnahme-Abläufe ohne Oberfläche.

Prüft, dass der Kern den echten Firmware-Code der Module OFM-BinaryInput und
OFM-SwitchActuator ausführt und Relais und Objekte so liefert, wie die
Parameter es verlangen. Läuft nach dem Bauen:  python sim/selftest.py
"""

import sys

from iocore import Core, ParamImage, load_meta

FAILS = []
WARNS = []          # alle Wächtermeldungen aller Fälle
EXPECTED = set()    # Wächterarten, die ein Fall bewusst auslöst

# Objektnummern (KO-Belegung im Kopf von src/RegInOut.xml)
BI_OUT = [50, 51]
SWA_CENTRAL = 100
SWA_BLOCK, SWA_SIZE = 110, 9
SWITCH, STATUS, STATUS_INV, LOCK, LOCK_STATUS, SCENE = range(6)


def ko(ch, idx):
    """Objekt idx von Relais ch (1..)."""
    return SWA_BLOCK + (ch - 1) * SWA_SIZE + idx


def check(label, cond, detail=''):
    print(('  ok    ' if cond else '  FEHLER ') + label + (f'  [{detail}]' if detail else ''))
    if not cond:
        FAILS.append(label)


class Device:
    """Ein frisch gestarteter Kern mit Parametern, Bus ein, 500 ms gelaufen."""

    def __init__(self, meta, values=None, configured=True, befund=None):
        self.befund = befund    # Wächtermeldungen gehören zu diesem Befund
        self.events = []
        self.core = Core(on_event=self._on_event)
        self.img = ParamImage(meta)
        base = {'BI_Channel1Active': 1, 'BI_Channel2Active': 1,
                'SWA_Ch1Active': 1, 'SWA_Ch1StatusSend': 1,
                'SWA_Ch2Active': 1, 'SWA_Ch2StatusSend': 1}
        for n, v in {**base, **(values or {})}.items():
            self.img.set(n, v)
        if configured:
            self.cmd('load ' + self.img.hex())
        self.cmd('bus 1')
        self.cmd('run 500')

    def _on_event(self, ev):
        self.events.append(ev)
        if ev.get('ev') == 'warn':
            WARNS.append(dict(ev, befund=getattr(self, 'befund', None)))

    def cmd(self, line):
        return self.core.cmd(line)

    def mark(self):
        return len(self.events)

    def sent(self, since, nr=None):
        return [e for e in self.events[since:] if e['ev'] == 'ko' and (nr is None or e['nr'] == nr)]

    def last(self, nr):
        s = self.sent(0, nr)
        return s[-1]['val'] if s else None

    def state(self):
        return self.core.state()

    def relay(self, ch):
        return self.state()['sim']['relays'][ch - 1]

    def relay_on(self, ch):
        return self.relay(ch)['on']

    def close(self):
        self.core.close()


def unconfigured(meta):
    print('1. Unparametriert')
    d = Device(meta, configured=False)
    st = d.state()
    check('Prozessor läuft, nicht parametriert', st['mcu'] and not st['configured'])
    check('beide Relais abgefallen', not d.relay_on(1) and not d.relay_on(2))
    m = d.mark()
    d.cmd(f'ko {ko(1, SWITCH)} 1.1 1')
    d.cmd('bi 1 1')
    d.cmd('run 1000')
    check('Schalttelegramm kommt nicht an, Relais bleibt aus', not d.relay_on(1))
    check('kein Telegramm gesendet', not d.sent(m), d.sent(m)[:2])
    d.close()


def relay_basics(meta):
    print('2. Relais: Schalten, Status, Öffner, Zentral')
    d = Device(meta, {'SWA_Ch1StatusInverted': 1, 'SWA_Ch1CentralFunction': 1})
    # Öffner per ETS-Download: so kommt jedes Gerät zu seinen Parametern, der
    # Flash ist danach gültig (ohne gültigen Flash siehe Befund B2)
    d.img.set('SWA_Ch2OperationMode', 1)
    d.cmd('pimg ' + d.img.hex())
    d.cmd('ets 3000')
    d.cmd('run 100')
    m = d.mark()
    d.cmd(f'ko {ko(1, SWITCH)} 1.1 1')
    d.cmd('run 50')
    check('K1 zieht an', d.relay_on(1))
    check('Status K1 = Ein gesendet', d.last(ko(1, STATUS)) is True)
    check('invertierter Status K1 = Aus gesendet', d.last(ko(1, STATUS_INV)) is False)
    d.cmd(f'ko {ko(1, SWITCH)} 1.1 0')
    d.cmd('run 50')
    check('K1 fällt ab', not d.relay_on(1))
    check('Status K1 = Aus gesendet', d.last(ko(1, STATUS)) is False)
    n = len(d.sent(m, ko(1, STATUS)))
    d.cmd(f'ko {ko(1, SWITCH)} 1.1 0')
    d.cmd('run 50')
    check('gleicher Wert erneut: Status wird wiederholt', len(d.sent(m, ko(1, STATUS))) == n + 1)

    # Oeffner: Spule invertiert zum Schaltbefehl
    d.cmd(f'ko {ko(2, SWITCH)} 1.1 1')
    d.cmd('run 50')
    r = d.relay(2)
    check('K2 als Öffner: Ein = Spule stromlos', not r['on'] and r['status'] is True, r)
    d.cmd(f'ko {ko(2, SWITCH)} 1.1 0')
    d.cmd('run 50')
    check('K2 als Öffner: Aus = Spule bestromt', d.relay_on(2))

    # Zentralfunktion nur fuer K1
    d.cmd(f'ko {SWA_CENTRAL} 1.1 1')
    d.cmd('run 50')
    check('Zentral Ein schaltet K1', d.relay_on(1))
    check('Zentral wirkt nicht auf K2 (ohne Zentralfunktion)', d.relay(2)['status'] is False)
    d.close()

    # Befund B2: Ohne gültigen Flash (readFlash mit Größe 0) bleibt die Spule
    # nach setup() stromlos. Beim Öffner ist damit die Last ein, der Status
    # meldet aber Aus - bis zum ersten Schaltbefehl.
    d = Device(meta, {'SWA_Ch2OperationMode': 1}, befund='B2')
    r = d.relay(2)
    check('Befund B2: Öffner ohne gültigen Flash startet passend zum Status (Aus = Spule bestromt)',
          r['on'] == (r['status'] is False), r)
    d.close()


def relay_lock_delay_scene(meta):
    print('3. Relais: Sperre, Einschaltverzögerung, Szene')
    d = Device(meta, {'SWA_Ch1BehaviorLock': 2, 'SWA_Ch1BehaviorUnlock': 3,
                      'SWA_Ch2TurnOnDelayBase': 0, 'SWA_Ch2TurnOnDelayTime': 2,
                      'SWA_Ch1ScenesActive': 1, 'SWA_Ch1SceneAActive': 1,
                      'SWA_Ch1SceneABehavior': 1, 'SWA_Ch1SceneANumber': 5})
    d.cmd(f'ko {ko(1, LOCK)} 1.1 1')
    d.cmd('run 50')
    check('Sperren mit "Ein": K1 zieht an', d.relay_on(1))
    check('Sperrstatus Ein gesendet', d.last(ko(1, LOCK_STATUS)) is True)
    d.cmd(f'ko {ko(1, SWITCH)} 1.1 0')
    d.cmd('run 50')
    check('gesperrt: Schalten Aus wirkt nicht', d.relay_on(1))
    d.cmd(f'ko {ko(1, LOCK)} 1.1 0')
    d.cmd('run 50')
    check('Entsperren "Schalten nachholen": K1 fällt ab', not d.relay_on(1))

    d.cmd(f'ko {ko(2, SWITCH)} 1.1 1')
    d.cmd('run 1500')
    check('K2 nach 1,5 s noch aus (Verzögerung 2 s)', not d.relay_on(2))
    d.cmd('run 600')
    check('K2 nach 2,1 s an', d.relay_on(2))

    d.cmd(f'ko {ko(1, SCENE)} 18.1 3')        # Szene 4: nicht belegt
    d.cmd('run 50')
    check('Szene 4 wirkt nicht', not d.relay_on(1))
    d.cmd(f'ko {ko(1, SCENE)} 18.1 4')        # Szene 5: Ein
    d.cmd('run 50')
    check('Szene 5 schaltet K1 ein', d.relay_on(1))
    d.close()


def binary_input(meta):
    print('4. Binäreingang: Flanken, Entprellung, zyklisch')
    d = Device(meta, {'BI_Channel2Debouncing': 50, 'BI_Channel1Periodic': 1,
                      'BI_Channel1PeriodicBase': 0, 'BI_Channel1PeriodicTime': 5})
    check('Startwert offen = Aus gesendet', d.last(BI_OUT[0]) is False and d.last(BI_OUT[1]) is False)
    m = d.mark()
    d.cmd('bi 1 1')
    d.cmd('run 50')
    check('Kontakt 1 geschlossen: Ein gesendet', d.last(BI_OUT[0]) is True)
    d.cmd('bi 1 0')
    d.cmd('run 50')
    check('Kontakt 1 offen: Aus gesendet', d.last(BI_OUT[0]) is False)

    m = d.mark()
    d.cmd('bi 2 1')
    d.cmd('run 30')
    d.cmd('bi 2 0')
    d.cmd('run 200')
    check('Preller 30 ms bei 50 ms Entprellung: kein Telegramm', not d.sent(m, BI_OUT[1]), d.sent(m, BI_OUT[1]))
    d.cmd('bi 2 1')
    d.cmd('run 100')
    check('Kontakt 2 länger als Entprellzeit: Ein gesendet', d.last(BI_OUT[1]) is True)

    m = d.mark()
    d.cmd('run 21000')
    n = len(d.sent(m, BI_OUT[0]))
    check('zyklisch alle 5 s: 4 Telegramme in 21 s', n == 4, n)
    d.close()


def ets_download(meta):
    print('5. ETS-Download')
    d = Device(meta)
    d.cmd(f'ko {ko(1, SWITCH)} 1.1 1')
    d.cmd('run 50')
    d.img.set('SWA_Ch2OperationMode', 1)
    d.cmd('pimg ' + d.img.hex())
    m = d.mark()
    d.cmd('ets 3000')
    phases = [e for e in d.events[m:] if e['ev'] in ('download', 'boot', 'flash')]
    check('Ablauf: Download, Flash gesichert, Neustart',
          [e['ev'] for e in phases] == ['download', 'flash', 'download', 'boot'], [e['ev'] for e in phases])
    d.cmd('run 100')
    check('K1 nach dem Download aus dem Flash wieder an', d.relay_on(1))
    check('neuer Parameter wirkt: K2 als Öffner ohne Befehl bestromt', d.relay_on(2), d.relay(2))
    d.close()


def bus_loss(meta):
    print('6. Busausfall und Busdip')
    d = Device(meta, {'SWA_Ch2BehaviorPowerLoss': 1, 'SWA_Ch2BehaviorPowerRegain': 2})
    d.cmd(f'ko {ko(1, SWITCH)} 1.1 1')
    d.cmd('run 50')
    d.cmd('bus 0')
    d.cmd('run 100')
    st = d.state()
    check('Bus aus: Prozessor aus, beide Relais abgefallen (monostabil)',
          not st['mcu'] and not d.relay_on(1) and not d.relay_on(2))
    d.cmd('bus 1')
    d.cmd('run 100')
    check('Bus ein: K1 aus dem Flash wieder an ("keine Änderung")', d.relay_on(1))

    d.cmd(f'ko {ko(2, SWITCH)} 1.1 0')
    d.cmd('run 50')
    m = d.mark()
    d.cmd('busdip 200')
    reboot = [e for e in d.events[m:] if e['ev'] == 'boot']
    check('Busdip 200 ms: kein Neustart', not reboot)
    check('Busdip: K2 nach Wiederkehr an ("bei Wiederkehr = Ein")', d.relay_on(2))
    check('Busdip: K1 unverändert an', d.relay_on(1))
    d.close()

    # Befund B1: "bei Busspannungswiederkehr" wirkt nur ohne Neustart. Nach
    # einem echten Busausfall startet der Prozessor neu, readFlash() stellt den
    # beim SAVE gespeicherten Zustand ("bei Busspannungsverlust") her, und
    # restorePower() wird nie gerufen.
    d = Device(meta, {'SWA_Ch1BehaviorPowerLoss': 1, 'SWA_Ch1BehaviorPowerRegain': 2}, befund='B1')
    d.cmd('bus 0')
    d.cmd('run 5000')
    d.cmd('bus 1')
    d.cmd('run 100')
    check('Befund B1: nach Busausfall mit Neustart wirkt "bei Wiederkehr = Ein"', d.relay_on(1), d.relay(1))
    d.close()


def supply_loss(meta):
    print('7. Ausfall der Relaisversorgung')
    d = Device(meta)
    d.cmd(f'ko {ko(1, SWITCH)} 1.1 1')
    d.cmd('run 50')
    d.cmd('supply 0')
    d.cmd('run 1000')
    r = d.relay(1)
    check('ohne Versorgung fällt K1 ab, Pin bleibt HIGH', not r['on'] and r['level'], r)
    check('Firmware meldet weiter Ein (kein Rückmeldekontakt)', r['status'] is True)
    d.cmd('supply 1')
    d.cmd('run 50')
    check('Versorgung zurück: K1 zieht ohne Befehl wieder an', d.relay_on(1))
    d.close()


def func_button(meta):
    print('8. Funktionstaste')
    d = Device(meta)
    m = d.mark()
    d.cmd('btn short')
    d.cmd('btn long')
    d.cmd('run 100')
    infos = [e for e in d.events[m:] if e['ev'] == 'info' and 'Funktionstaste' in e['text']]
    check('kein simuliertes Modul belegt die Taste (Meldung, keine Wirkung)', len(infos) == 2 and not d.sent(m))
    d.close()


def misconfig(meta):
    print('9. Fehlkonfiguration: Kanal inaktiv, nur ein Kanal verfügbar')
    d = Device(meta, {'SWA_Ch1Active': 0})
    d.cmd(f'ko {ko(1, SWITCH)} 1.1 1')
    d.cmd(f'ko {ko(2, SWITCH)} 1.1 1')
    d.cmd('run 50')
    check('inaktiver K1 schaltet nicht', not d.relay_on(1))
    check('Nachbar K2 läuft weiter', d.relay_on(2))
    d.close()

    d = Device(meta, {'SWA_VisibleChannels': 1})
    d.cmd(f'ko {ko(2, SWITCH)} 1.1 1')
    d.cmd(f'ko {ko(1, SWITCH)} 1.1 1')
    d.cmd('run 50')
    check('"Verfügbare Kanäle = 1": K2 bleibt aus', not d.relay_on(2))
    check('K1 läuft', d.relay_on(1))
    d.close()


def steady_bus_load(meta):
    print('10. Buslast im Beharrungszustand')
    d = Device(meta)
    d.cmd(f'ko {ko(1, SWITCH)} 1.1 1')
    d.cmd('run 1000')
    m = d.mark()
    d.cmd('run 600000')
    n = len(d.sent(m))
    check('10 min ohne Änderung: kein Telegramm', n == 0, n)
    d.close()


def console(meta):
    print('11. Konsole')
    d = Device(meta)
    d.cmd('cmd sa switch a 1')
    d.cmd('run 50')
    check('"sa switch a 1" schaltet K1', d.relay_on(1))
    d.cmd('cmd sa toggle a')
    d.cmd('run 50')
    check('"sa toggle a" schaltet K1 aus', not d.relay_on(1))
    m = d.mark()
    d.cmd('cmd sa test mode')
    pulses = [e['on'] for e in d.events[m:] if e['ev'] == 'relay']
    check('"sa test mode" zieht jedes Relais zweimal an',
          sum(p[0] for p in pulses) == 2 and sum(p[1] for p in pulses) == 2, pulses)
    # Der Testmodus wartet mit delay(500) - bewusst ausgeloest
    EXPECTED.add('Blockierung')
    blocking = [w for w in WARNS if w['inv'] == 'Blockierung']
    check('Wächter meldet die Blockierung durch delay() im Testmodus', blocking)
    d.close()


def main():
    meta = load_meta()
    unconfigured(meta)
    relay_basics(meta)
    relay_lock_delay_scene(meta)
    binary_input(meta)
    ets_download(meta)
    bus_loss(meta)
    supply_loss(meta)
    func_button(meta)
    misconfig(meta)
    steady_bus_load(meta)
    console(meta)

    print('Invariantenwächter:')
    for w in WARNS:
        note = f"(Befund {w['befund']})" if w['befund'] else '(erwartet)' if w['inv'] in EXPECTED else ''
        print('  ', w['inv'], '-', w['text'], note)
    if not WARNS:
        print('   keine Meldung')
    unexpected = [w for w in WARNS if not w['befund'] and w['inv'] not in EXPECTED]
    check('keine unerwartete Wächtermeldung', not unexpected, [w['text'] for w in unexpected][:3])

    print()
    print('ALLES OK' if not FAILS else f'{len(FAILS)} FEHLER: ' + '; '.join(FAILS))
    return 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(main())
