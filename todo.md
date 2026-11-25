# DxMap – offene Punkte

## 1. Vor einer Veröffentlichung

- [ ] Lizenz: Das Repo ist öffentlich, hat aber keine Lizenzdatei. Rechtlich heißt das „alle Rechte vorbehalten“, niemand darf den Code verwenden. Dazu kommt: WinDom steht unter der LGPL und wird statisch gelinkt. Weil dein Quelltext offen ist, ist das unproblematisch, sollte aber im README stehen. GPL oder MIT wären naheliegende Lizenzen.
- [ ] README: Das README aus der alten Unterhaltung ist nie ins Repo gekommen. Hinein gehören: Bauanleitung, Tastenkürzel, DXMAP.INF, unterstützte Systeme.
- [ ] Fertige Programme: Die .prg sind per .gitignore ausgeschlossen. Zum Herunterladen eignet sich ein GitHub-Release mit den drei Programmen.

## 2. Offene Tests

- [ ] Hatari mit TOS 4 steht noch aus. Dort interessieren vor allem 16 Farben (flache Darstellung), 256 Farben (Palette) und Monochrom.
- [ ] Einstellungsfenster: Hat noch niemand gesehen. Zu prüfen sind Layout, Return für OK, Popup und wo DXMAP.INF landet.
- [ ] Reines TOS ohne MiNT: Der Rückfall auf Fsfirst mit 8.3-Namen ist ungetestet.
- [ ] Rechtsklick über bclick = 258 ist auf TOS 1.x–3.x und EmuTOS ungetestet.
- [ ] Echter ST mit 1–4 MB: Wie verhält sich ein großer Scan, wenn der Speicher ausgeht?

## 3. Funktionen, die vermutlich fehlen

- [ ] Bedienung sichtbar machen: Zoom, Esc, T, D, + und − gibt es nur als Tasten. Sie stehen weder im Menü noch in der Info-Box. Menüeinträge wie „Ansicht → Hineinzoomen / Eine Ebene hoch“ und Kürzel im Menütext („Öffnen… ^O“) würden helfen.
- [ ] Ordner per Drag & Drop oder VA_START: Ziehst du einen Ordner auf das Fenster oder auf das Programm-Icon, während DxMap läuft, passiert nichts. Nur beim Start wird ein Pfad als Argument gelesen. Für beides gibt es Atari-Standardprotokolle, WinDom hat in examples/frame/dragdrop.c ein Beispiel.
- [ ] Dateiname beim Überfahren: Eine Sprechblase (BubbleGEM, WinDom hat examples/bubble) oder ein Live-Update der Statuszeile, ohne dass man klicken muss.
- [ ] Dateioperationen auch für Ordner: Der Rechtsklick funktioniert nur bei Dateien. Ordner lassen sich nicht löschen, kopieren oder verschieben. Nützlich wäre außerdem „Im Desktop zeigen“ bzw. „Öffnen“ über das AV-Protokoll.
- [ ] Legende der Dateitypen mit Anteilen: Welche Farbe ist was, und wie viel Platz belegt sie? Stand schon auf der alten Liste.
- [ ] Belegte Cluster statt Dateigröße: Zeigt den tatsächlichen Platzverbrauch, auch von der alten Liste.
- [ ] Ikonifizieren: Das Fenster hat keinen Iconify-Knopf (SMALLER).

## 4. Randfälle im Code

- [ ] Dateien über 4 GB: Fxattr und Dxreaddir liefern die Größe nur als 32-Bit-Wert. Unter MiNT mit ext2 oder hostfs werden solche Dateien zu klein gezählt. Abhilfe wäre Fstat64 für betroffene Dateien.
- [ ] Aufräumen aus dem Review: FLG_NOPAL ist überflüssig, bw/bh sind ungenutzt, dazu einige const-Warnungen mit -Wextra.
- [ ] ARAnyM/XaAES-Menüfehler: Er ist nur umgangen, nicht behoben. Wenn Hatari sauber ist, würde ich ihn als Eigenheit von XaAES bzw. ARAnyM abhaken.

Meine Empfehlung für die Reihenfolge: zuerst Lizenz und README, weil das Repo öffentlich ist. Danach die Bedienung sichtbar machen (Menüeinträge und Kürzel), dann Drag & Drop/VA_START. Die Tests aus Abschnitt 2 laufen parallel bei dir.
