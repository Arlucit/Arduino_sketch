# Scheda ESP32 → HUB75 (singolo pannello, senza componenti extra)

Scheda 76,2 × 57,15 mm, 2 strati. Montati ci sono solo: due strip femmina 1x19 per la DevKit a 38 pin, un header HUB75 2x8 e due morsetti a vite a 5 V. Nessun chip, condensatore, fusibile o diodo.

- I segnali vanno **direttamente** dai GPIO ai pin HUB75 (R1=25 G1=26 B1=27 R2=14 G2=12 B2=13 A=23 B=19 C=5 D=17 E=18 CLK=16 LAT=4 OE=15).
- Il 5 V dal morsetto "5V IN" alimenta sia il pin 5V dell'ESP sia il morsetto "5V PANNELLO". Non collegare il USB dell'ESP mentre e' alimentato dal morsetto, salvo sapere che la DevKit ha il diodo di protezione.
- Il fotoresistore (IO35) non e' sulla scheda: va collegato a parte.
- `tetris-hub75-gerber.zip` per la produzione, `tetris-hub75.kicad_pcb` (non provato in KiCad), `BOM.csv`, `netlist.csv`, anteprime, `gen_pcb.py` (generatore).
- `gen_pcb_con_buffer.py`: variante precedente con 2x 74HCT245, USB-C, PTC, diodo, condensatori (per quando servira').

Da verificare prima di ordinare: piedinatura della propria DevKit (liste `Lp`/`Rp`), pin 1 dell'HUB75, DRC in KiCad. Controlli interni passati: nessun corto, nessun aperto, distanze >= 0,2 mm.
