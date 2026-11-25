# DxMap

**Treemap der Plattenbelegung für Atari ST/STE/TT/Falcon, Milan, Hades und FireBee**

DxMap zeigt, wo auf einem Laufwerk oder in einem Ordner der Platz bleibt. Jede Datei
ist ein Rechteck, dessen Fläche ihrer Größe entspricht; Ordner sind verschachtelte
Rechtecke. Große Brocken fallen so auf einen Blick auf. Das Prinzip kennt man von
WinDirStat, GdMap oder KDirStat – DxMap bringt es als GEM-Programm auf den Atari.

Der Name kommt von `Dxreaddir()`, dem MiNT-Aufruf, mit dem DxMap Verzeichnisse liest,
und *Map*.

> **Status: Vorabversion 0.1.0.** Getestet unter ARAnyM mit XaAES und unter
> MagiC-on-Linux. Tests unter Hatari/TOS 4, reinem TOS und auf echter Hardware stehen
> noch aus (siehe [`todo.md`](todo.md)).

## Funktionen

- Treemap im Fenster, größenveränderbar, mit Menü in der Fensterzeile
- Farben nach Dateityp (Programme, Archive, Bilder, Musik, Texte, Quelltexte …)
- Kissen-Schattierung ab 256 Farben, flache Farben bei 16 Farben, Muster in
  Monochrom und 4 Farben
- Lange Dateinamen unter MiNT und MagiC
- Zoom in Ordner hinein und wieder heraus
- Kopieren, Verschieben und Löschen von Dateien per Rechtsklick
- Statusleiste mit Pfad und Größe der gewählten Datei
- Einstellungsfenster, Einstellungen dauerhaft in `DXMAP.INF`
- Zeitmessung der einzelnen Phasen (Scan, Sortieren, Layout, Zeichnen)

## Systemvoraussetzungen

| | |
|---|---|
| Betriebssystem | TOS, EmuTOS, MiNT/FreeMiNT, MagiC |
| AES | jedes GEM-AES; mit Multitasking-AES (XaAES, MagiC, N.AES) im Fenster neben anderen Programmen |
| CPU | `DXMAP.PRG`: jeder 68000 aufwärts · `DXMAP020.PRG`: 68020–68060 · `DXMAPCF.PRG`: ColdFire (FireBee) |
| Farben | ab 2 Farben; Kissen-Schattierung ab 256 Farben |
| Speicher | pro Datei etwa 32 Byte plus Namenslänge; ein Scan mit 60 000 Dateien braucht grob 3 MB |

## Wie es funktioniert

**Scannen.** DxMap liest den gewählten Ordner mit allen Unterordnern ein:

- unter MiNT und MagiC mit `Dopendir()`/`Dxreaddir()`, das Name und Dateiattribute in
  einem Aufruf liefert (fehlt `Dxreaddir()`, mit `Dreaddir()` + `Fxattr()`); lange
  Namen werden unterstützt, symbolische Links und Gerätedateien übersprungen,
- unter reinem TOS mit `Fsfirst()`/`Fsnext()` und 8.3-Namen.

Der Scan läuft in Zeitscheiben von 50 ms. Dazwischen bleibt das Programm bedienbar,
und **Esc** bricht ab. Knoten und Namen liegen in einem Block-Allokator, also in
32-KB-Blöcken statt einzelner `malloc()`-Aufrufe.

**Layout.** Die Rechtecke werden nach dem *Squarified-Treemap*-Verfahren aufgeteilt,
das möglichst quadratische Kacheln erzeugt. Flächen mit einer Kante unter der
*Mindestgröße* (Voreinstellung 2 Pixel) werden nicht weiter zerlegt, sondern als eine
graue Kachel gezeichnet. Das spart bei großen Bäumen viel Rechenzeit; diese Flächen
wären ohnehin nur Pixelkrümel. Wer jede Datei einzeln sehen will, stellt die
Mindestgröße auf 1.

**Zeichnen.** Alles läuft über das VDI. Beim Vergrößern des Fensters wird erst neu
berechnet, wenn die Größe 300 ms lang unverändert war; bis dahin zeigt die Busy Bee,
dass gearbeitet wird.

## Bedienung

### Start

DxMap öffnet ein Fenster und fragt per Dateiauswahl nach dem Laufwerk oder Ordner.
Alternativ kann ein Pfad als Argument übergeben werden, z. B. `DXMAP.PRG C:\GAMES`
oder per Ziehen eines Ordners auf das Programm-Icon am Desktop.

### Maus

| Aktion | Wirkung |
|---|---|
| Linksklick auf Kachel | Datei bzw. Ordner wählen; Pfad und Größe erscheinen in der Statusleiste |
| Doppelklick | eine Ebene tiefer zoomen, in den Ordner, der die Kachel enthält |
| Rechtsklick auf Datei | Popup *Kopieren… / Verschieben… / Löschen* |

Beim Kopieren und Verschieben wird das Ziel über aufklappende Popups gewählt:
Laufwerk → Ordner → Unterordner … mit *„hierher“*, *„.. Zurück“*, Blättern bei langen
Listen, *„Zuletzt“* für das letzte Ziel und *„Dateiauswahl…“* für die normale
Dateiauswahl. Vor dem Überschreiben und vor dem Löschen wird nachgefragt. Die Treemap
wird danach ohne neuen Scan angepasst.

### Tasten

| Taste | Wirkung |
|---|---|
| **Strg+O** / **O** | Ordner öffnen (neuer Scan) |
| **Strg+R** / **R** | aktualisieren (denselben Ordner neu scannen) |
| **Strg+E** | Einstellungen |
| **Esc** | laufenden Scan abbrechen (Ergebnis wird als unvollständig markiert) |
| **Return** | eine Ebene tiefer zoomen (wie Doppelklick) |
| **Backspace** | eine Ebene heraus |
| **Strg+C** / **C** | Kissen-Schattierung ein/aus |
| **G** | Ordnerränder ein/aus |
| **+** / **−** | Mindestgröße der Kacheln erhöhen/verringern |
| **T** | Zeitmessung in der Statusleiste ein/aus |
| **D** | Fenster mit der Aufschlüsselung der Scan-Zeit (genauer, wenn **T** vor dem Scan an war) |
| **Strg+I** / **H** / **?** | Info-Fenster (Version, Copyright, Kurzhilfe) |
| **Strg+Q** / **Q** | beenden |

### Menü

- **DxMap → Info…** (^I)
- **Datei → Öffnen…** (^O), **Einstellungen…** (^E), **Beenden** (^Q)
- **Ansicht → Farbmodus** (^C, Kissen-Schattierung mit Häkchen) und **Aktualisieren** (^R)

Info, Einstellungen und Scan-Details öffnen sich in eigenen Fenstern statt in
Alert-Boxen. Unter MiNT und MagiC laufen andere Programme dabei weiter.

### Einstellungen

*Datei → Einstellungen…* öffnet ein eigenes Fenster:

- **Ordnerränder anzeigen** – Ordner bekommen einen 1-Pixel-Rahmen
- **Kissen-Schattierung (Farbmodus)** – nur ab 256 Farben wählbar
- **Mindestgröße** – Popup-Liste 1 bis 16 Pixel (1 = jede Datei einzeln)
- **Versteckte Dateien mitzählen** – gilt ab dem nächsten Scan

**OK** übernimmt, **Abbruch** verwirft, **Sichern** übernimmt und schreibt
`DXMAP.INF` in das Verzeichnis, aus dem DxMap gestartet wurde. Die Datei wird beim
Start gelesen und ist eine einfache Textdatei:

```
; DxMap settings
dirs=1
cushions=1
hidden=1
minpx=2
```

### Titel- und Statusleiste

Der Fenstertitel zeigt den angezeigten Ordner mit Gesamtgröße sowie die Zahl der
Dateien und Ordner darin, z. B. `DxMap: C:\GAMES  (12.3 MB, 8459 Dateien, 1009 Ordner)`.
Nach dem Hineinzoomen gelten die Angaben für den gezoomten Ordner.


Unten im Fenster stehen während des Scans die Zahl der Dateien und Ordner, danach
Pfad und Größe der gewählten Datei, das Ergebnis der letzten Dateioperation oder die
Scan-Dauer. Mit **T** kommen rechts die Messwerte für Scan, Sortieren, Layout und
Zeichnen sowie die Zahl der Kacheln dazu.

## Herunterladen

Fertige Programme gibt es unter
[Releases](https://github.com/ragnar76/dxmap/releases). Die passende Datei nach
Prozessor wählen (siehe oben) und z. B. in `DXMAP.PRG` umbenennen.

## Selbst bauen

Benötigt wird die m68k-atari-mint-Cross-Toolchain mit

- [MiNTLib](https://github.com/freemint/mintlib)
- [GEMlib](https://github.com/freemint/gemlib)
- WinDom 2.0.1 (`libwindom`)
- LDG (`libldg`)

```sh
make            # baut dxmap.prg, dxmap020.prg und dxmapcf.prg
make clean
```

Der gesamte Quelltext steckt in `dxmap.c`; das Menü und das Einstellungsfenster sind
im Code aufgebaut, eine RSC-Datei gibt es nicht.

## Copyright und Lizenz

Copyright © 2026 Bernd Mueller

DxMap ist freie Software: Sie dürfen es unter den Bedingungen der
**GNU General Public License**, wie von der Free Software Foundation veröffentlicht,
weitergeben und/oder ändern, entweder gemäß **Version 3** der Lizenz oder (nach Ihrer
Wahl) jeder späteren Version. Den vollständigen Text enthält die Datei
[`LICENSE`](LICENSE).

DxMap wird in der Hoffnung verbreitet, dass es nützlich ist, aber **ohne jede
Gewährleistung** – sogar ohne die implizite Gewährleistung der Marktreife oder der
Eignung für einen bestimmten Zweck. Dateioperationen (Kopieren, Verschieben,
Löschen) erfolgen auf eigenes Risiko.

DxMap wird statisch gegen **WinDom** (© WinDom-Autoren, GNU LGPL 2.1), die
**GEMlib** und die **MiNTLib** gelinkt. Deren Lizenzen gelten für die jeweiligen
Teile; da der vollständige Quelltext von DxMap offenliegt, lassen sich die
Programme jederzeit gegen geänderte Versionen dieser Bibliotheken neu linken.
