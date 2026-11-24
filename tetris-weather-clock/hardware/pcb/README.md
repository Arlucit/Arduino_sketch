# Scheda ESP32 → HUB75 (singolo pannello)

Scheda 101,6 × 57,15 mm, 2 strati, tutta con componenti a foro passante. Su un lato si infila l'ESP32 DevKit a 38 pin (due strip femmina 1x19), sull'altro c'è il connettore HUB75 2x8: **niente cavi volanti**. Alimentazione da USB-C (alimentatore del Raspberry) che alimenta sia l'ESP sia il pannello tramite i morsetti 5V.

## File
- `tetris-hub75-gerber.zip`: Gerber + fori, da caricare direttamente da un produttore (JLCPCB, PCBWay...)
- `tetris-hub75.kicad_pcb`: progetto KiCad (non ancora aperto in KiCad: controllare)
- `BOM.csv`, `netlist.csv`, `anteprima-top.png`, `anteprima-bottom.png`, `drc_report.txt`
- `gen_pcb.py`: generatore (posizioni, reti, router, controlli, esportazione). `python3 gen_pcb.py out`

## Circuito
- 2× 74HCT245 portano i segnali dell'ESP (3,3 V) a 5 V per il pannello.
- Mappa GPIO: R1=25 G1=26 B1=27 R2=14 G2=12 B2=13 A=23 B=19 C=5 D=17 E=18 CLK=16 LAT=4 OE=15, LDR=35.
- USB-C (modulino con resistenze CC 5,1k) → PTC 3 A → rail +5V → morsetti J3/J4 (pannello) e C1 1000 µF.
- Diodo 1N5819: dal rail all'ESP (5V), così l'USB dell'ESP non rialimenta il pannello.
- Pull-up 10k su OE (lato ESP) per tenere il pannello spento durante il reset.
- J5: fotoresistenza tra 3V3 e IO35 con 10k a massa.

## Da controllare prima di ordinare
1. Piedinatura della **tua** DevKit (assunta: DevKitC V4, 38 pin, file in `gen_pcb.py` liste `Lp`/`Rp`).
2. Orientamento del pin 1 dell'HUB75 e del modulo USB-C.
3. Aprire i Gerber in un visualizzatore (es. gerbv, JLCPCB viewer) ed eseguire il DRC in KiCad.
4. Con l'alimentatore USB-C da ~3 A basta per **un** pannello a luminosità moderata.
Controlli interni già passati: nessun corto, nessuna rete interrotta, distanze ≥ 0,2 mm.
