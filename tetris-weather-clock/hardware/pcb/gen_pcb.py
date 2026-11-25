#!/usr/bin/env python3
"""Generatore della scheda ESP32 DevKit -> HUB75 (singolo pannello).

Produce: Gerber + Excellon, .kicad_pcb, BOM, netlist, anteprime PNG.
Uso:  python3 gen_pcb.py <cartella_output>
Il progetto e' interamente descritto qui: modifica le posizioni/le reti e rilancia.
"""
import math, random, heapq, os, sys, csv, time
import numpy as np
from PIL import Image, ImageDraw
from scipy import ndimage

OUT = sys.argv[1] if len(sys.argv) > 1 else "out"
os.makedirs(OUT, exist_ok=True)

W, H = 76.2, 57.15          # dimensioni scheda (mm)
G = 0.254                    # griglia del router
CLR = 0.2                    # distanza minima rame-rame
TW = 0.25                    # larghezza piste segnale
VIA_D, VIA_H = 0.8, 0.4
EDGE = 0.6                   # distanza minima delle piste dal bordo
POUR_INSET = 0.4
GND = "GND"

# ------------------------------------------------------------------ componenti
parts = []     # dict(ref,value,fp,pads=[(num,x,y,d,drill,net)], silk=[...])
holes = []     # fori non metallizzati (x,y,d)
silk = []      # segmenti ("l",x1,y1,x2,y2) ; testo gia' convertito in segmenti


def add(ref, value, fp, pads, note=""):
    parts.append(dict(ref=ref, value=value, fp=fp, pads=pads, note=note))


PD, DR = 1.7, 1.0
Lp = ['3V3', 'EN', 'VP', 'VN', 'IO34', 'IO35', 'IO32', 'IO33', 'IO25', 'IO26', 'IO27', 'IO14', 'IO12', 'GND',
      'IO13', 'SD2', 'SD3', 'CMD', '5V']
Rp = ['GND', 'IO23', 'IO22', 'TX', 'RX', 'IO21', 'GND', 'IO19', 'IO18', 'IO5', 'IO17', 'IO16', 'IO4', 'IO0',
      'IO2', 'IO15', 'SD1', 'SD0', 'CLK']
espnet = {'GND': GND, '5V': '+5V', 'IO25': 'R1_H', 'IO26': 'G1_H', 'IO27': 'B1_H', 'IO14': 'R2_H',
          'IO12': 'G2_H', 'IO13': 'B2_H', 'IO23': 'A_H', 'IO19': 'B_H', 'IO5': 'C_H', 'IO17': 'D_H',
          'IO18': 'E_H', 'IO16': 'CLK_H', 'IO4': 'LAT_H', 'IO15': 'OE_H'}
XL, XR = 15.24, 40.64
add("J1", "ESP32 DevKit sx", "Socket_1x19_P2.54",
    [(i + 1, XL, 5.08 + 2.54 * i, PD, DR, espnet.get(n)) for i, n in enumerate(Lp)], "femmina 1x19, lato sx")
add("J6", "ESP32 DevKit dx", "Socket_1x19_P2.54",
    [(i + 1, XR, 5.08 + 2.54 * i, PD, DR, espnet.get(n)) for i, n in enumerate(Rp)], "femmina 1x19, lato dx")


hub = ['R1', 'G1', 'B1', GND, 'R2', 'G2', 'B2', 'E', 'A', 'B', 'C', 'D', 'CLK', 'LAT', 'OE', GND]
hp = []
for n in range(1, 17):
    x = 50.8 if n % 2 else 53.34
    y = 7.62 + 2.54 * ((n - 1) // 2)
    hp.append((n, x, y, PD, DR, GND if hub[n - 1] == GND else f"{hub[n - 1]}_H"))
add("J_HUB", "HUB75 2x8", "IDC-Header_2x08_P2.54_Vertical", hp, "header box 2x8 per il flat del pannello")
add("J3", "Ingresso 5V", "TerminalBlock_5.08_1x2", [(1, 63.5, 50.8, 2.6, 1.4, '+5V'), (2, 68.58, 50.8, 2.6, 1.4, GND)],
    "morsetto: 5V dall'alimentatore, alimenta ESP e pannello")
add("J4", "Alim. pannello", "TerminalBlock_5.08_1x2", [(1, 63.5, 40.64, 2.6, 1.4, '+5V'), (2, 68.58, 40.64, 2.6, 1.4, GND)],
    "morsetto: cavo di alimentazione del pannello")
for k, (hx, hy) in enumerate([(4, 4), (W - 4, 4), (4, H - 4), (W - 4, H - 4)]):
    holes.append((hx, hy, 3.2))

NETW = {'+5V': 1.2, 'VBUS': 1.2, 'ESP_5V': 0.5, '3V3': 0.4}

# ------------------------------------------------------------------ serigrafia
from matplotlib.textpath import TextPath
from matplotlib.font_manager import FontProperties


def stext(x, y, s, h=1.6):
    tp = TextPath((0, 0), s, size=h, prop=FontProperties(family="DejaVu Sans"))
    for poly in tp.to_polygons():
        for a, b in zip(poly[:-1], poly[1:]):
            silk.append(("l", x + a[0], y - a[1], x + b[0], y - b[1]))


def srect(x1, y1, x2, y2):
    silk.extend([("l", x1, y1, x2, y1), ("l", x2, y1, x2, y2), ("l", x2, y2, x1, y2), ("l", x1, y2, x1, y1)])


def scirc(x, y, r, n=32):
    for k in range(n):
        a, b = 2 * math.pi * k / n, 2 * math.pi * (k + 1) / n
        silk.append(("l", x + r * math.cos(a), y + r * math.sin(a), x + r * math.cos(b), y + r * math.sin(b)))


srect(13.5, 2.0, 42.4, 53.6)                     # ingombro DevKit
stext(18.5, 27.5, "ESP32", 2.2)
stext(18.5, 31.0, "DevKit", 2.2)
stext(21.0, 51.5, "USB", 1.6)
srect(48.7, 5.2, 55.5, 27.8); stext(48.7, 4.9, "HUB75", 1.4)
srect(60.8, 46.7, 70.9, 54.8); stext(61.0, 46.3, "5V IN", 1.3)
srect(60.8, 36.5, 70.9, 44.6); stext(61.0, 36.1, "5V PANNELLO", 1.1)
# pin 1 degli header e dei chip
for (x, y) in [(XL, 5.08), (XR, 5.08), (50.8, 7.62)]:
    scirc(x - 1.5, y - 1.5, 0.4, 12)
for (x, y) in [(XL - 2.0, 5.08 + 2.54 * 18), (XR - 2.0, 5.08 + 2.54 * 18)]:
    pass
stext(11.0, 53.0, "5V", 1.1)
stext(2.5, 44.0, "TetrisClock", 1.5)
stext(2.5, 47.0, "HUB75 v1", 1.5)

# ------------------------------------------------------------------ rete / elementi di rame
allpads = []
for p in parts:
    for (num, x, y, d, dr, net) in p["pads"]:
        allpads.append(dict(ref=p["ref"], num=num, x=x, y=y, d=d, drill=dr, net=net or f"unconnected-{p['ref']}-{num}"))

nets = {}
for q in allpads:
    nets.setdefault(q["net"], []).append(q)

NX, NY = int(round(W / G)) + 1, int(round(H / G)) + 1


def paint(g, x, y, r):
    rc = r / G
    cx, cy = x / G, y / G
    i0, i1 = max(0, int(math.floor(cx - rc))), min(NX - 1, int(math.ceil(cx + rc)))
    j0, j1 = max(0, int(math.floor(cy - rc))), min(NY - 1, int(math.ceil(cy + rc)))
    if i0 > i1 or j0 > j1:
        return
    ii, jj = np.ogrid[i0:i1 + 1, j0:j1 + 1]
    g[i0:i1 + 1, j0:j1 + 1] |= ((ii - cx) ** 2 + (jj - cy) ** 2) <= rc * rc + 1e-9


def base_items():
    it = ([], [])
    for q in allpads:
        for l in (0, 1):
            it[l].append((q["x"], q["y"], q["d"] / 2, q["net"]))
    for (x, y, d) in holes:
        for l in (0, 1):
            it[l].append((x, y, d / 2 + 0.4, "#hole"))
    return it


def edge_mask(w):
    g = np.zeros((NX, NY), bool)
    m = EDGE + w / 2
    xs = np.arange(NX) * G
    ys = np.arange(NY) * G
    g[xs < m, :] = True
    g[xs > W - m, :] = True
    g[:, ys < m] = True
    g[:, ys > H - m] = True
    return g


def build_blocked(items, net, w, layer):
    g = edge_mask(w)
    for (x, y, r, n) in items[layer]:
        if n == net:
            continue
        paint(g, x, y, r + CLR + w / 2)
    return g


def dijkstra(bt, bv, sources, tree, w):
    NN = NX * NY
    tot = 2 * NN
    INF = 1e18
    dist = [INF] * tot
    prev = [-1] * tot
    flatb = [bt[0].reshape(-1), bt[1].reshape(-1)]
    flatv = [bv[0].reshape(-1), bv[1].reshape(-1)]
    h = []
    for s in sources:
        dist[s] = 0.0
        heapq.heappush(h, (0.0, s))
    while h:
        d, u = heapq.heappop(h)
        if d > dist[u]:
            continue
        if tree[u]:
            path = [u]
            while prev[path[-1]] != -1:
                path.append(prev[path[-1]])
            return path[::-1]
        l, rem = divmod(u, NN)
        i, j = divmod(rem, NY)
        # direzioni: strato 0 preferisce orizzontale, strato 1 verticale
        for di, dj in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            a, b = i + di, j + dj
            if a < 0 or b < 0 or a >= NX or b >= NY:
                continue
            v = l * NN + a * NY + b
            if flatb[l][a * NY + b] and not tree[v]:
                continue
            horiz = di != 0
            c = 1.0 if (horiz == (l == 0)) else 1.7
            nd = d + c
            if nd < dist[v]:
                dist[v] = nd
                prev[v] = u
                heapq.heappush(h, (nd, v))
        o = 1 - l
        v = o * NN + rem
        if (not flatv[l][rem]) and (not flatv[o][rem] or tree[v]):
            nd = d + 14.0
            if nd < dist[v]:
                dist[v] = nd
                prev[v] = u
                heapq.heappush(h, (nd, v))
    return None


def route_all(order):
    items = base_items()
    tracks, vias = [], []
    for net in order:
        pins = [(q["x"], q["y"]) for q in nets[net]]
        w = NETW.get(net, TW)
        bt = [build_blocked(items, net, w, l) for l in (0, 1)]
        bv = [build_blocked(items, net, max(w, VIA_D), l) for l in (0, 1)]
        NN = NX * NY
        tree = bytearray(2 * NN)

        def cid(l, x, y):
            return l * NN + int(round(x / G)) * NY + int(round(y / G))

        tree[cid(0, *pins[0])] = 1
        tree[cid(1, *pins[0])] = 1
        rest = pins[1:]
        while rest:
            # pin piu' vicino all'albero (distanza Manhattan fra pin)
            trpins = [p for p in pins if p not in rest]
            best = min(rest, key=lambda p: min(abs(p[0] - t[0]) + abs(p[1] - t[1]) for t in trpins))
            rest.remove(best)
            src = [cid(0, *best), cid(1, *best)]
            path = dijkstra(bt, bv, src, tree, w)
            if path is None:
                return None, None, net
            seg_start = None
            for k, u in enumerate(path):
                l, rem = divmod(u, NN)
                i, j = divmod(rem, NY)
                tree[u] = 1
                items[l].append((i * G, j * G, w / 2, net))
                if k > 0:
                    pl, prem = divmod(path[k - 1], NN)
                    pi, pj = divmod(prem, NY)
                    if pl != l:
                        vias.append((i * G, j * G, net))
                        items[0].append((i * G, j * G, VIA_D / 2, net))
                        items[1].append((i * G, j * G, VIA_D / 2, net))
                        tree[(1 - l) * NN + rem] = 1
                    else:
                        tracks.append([l, pi * G, pj * G, i * G, j * G, w, net])
        # unisci segmenti collineari
    return merge(tracks), vias, None


def merge(tr):
    out = []
    for t in tr:
        if out:
            o = out[-1]
            if o[0] == t[0] and o[6] == t[6] and o[5] == t[5] and abs(o[3] - t[1]) < 1e-9 and abs(o[4] - t[2]) < 1e-9:
                dx1, dy1 = o[3] - o[1], o[4] - o[2]
                dx2, dy2 = t[3] - t[1], t[4] - t[2]
                if abs(dx1 * dy2 - dy1 * dx2) < 1e-9 and (dx1 * dx2 + dy1 * dy2) > 0:
                    o[3], o[4] = t[3], t[4]
                    continue
        out.append(list(t))
    return out


def hpwl(net):
    xs = [q["x"] for q in nets[net]]
    ys = [q["y"] for q in nets[net]]
    return (max(xs) - min(xs)) + (max(ys) - min(ys))


routable = [n for n in nets if n != GND and len(nets[n]) > 1]
first = [n for n in ['+5V'] if n in routable]
sig = sorted([n for n in routable if n not in first], key=lambda n: -hpwl(n))

t0 = time.time()
result = None
rng = random.Random(7)
for attempt in range(40):
    order = first + sig
    tr, vi, bad = route_all(order)
    print(f"tentativo {attempt}: {'OK' if tr is not None else 'fallita su ' + str(bad)}  ({time.time() - t0:.0f}s)", flush=True)
    if tr is not None:
        result = (tr, vi)
        break
    if bad in sig:   # porta la rete difficile davanti
        sig.remove(bad)
        sig.insert(rng.randint(0, 2), bad)
    else:
        rng.shuffle(sig)
if result is None:
    sys.exit("routing non riuscito")
tracks, vias = result

# ------------------------------------------------------------------ via di cucitura GND
items = base_items()
for t in tracks:
    # campioni lungo la pista
    n = max(1, int(round(math.hypot(t[3] - t[1], t[4] - t[2]) / G)))
    for k in range(n + 1):
        x = t[1] + (t[3] - t[1]) * k / n
        y = t[2] + (t[4] - t[2]) * k / n
        items[t[0]].append((x, y, t[5] / 2, t[6]))
for v in vias:
    for l in (0, 1):
        items[l].append((v[0], v[1], VIA_D / 2, v[2]))
bvg = [build_blocked(items, GND, VIA_D, l) for l in (0, 1)]
stitch = []
gnd_pads = [q for q in allpads if q["net"] == GND]
step = int(round(5.08 / G))
for i in range(int(2.0 / G), NX, step):
    for j in range(int(2.0 / G), NY, step):
        x, y = i * G, j * G
        if bvg[0][i, j] or bvg[1][i, j]:
            continue
        # lontano dalle anelle termiche e dai fori
        if any(math.hypot(x - q["x"], y - q["y"]) < 2.4 for q in allpads):
            continue
        if any(math.hypot(x - hx, y - hy) < hd / 2 + 1.2 for hx, hy, hd in holes):
            continue
        # evita le zone dove le piste passano molto vicine (isole): richiede cella libera anche attorno
        if bvg[0][max(i - 3, 0):i + 4, max(j - 3, 0):j + 4].any() or bvg[1][max(i - 3, 0):i + 4, max(j - 3, 0):j + 4].any():
            continue
        stitch.append((x, y))
        for l in (0, 1):
            paint(bvg[l], x, y, VIA_D / 2 + CLR + VIA_D / 2 + 1.5)
print("via di cucitura:", len(stitch))
for (x, y) in stitch:
    vias.append((x, y, GND))

# ------------------------------------------------------------------ rasterizzazione (verifica e anteprima)
S = 0.05
RW, RH = int(W / S) + 1, int(H / S) + 1
THERM_R, SPOKE_W, SPOKE_L = 1.3, 0.5, 1.7
SPK = [(0.7071, 0.7071), (-0.7071, 0.7071), (0.7071, -0.7071), (-0.7071, -0.7071)]


def disc(dr, x, y, d, f):
    r = d / 2 / S
    dr.ellipse([x / S - r, y / S - r, x / S + r, y / S + r], fill=f)


def line(dr, x1, y1, x2, y2, w, f):
    dr.line([x1 / S, y1 / S, x2 / S, y2 / S], fill=f, width=max(1, int(round(w / S))))
    disc(dr, x1, y1, w, f)
    disc(dr, x2, y2, w, f)


def draw_items(dr, layer, f, only_net=None, skip_net=None):
    for t in tracks:
        if t[0] == layer and (only_net is None or t[6] == only_net) and (skip_net is None or t[6] != skip_net):
            line(dr, t[1], t[2], t[3], t[4], t[5], f)
    for q in allpads:
        if (only_net is None or q["net"] == only_net) and (skip_net is None or q["net"] != skip_net):
            disc(dr, q["x"], q["y"], q["d"], f)
    for v in vias:
        if (only_net is None or v[2] == only_net) and (skip_net is None or v[2] != skip_net):
            disc(dr, v[0], v[1], VIA_D, f)



def seg_dist(px, py, x1, y1, x2, y2):
    dx, dy = x2 - x1, y2 - y1
    L2 = dx * dx + dy * dy
    t = 0 if L2 == 0 else max(0, min(1, ((px - x1) * dx + (py - y1) * dy) / L2))
    return math.hypot(px - (x1 + t * dx), py - (y1 + t * dy))


def spoke_ok(layer, q, dx, dy):
    for k in range(0, 18):
        d = SPOKE_L * k / 17
        px, py = q["x"] + dx * d, q["y"] + dy * d
        need = CLR + SPOKE_W / 2 + 0.03
        if px < POUR_INSET + 0.3 or px > W - POUR_INSET - 0.3 or py < POUR_INSET + 0.3 or py > H - POUR_INSET - 0.3:
            return False
        for t in tracks:
            if t[0] == layer and t[6] != GND and seg_dist(px, py, t[1], t[2], t[3], t[4]) < need + t[5] / 2:
                return False
        for o in allpads:
            if o["net"] != GND and math.hypot(px - o["x"], py - o["y"]) < need + o["d"] / 2:
                return False
        for v in vias:
            if v[2] != GND and math.hypot(px - v[0], py - v[1]) < need + VIA_D / 2:
                return False
        for (hx, hy, hd) in holes:
            if math.hypot(px - hx, py - hy) < need + hd / 2 + 0.3:
                return False
    return True


SPOKES = {l: [(q, dx, dy) for q in gnd_pads for dx, dy in SPK if spoke_ok(l, q, dx, dy)] for l in (0, 1)}
print("spoke termiche valide:", len(SPOKES[0]), len(SPOKES[1]), "su", 4 * len(gnd_pads))

def layer_image(layer):
    img = Image.new("L", (RW, RH), 0)
    dr = ImageDraw.Draw(img)
    dr.rectangle([POUR_INSET / S, POUR_INSET / S, (W - POUR_INSET) / S, (H - POUR_INSET) / S], fill=255)
    # clear: rame di altre reti + clearance
    for t in tracks:
        if t[0] == layer and t[6] != GND:
            line(dr, t[1], t[2], t[3], t[4], t[5] + 2 * CLR, 0)
    for q in allpads:
        if q["net"] != GND:
            disc(dr, q["x"], q["y"], q["d"] + 2 * CLR, 0)
    for v in vias:
        if v[2] != GND:
            disc(dr, v[0], v[1], VIA_D + 2 * CLR, 0)
    for (hx, hy, hd) in holes:
        disc(dr, hx, hy, hd + 1.0, 0)
    for q in gnd_pads:
        disc(dr, q["x"], q["y"], 2 * THERM_R, 0)
    # rame (scuro)
    draw_items(dr, layer, 255)
    for (q, dx, dy) in SPOKES[layer]:
        line(dr, q["x"], q["y"], q["x"] + dx * SPOKE_L, q["y"] + dy * SPOKE_L, SPOKE_W, 255)
    return img


LAY = [layer_image(0), layer_image(1)]
LA = [np.array(im) > 127 for im in LAY]

# ---- verifica connettivita' (corti e aperti)
lab, comp = [], []
for l in (0, 1):
    lb, n = ndimage.label(LA[l])
    lab.append(lb)
parent = {}


def find(a):
    while parent.setdefault(a, a) != a:
        parent[a] = parent[parent[a]]
        a = parent[a]
    return a


def union(a, b):
    parent[find(a)] = find(b)


def pix(x, y):
    return int(round(x / S)), int(round(y / S))


problems = []
hole_pts = [(q["x"], q["y"], q["net"]) for q in allpads] + [(v[0], v[1], v[2]) for v in vias]
netgroups = {}
for (x, y, net) in hole_pts:
    px, py = pix(x, y)
    a, b = lab[0][py, px], lab[1][py, px]
    if a == 0 or b == 0:
        problems.append(f"rame mancante su un foro di {net} a ({x:.2f},{y:.2f})")
        continue
    union((0, a), (1, b))
    netgroups.setdefault(net, []).append((0, a))
for net, lst in netgroups.items():
    roots = {find(x) for x in lst}
    if len(roots) > 1 and not net.startswith("unconnected"):
        problems.append(f"APERTO: la rete {net} e' divisa in {len(roots)} parti")
rootnet = {}
for net, lst in netgroups.items():
    for x in lst:
        rootnet.setdefault(find(x), set()).add(net)
for r, ns in rootnet.items():
    real = {n for n in ns}
    if len(real) > 1:
        problems.append(f"CORTO fra {sorted(real)}")

# ---- verifica distanze minime (pista-pista e pista-platea)
for l in (0, 1):
    allm = {}
    for net in set(q["net"] for q in allpads) | {v[2] for v in vias} | {t[6] for t in tracks}:
        if net == GND:
            continue
        im = Image.new("L", (RW, RH), 0)
        draw_items(ImageDraw.Draw(im), l, 255, only_net=net)
        allm[net] = np.array(im) > 127
    union_m = np.zeros((RH, RW), bool)
    for m in allm.values():
        union_m |= m
    E = ndimage.distance_transform_edt(~union_m) * S
    gm = np.zeros((RH, RW), bool)
    gim = Image.new("L", (RW, RH), 0)
    draw_items(ImageDraw.Draw(gim), l, 255, only_net=GND)
    for (q, dx, dy) in SPOKES[l]:
        line(ImageDraw.Draw(gim), q['x'], q['y'], q['x'] + dx * SPOKE_L, q['y'] + dy * SPOKE_L, SPOKE_W, 255)
    gm = np.array(gim) > 127
    # platea pura = rame del layer meno tutte le entita' disegnate
    ent = Image.new("L", (RW, RH), 0)
    draw_items(ImageDraw.Draw(ent), l, 255)
    pour = LA[l] & ~(np.array(ent) > 127)
    v = (E < CLR - 0.06) & pour
    if v.sum() > 0:
        problems.append(f"layer {l}: {int(v.sum())} px di platea a meno di {CLR} mm dal rame di altre reti")
    # pista GND-entita' (pad/vie) vs altre reti
    v2 = (E < CLR - 0.06) & gm
    if v2.sum() > 0:
        problems.append(f"layer {l}: rame GND troppo vicino ad altre reti ({int(v2.sum())} px)")
    # reti fra loro
    names = list(allm)
    for n1 in names:
        Ei = ndimage.distance_transform_edt(~allm[n1]) * S
        for n2 in names:
            if n1 >= n2:
                continue
            mm = Ei[allm[n2]]
            if mm.size and mm.min() < CLR - 0.06:
                problems.append(f"layer {l}: distanza {mm.min():.3f} mm fra {n1} e {n2}")

ntr = len(tracks)
print(f"piste: {ntr}, via: {len(vias)}  (di cui cucitura {len(stitch)})")
if problems:
    print("PROBLEMI:")
    for p in problems:
        print("  -", p)
else:
    print("Controlli raster OK: nessun corto, nessun aperto, distanze >= %.2f mm" % CLR)
open(os.path.join(OUT, "drc_report.txt"), "w").write(
    "piste %d, via %d (cucitura %d)\n" % (ntr, len(vias), len(stitch)) + ("\n".join(problems) if problems else "OK: nessun corto, nessun aperto, distanza minima >= %.2f mm\n" % CLR))

# ------------------------------------------------------------------ Gerber
class Gb:
    def __init__(self, name, func):
        self.name, self.func = name, func
        self.ap = {}
        self.body = []

    def a(self, kind, *dims):
        k = (kind,) + dims
        if k not in self.ap:
            self.ap[k] = 10 + len(self.ap)
        return self.ap[k]

    def sel(self, k):
        self.body.append(f"D{k}*")

    def xy(self, x, y):
        return f"X{int(round(x * 1e6))}Y{int(round((H - y) * 1e6))}"

    def flash(self, x, y, d):
        self.sel(self.a("C", round(d, 4)))
        self.body.append(self.xy(x, y) + "D03*")

    def line(self, x1, y1, x2, y2, w):
        self.sel(self.a("C", round(w, 4)))
        self.body.append(self.xy(x1, y1) + "D02*")
        self.body.append(self.xy(x2, y2) + "D01*")

    def region(self, pts):
        self.body.append("G36*")
        self.body.append(self.xy(*pts[0]) + "D02*")
        for p in pts[1:] + [pts[0]]:
            self.body.append(self.xy(*p) + "D01*")
        self.body.append("G37*")

    def pol(self, dark):
        self.body.append("%LPD*%" if dark else "%LPC*%")

    def save(self):
        hdr = ["%TF.GenerationSoftware,TetrisClock,gen_pcb,1.0*%", f"%TF.FileFunction,{self.func}*%",
               "%TF.FilePolarity,Positive*%", "%FSLAX46Y46*%", "%MOMM*%", "G01*"]
        for k, code in self.ap.items():
            if k[0] == "C":
                hdr.append(f"%ADD{code}C,{k[1]}*%")
        with open(os.path.join(OUT, self.name), "w") as f:
            f.write("\n".join(hdr + ["%LPD*%"] + self.body + ["M02*"]) + "\n")


names = {0: ("tetris-hub75-F_Cu.gbr", "Copper,L1,Top"), 1: ("tetris-hub75-B_Cu.gbr", "Copper,L2,Bot")}
for l in (0, 1):
    g = Gb(*names[l])
    g.region([(POUR_INSET, POUR_INSET), (W - POUR_INSET, POUR_INSET), (W - POUR_INSET, H - POUR_INSET), (POUR_INSET, H - POUR_INSET)])
    g.pol(False)
    for t in tracks:
        if t[0] == l and t[6] != GND:
            g.line(t[1], t[2], t[3], t[4], t[5] + 2 * CLR)
    for q in allpads:
        if q["net"] != GND:
            g.flash(q["x"], q["y"], q["d"] + 2 * CLR)
    for v in vias:
        if v[2] != GND:
            g.flash(v[0], v[1], VIA_D + 2 * CLR)
    for (hx, hy, hd) in holes:
        g.flash(hx, hy, hd + 1.0)
    for q in gnd_pads:
        g.flash(q["x"], q["y"], 2 * THERM_R)
    g.pol(True)
    for t in tracks:
        if t[0] == l:
            g.line(t[1], t[2], t[3], t[4], t[5])
    for q in allpads:
        g.flash(q["x"], q["y"], q["d"])
    for v in vias:
        g.flash(v[0], v[1], VIA_D)
    for (q, dx, dy) in SPOKES[l]:
        g.line(q["x"], q["y"], q["x"] + dx * SPOKE_L, q["y"] + dy * SPOKE_L, SPOKE_W)
    g.save()

for l, nm, fn in ((0, "tetris-hub75-F_Mask.gbr", "Soldermask,Top"), (1, "tetris-hub75-B_Mask.gbr", "Soldermask,Bot")):
    g = Gb(nm, fn)
    for q in allpads:
        g.flash(q["x"], q["y"], q["d"] + 0.1)
    g.save()

g = Gb("tetris-hub75-F_Silkscreen.gbr", "Legend,Top")
for s in silk:
    g.line(s[1], s[2], s[3], s[4], 0.15)
g.save()

g = Gb("tetris-hub75-Edge_Cuts.gbr", "Profile,NP")
pts = [(0, 0), (W, 0), (W, H), (0, H), (0, 0)]
for a, b in zip(pts[:-1], pts[1:]):
    g.line(a[0], a[1], b[0], b[1], 0.1)
g.save()


def drl(name, plated, tools):
    # tools: dict diametro -> [(x,y)]
    lines = ["M48", "; DRILL file {TetrisClock gen_pcb}", f"; FORMAT={{-:-/ absolute / metric / decimal}}",
             "FMAT,2", "METRIC,TZ"]
    codes = {}
    for k, d in enumerate(sorted(tools), 1):
        codes[d] = k
        lines.append(f"T{k}C{d:.3f}")
    lines += ["%", "G90", "G05"]
    for d in sorted(tools):
        lines.append(f"T{codes[d]}")
        for (x, y) in tools[d]:
            lines.append(f"X{x:.4f}Y{H - y:.4f}")
    lines += ["T0", "M30"]
    open(os.path.join(OUT, name), "w").write("\n".join(lines) + "\n")


tools = {}
for q in allpads:
    tools.setdefault(round(q["drill"], 3), []).append((q["x"], q["y"]))
for v in vias:
    tools.setdefault(VIA_H, []).append((v[0], v[1]))
drl("tetris-hub75-PTH.drl", True, tools)
drl("tetris-hub75-NPTH.drl", False, {3.2: [(x, y) for x, y, d in holes]})

# ------------------------------------------------------------------ KiCad
netnames = sorted({q["net"] for q in allpads if not q["net"].startswith("unconnected")} | {t[6] for t in tracks})
nid = {n: i + 1 for i, n in enumerate(netnames)}
k = []
k.append('(kicad_pcb (version 20221018) (generator "gen_pcb")')
k.append('  (general (thickness 1.6))')
k.append('  (paper "A4")')
k.append('  (layers (0 "F.Cu" signal) (31 "B.Cu" signal) (36 "B.SilkS" user "B.Silkscreen") (37 "F.SilkS" user "F.Silkscreen")'
         ' (38 "B.Mask" user "B.Mask") (39 "F.Mask" user "F.Mask") (44 "Edge.Cuts" user) (47 "F.CrtYd" user "F.Courtyard") (49 "F.Fab" user "F.Fabrication"))')
k.append('  (setup (pad_to_mask_clearance 0.05) (pcbplotparams (layerselection 0x00010fc_ffffffff)))')
k.append('  (net 0 "")')
for n in netnames:
    k.append(f'  (net {nid[n]} "{n}")')
for p in parts:
    x0, y0 = p["pads"][0][1], p["pads"][0][2]
    k.append(f'  (footprint "TetrisClock:{p["fp"]}" (layer "F.Cu") (at {x0:.3f} {y0:.3f})')
    k.append(f'    (property "Reference" "{p["ref"]}") (property "Value" "{p["value"]}")')
    k.append(f'    (fp_text reference "{p["ref"]}" (at 0 -2.2) (layer "F.SilkS") (effects (font (size 1 1) (thickness 0.15))))')
    k.append(f'    (fp_text value "{p["value"]}" (at 0 2.2) (layer "F.Fab") (effects (font (size 1 1) (thickness 0.15))))')
    for (num, x, y, d, dr, net) in p["pads"]:
        shape = "rect" if num == 1 and p["ref"] not in ("U1", "U2") and False else "circle"
        nt = f' (net {nid[net]} "{net}")' if net and not net.startswith("unconnected") else ""
        k.append(f'    (pad "{num}" thru_hole {shape} (at {x - x0:.3f} {y - y0:.3f}) (size {d} {d}) (drill {dr}) (layers "*.Cu" "*.Mask"){nt})')
    k.append('  )')
for i, (hx, hy, hd) in enumerate(holes):
    k.append(f'  (footprint "MountingHole:MountingHole_3.2mm_M3" (layer "F.Cu") (at {hx:.3f} {hy:.3f})')
    k.append(f'    (fp_text reference "H{i + 1}" (at 0 -3) (layer "F.SilkS") (effects (font (size 1 1) (thickness 0.15))))')
    k.append('    (pad "" np_thru_hole circle (at 0 0) (size 3.2 3.2) (drill 3.2) (layers "*.Cu" "*.Mask")))')
for t in tracks:
    ly = "F.Cu" if t[0] == 0 else "B.Cu"
    k.append(f'  (segment (start {t[1]:.3f} {t[2]:.3f}) (end {t[3]:.3f} {t[4]:.3f}) (width {t[5]}) (layer "{ly}") (net {nid[t[6]]}))')
for v in vias:
    k.append(f'  (via (at {v[0]:.3f} {v[1]:.3f}) (size {VIA_D}) (drill {VIA_H}) (layers "F.Cu" "B.Cu") (net {nid[v[2]]}))')
for a, b in zip(pts[:-1], pts[1:]):
    k.append(f'  (gr_line (start {a[0]} {a[1]}) (end {b[0]} {b[1]}) (stroke (width 0.1) (type default)) (layer "Edge.Cuts"))')
for ly in ("F.Cu", "B.Cu"):
    k.append(f'  (zone (net {nid[GND]}) (net_name "{GND}") (layer "{ly}") (hatch edge 0.5)')
    k.append('    (connect_pads (clearance 0.2)) (min_thickness 0.25)')
    k.append('    (fill yes (thermal_gap 0.3) (thermal_bridge_width 0.5))')
    k.append(f'    (polygon (pts (xy {POUR_INSET} {POUR_INSET}) (xy {W - POUR_INSET} {POUR_INSET}) (xy {W - POUR_INSET} {H - POUR_INSET}) (xy {POUR_INSET} {H - POUR_INSET}))))')
k.append(')')
open(os.path.join(OUT, "tetris-hub75.kicad_pcb"), "w").write("\n".join(k) + "\n")

# ------------------------------------------------------------------ BOM + netlist
with open(os.path.join(OUT, "BOM.csv"), "w", newline="") as f:
    w = csv.writer(f)
    w.writerow(["Rif", "Valore", "Footprint", "Q.ta", "Note"])
    for p in parts:
        w.writerow([p["ref"], p["value"], p["fp"], 1, p["note"]])
with open(os.path.join(OUT, "netlist.csv"), "w", newline="") as f:
    w = csv.writer(f)
    w.writerow(["Rete", "Pin"])
    for n in sorted(nets):
        if n.startswith("unconnected"):
            continue
        w.writerow([n, " ".join(f"{q['ref']}.{q['num']}" for q in nets[n])])

# ------------------------------------------------------------------ anteprime
def preview(side, fn):
    PS = 20  # px/mm
    im = Image.new("RGB", (int(W * PS), int(H * PS)), (16, 70, 40))
    cu = LAY[side].resize((int(W * PS), int(H * PS)), Image.NEAREST)
    cu = np.array(cu) > 127
    arr = np.array(im)
    arr[cu] = (30, 130, 78)
    im = Image.fromarray(arr)
    dr = ImageDraw.Draw(im)
    # piste del lato scelto piu' chiare
    for t in tracks:
        if t[0] == side:
            dr.line([t[1] * PS, t[2] * PS, t[3] * PS, t[4] * PS], fill=(214, 168, 62), width=int(t[5] * PS))
            r = t[5] * PS / 2
            for (x, y) in ((t[1], t[2]), (t[3], t[4])):
                dr.ellipse([x * PS - r, y * PS - r, x * PS + r, y * PS + r], fill=(214, 168, 62))
    for q in allpads:
        r = q["d"] / 2 * PS
        dr.ellipse([q["x"] * PS - r, q["y"] * PS - r, q["x"] * PS + r, q["y"] * PS + r], fill=(235, 200, 90))
    for v in vias:
        r = VIA_D / 2 * PS
        dr.ellipse([v[0] * PS - r, v[1] * PS - r, v[0] * PS + r, v[1] * PS + r], fill=(235, 200, 90))
    for q in allpads:
        r = q["drill"] / 2 * PS
        dr.ellipse([q["x"] * PS - r, q["y"] * PS - r, q["x"] * PS + r, q["y"] * PS + r], fill=(10, 10, 10))
    for v in vias:
        r = VIA_H / 2 * PS
        dr.ellipse([v[0] * PS - r, v[1] * PS - r, v[0] * PS + r, v[1] * PS + r], fill=(10, 10, 10))
    for (hx, hy, hd) in holes:
        r = hd / 2 * PS
        dr.ellipse([hx * PS - r, hy * PS - r, hx * PS + r, hy * PS + r], fill=(10, 10, 10))
    if side == 0:
        for s in silk:
            dr.line([s[1] * PS, s[2] * PS, s[3] * PS, s[4] * PS], fill=(245, 245, 245), width=2)
    else:
        im = im.transpose(Image.FLIP_LEFT_RIGHT)
    im.save(os.path.join(OUT, fn))


preview(0, "anteprima-top.png")
preview(1, "anteprima-bottom.png")
print("fatto in %.0fs" % (time.time() - t0))
