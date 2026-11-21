/*******************************************************************
  Motore Tetris per l'orologio (senza dipendenze, C++11).

  - Ogni cifra e' una griglia 3x7 di celle da 4x4 pixel (cifra 12x28 px, tratto spesso)
  - La cifra viene "tassellata" con tetramini veri (I, O, T, S, Z, J, L)
    con i colori classici; i pezzi avanzati sono tromini/domino/monomini
  - I pezzi cadono uno alla volta, nell'ordine giusto (prima quelli in
    basso), possono ruotare e spostarsi di lato durante la caduta e si
    impilano esattamente a formare la cifra
  - Quando una cifra cambia, i vecchi pezzi cadono fuori dal pannello
  - Tutto il percorso di ogni pezzo e' calcolato in anticipo e verificato
    cella per cella, quindi l'animazione non puo' sbagliare la posizione
  - Il tempo e' misurato in "tick": la velocita' si regola cambiando la
    durata del tick nel chiamante
*******************************************************************/
#pragma once
#include <stdint.h>
#include <string.h>
#include <algorithm>
#include <vector>

namespace tet {

static const int CELL = 4;        // pixel per cella
static const int GW = 3;          // colonne della cifra
static const int GH = 7;          // righe della cifra
static const int GLYPH_Y = 2;     // y in pixel della riga 0 della cifra
static const int SPAWN_TOP = -1;  // riga (in celle) da cui spuntano i pezzi
static const int EXIT_ROW = 9;    // riga oltre la quale un pezzo e' fuori dal pannello
static const int MAXP = 32;

static const char *const FONT[11][GH] = {
    {"XXX", "X.X", "X.X", "X.X", "X.X", "X.X", "XXX"},  // 0
    {".X.", "XX.", ".X.", ".X.", ".X.", ".X.", "XXX"},  // 1
    {"XXX", "..X", "..X", "XXX", "X..", "X..", "XXX"},  // 2
    {"XXX", "..X", "..X", "XXX", "..X", "..X", "XXX"},  // 3
    {"X.X", "X.X", "X.X", "XXX", "..X", "..X", "..X"},  // 4
    {"XXX", "X..", "X..", "XXX", "..X", "..X", "XXX"},  // 5
    {"XXX", "X..", "X..", "XXX", "X.X", "X.X", "XXX"},  // 6
    {"XXX", "..X", "..X", "..X", "..X", "..X", "..X"},  // 7
    {"XXX", "X.X", "X.X", "XXX", "X.X", "X.X", "XXX"},  // 8
    {"XXX", "X.X", "X.X", "XXX", "..X", "..X", "XXX"},  // 9
    {"...", "...", "...", "...", "...", "...", "..."},  // vuota
};

struct Rng {
  uint32_t s;
  explicit Rng(uint32_t seed = 2463534242u) : s(seed ? seed : 1u) {}
  uint32_t next() {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
  }
  int below(int n) { return n <= 1 ? 0 : (int)(next() % (uint32_t)n); }
};

struct Cell {
  int8_t x, y;
};
static inline Cell mk(int x, int y) {
  Cell c;
  c.x = (int8_t)x;
  c.y = (int8_t)y;
  return c;
}

struct Step {
  int8_t j, x, y;  // orientamento, colonna e riga dell'angolo in alto a sinistra
};

// kind = indice colore 0..6 (assegnato da colorize); classify() dice la forma classica I,O,T,S,Z,J,L
struct Piece {
  uint8_t n;
  uint8_t kind;
  uint8_t w[4], h[4];
  Cell o[4][4];  // o[j][i]: cella i nell'orientamento j (normalizzata a 0,0)
  int8_t fx, fy;  // posizione finale (angolo in alto a sinistra)
  int16_t start;  // tick di comparsa dentro la fase di costruzione
  std::vector<Step> path;
  Piece() : n(0), kind(7), fx(0), fy(0), start(0) {
    memset(w, 0, sizeof(w));
    memset(h, 0, sizeof(h));
    memset(o, 0, sizeof(o));
  }
};

struct Raw {
  Cell c[4];
  uint8_t n;
  int maxY;
};

static inline int fdiv2(int v) { return v >= 0 ? v / 2 : -((1 - v) / 2); }

static inline void rotateCw(const Cell *in, int n, int h, Cell *out) {
  for (int i = 0; i < n; i++) out[i] = mk(h - 1 - in[i].y, in[i].x);
}

static inline uint16_t maskOf(const Cell *c, int n) {
  uint16_t m = 0;
  for (int i = 0; i < n; i++) m |= (uint16_t)(1u << (c[i].y * 4 + c[i].x));
  return m;
}

static inline int classify(const Cell *c, int n) {
  if (n != 4) return 7;
  static const int8_t T[7][8] = {
      {0, 0, 1, 0, 2, 0, 3, 0},  // I
      {0, 0, 1, 0, 0, 1, 1, 1},  // O
      {1, 0, 0, 1, 1, 1, 2, 1},  // T
      {1, 0, 2, 0, 0, 1, 1, 1},  // S
      {0, 0, 1, 0, 1, 1, 2, 1},  // Z
      {0, 0, 0, 1, 1, 1, 2, 1},  // J
      {2, 0, 0, 1, 1, 1, 2, 1},  // L
  };
  uint16_t target = maskOf(c, 4);
  for (int k = 0; k < 7; k++) {
    Cell a[4], b[4];
    int w = 0, h = 0;
    for (int i = 0; i < 4; i++) {
      a[i] = mk(T[k][i * 2], T[k][i * 2 + 1]);
      if (a[i].x + 1 > w) w = a[i].x + 1;
      if (a[i].y + 1 > h) h = a[i].y + 1;
    }
    for (int r = 0; r < 4; r++) {
      if (maskOf(a, 4) == target) return k;
      rotateCw(a, 4, h, b);
      memcpy(a, b, sizeof(a));
      int t = w;
      w = h;
      h = t;
    }
  }
  return 7;
}

static inline Piece makePiece(const Raw &R) {
  Piece p;
  p.n = R.n;
  int minx = 99, miny = 99, maxx = -1, maxy = -1;
  for (int i = 0; i < R.n; i++) {
    minx = std::min(minx, (int)R.c[i].x);
    miny = std::min(miny, (int)R.c[i].y);
    maxx = std::max(maxx, (int)R.c[i].x);
    maxy = std::max(maxy, (int)R.c[i].y);
  }
  p.fx = (int8_t)minx;
  p.fy = (int8_t)miny;
  p.w[0] = (uint8_t)(maxx - minx + 1);
  p.h[0] = (uint8_t)(maxy - miny + 1);
  for (int i = 0; i < R.n; i++) p.o[0][i] = mk(R.c[i].x - minx, R.c[i].y - miny);
  for (int j = 1; j < 4; j++) {
    rotateCw(p.o[j - 1], R.n, p.h[j - 1], p.o[j]);
    p.w[j] = p.h[j - 1];
    p.h[j] = p.w[j - 1];
  }
  p.kind = (uint8_t)classify(p.o[0], R.n);
  return p;
}

static inline bool hit(const Piece &p, int j, int px, int py, const bool occ[GH][GW]) {
  for (int i = 0; i < p.n; i++) {
    int x = px + p.o[j][i].x, y = py + p.o[j][i].y;
    if (x < 0 || x >= GW || y >= GH) return true;
    if (y >= 0 && occ[y][x]) return true;
  }
  return false;
}

// Calcola il percorso del pezzo fino alla posizione finale.
// straight=false: parte ruotato e/o spostato e fa una azione per tick mentre cade.
// straight=true : cade dritto (percorso sempre valido se l'ordine e' corretto).
static inline bool simulate(Piece &p, Rng &rng, const bool occ[GH][GW], bool straight) {
  p.path.clear();
  int j = 0, px = p.fx, py = SPAWN_TOP;
  if (!straight) {
    j = rng.below(4);
    px = rng.below(GW - p.w[j] + 1);
  }
  for (int iter = 0; iter < 48; iter++) {
    if (hit(p, j, px, py, occ)) return false;
    Step s;
    s.j = (int8_t)j;
    s.x = (int8_t)px;
    s.y = (int8_t)py;
    p.path.push_back(s);
    if (j == 0 && px == p.fx && py == p.fy) return true;
    if (py > p.fy) return false;
    if (!straight) {
      if (j != 0) {
        int nj = (j + 1) & 3;
        int npx = px + fdiv2((int)p.w[j] - (int)p.w[nj]);
        int npy = py + fdiv2((int)p.h[j] - (int)p.h[nj]);
        if (!hit(p, nj, npx, npy, occ)) {
          j = nj;
          px = npx;
          py = npy;
        }
      } else if (px != p.fx) {
        int d = px < p.fx ? 1 : -1;
        if (!hit(p, j, px + d, py, occ)) px += d;
      }
      if (j == 0 && px == p.fx && py == p.fy) continue;  // arrivato: niente altra caduta
    }
    if (py < p.fy) {
      if (hit(p, j, px, py + 1, occ)) return false;
      py++;
    }
  }
  return false;
}

static inline int freeNeighbours(const bool g[GH][GW], const int8_t id[GH][GW], int x, int y) {
  static const int dx[4] = {1, -1, 0, 0}, dy[4] = {0, 0, 1, -1};
  int d = 0;
  for (int k = 0; k < 4; k++) {
    int nx = x + dx[k], ny = y + dy[k];
    if (nx >= 0 && nx < GW && ny >= 0 && ny < GH && g[ny][nx] && id[ny][nx] < 0) d++;
  }
  return d;
}

// Divide la cifra in pezzi connessi da al massimo 4 celle (a caso, con qualche euristica)
static inline int tileOnce(const bool g[GH][GW], Rng &rng, Raw *raws) {
  static const int dx[4] = {1, -1, 0, 0}, dy[4] = {0, 0, 1, -1};
  int8_t id[GH][GW];
  memset(id, -1, sizeof(id));
  int left = 0;
  for (int y = 0; y < GH; y++)
    for (int x = 0; x < GW; x++)
      if (g[y][x]) left++;
  int np = 0;
  while (left > 0 && np < MAXP) {
    int bestDeg = 99, cnt = 0, sx = 0, sy = 0;
    for (int y = 0; y < GH; y++)
      for (int x = 0; x < GW; x++) {
        if (!g[y][x] || id[y][x] >= 0) continue;
        int d = freeNeighbours(g, id, x, y);
        if (d < bestDeg) {
          bestDeg = d;
          cnt = 1;
          sx = x;
          sy = y;
        } else if (d == bestDeg) {
          cnt++;
          if (rng.below(cnt) == 0) {
            sx = x;
            sy = y;
          }
        }
      }
    Raw &R = raws[np];
    R.n = 0;
    R.c[R.n++] = mk(sx, sy);
    id[sy][sx] = (int8_t)np;
    left--;
    while (R.n < 4) {
      Cell cand[16];
      int nc = 0;
      for (int i = 0; i < R.n; i++)
        for (int k = 0; k < 4; k++) {
          int nx = R.c[i].x + dx[k], ny = R.c[i].y + dy[k];
          if (nx < 0 || nx >= GW || ny < 0 || ny >= GH || !g[ny][nx] || id[ny][nx] >= 0) continue;
          bool dup = false;
          for (int q = 0; q < nc; q++)
            if (cand[q].x == nx && cand[q].y == ny) dup = true;
          if (!dup && nc < 16) cand[nc++] = mk(nx, ny);
        }
      if (nc == 0) break;
      int pick = 0;
      if (rng.below(2) == 0) {
        pick = rng.below(nc);
      } else {
        int bd = 99, bc = 0;
        for (int q = 0; q < nc; q++) {
          int d = freeNeighbours(g, id, cand[q].x, cand[q].y);
          if (d < bd) {
            bd = d;
            bc = 1;
            pick = q;
          } else if (d == bd) {
            bc++;
            if (rng.below(bc) == 0) pick = q;
          }
        }
      }
      Cell c = cand[pick];
      R.c[R.n++] = c;
      id[c.y][c.x] = (int8_t)np;
      left--;
    }
    int my = 0;
    for (int i = 0; i < R.n; i++) my = std::max(my, (int)R.c[i].y);
    R.maxY = my;
    np++;
  }
  return np;
}

// Ordine di posa: un pezzo va posato prima di quelli che stanno sopra di lui nella stessa colonna.
static inline bool orderRaws(const Raw *r, int np, Rng &rng, int *order) {
  static bool before[MAXP][MAXP];
  int indeg[MAXP];
  memset(before, 0, sizeof(before));
  memset(indeg, 0, sizeof(indeg));
  for (int p = 0; p < np; p++)
    for (int q = 0; q < np; q++) {
      if (p == q) continue;
      bool e = false;
      for (int a = 0; a < r[p].n && !e; a++)
        for (int b = 0; b < r[q].n; b++)
          if (r[p].c[a].x == r[q].c[b].x && r[q].c[b].y < r[p].c[a].y) {
            e = true;
            break;
          }
      if (e) {
        before[p][q] = true;
        indeg[q]++;
      }
    }
  bool placed[MAXP];
  memset(placed, 0, sizeof(placed));
  for (int k = 0; k < np; k++) {
    int best = -1, bestY = -1, cnt = 0;
    for (int p = 0; p < np; p++) {
      if (placed[p] || indeg[p] != 0) continue;
      if (r[p].maxY > bestY) {
        bestY = r[p].maxY;
        best = p;
        cnt = 1;
      } else if (r[p].maxY == bestY) {
        cnt++;
        if (rng.below(cnt) == 0) best = p;
      }
    }
    if (best < 0) return false;
    order[k] = best;
    placed[best] = true;
    for (int q = 0; q < np; q++)
      if (before[best][q]) indeg[q]--;
  }
  return true;
}

static inline bool buildPieces(const Raw *r, const int *order, int np, Rng &rng, std::vector<Piece> &out) {
  bool occ[GH][GW];
  memset(occ, 0, sizeof(occ));
  out.clear();
  int start = 0;
  for (int k = 0; k < np; k++) {
    Piece p = makePiece(r[order[k]]);
    bool ok = false;
    for (int a = 0; a < 10 && !ok; a++) ok = simulate(p, rng, occ, false);
    if (!ok) ok = simulate(p, rng, occ, true);
    if (!ok) return false;
    for (int i = 0; i < p.n; i++) occ[p.fy + p.o[0][i].y][p.fx + p.o[0][i].x] = true;
    p.start = (int16_t)start;
    start += (int)p.path.size();
    out.push_back(p);
  }
  return true;
}

// Colori vivi: ogni pezzo della cifra ha un colore diverso dagli altri (7 colori, fino a 7 pezzi)
static inline void colorize(std::vector<Piece> &v, Rng &rng) {
  int off = rng.below(7);
  for (size_t k = 0; k < v.size(); k++) v[k].kind = (uint8_t)((k + off) % 7);
}

static inline bool buildDigit(int digit, Rng &rng, std::vector<Piece> &out) {
  out.clear();
  if (digit < 0 || digit > 9) return true;
  bool g[GH][GW];
  for (int y = 0; y < GH; y++)
    for (int x = 0; x < GW; x++) g[y][x] = FONT[digit][y][x] == 'X';

  Raw best[MAXP];
  int bestOrder[MAXP], bestN = 0;
  int bestScore = 1 << 30;
  for (int attempt = 0; attempt < 48; attempt++) {
    Raw raws[MAXP];
    int order[MAXP];
    int np = tileOnce(g, rng, raws);
    if (!orderRaws(raws, np, rng, order)) continue;
    int small = 0;
    for (int i = 0; i < np; i++)
      if (raws[i].n < 4) small++;
    int score = np + 6 * small;
    if (score < bestScore) {
      bestScore = score;
      bestN = np;
      memcpy(best, raws, sizeof(Raw) * np);
      memcpy(bestOrder, order, sizeof(int) * np);
    }
  }
  if (bestScore < (1 << 30) && buildPieces(best, bestOrder, bestN, rng, out)) {
    colorize(out, rng);
    return true;
  }

  // Ripiego sicuro: un pezzo per cella, dal basso verso l'alto
  Raw raws[MAXP];
  int order[MAXP], np = 0;
  for (int y = GH - 1; y >= 0; y--)
    for (int x = 0; x < GW; x++)
      if (g[y][x] && np < MAXP) {
        raws[np].n = 1;
        raws[np].c[0] = mk(x, y);
        raws[np].maxY = y;
        order[np] = np;
        np++;
      }
  bool ok = buildPieces(raws, order, np, rng, out);
  if (ok) colorize(out, rng);
  return ok;
}

class Engine {
 public:
  static const int SLOTS = 4;
  struct Slot {
    int digit;
    std::vector<Piece> cur, old;
    uint32_t t0;
    int outLen, inLen;
    bool anim;
    Slot() : digit(-2), t0(0), outLen(0), inLen(0), anim(false) {}
  };
  Slot slot[SLOTS];
  Rng rng;

  void seed(uint32_t s) { rng = Rng(s); }

  void reset() {
    for (int i = 0; i < SLOTS; i++) slot[i] = Slot();
  }

  bool slotBusy(int i, uint32_t now) const {
    const Slot &s = slot[i];
    return s.anim && (int32_t)(now - (s.t0 + (uint32_t)(s.outLen + s.inLen))) < 0;
  }

  bool animating(uint32_t now) const {
    for (int i = 0; i < SLOTS; i++)
      if (slotBusy(i, now)) return true;
    return false;
  }

  // value: 0..9 oppure -1 per cifra vuota. delay: tick di attesa prima di iniziare.
  void setDigit(int i, int value, uint32_t now, int delay) {
    Slot &s = slot[i];
    if (value == s.digit) return;
    bool busy = slotBusy(i, now);
    std::vector<Piece> fresh;
    buildDigit(value, rng, fresh);
    s.old.clear();
    if (!busy)
      s.old.swap(s.cur);
    else
      s.cur.clear();
    s.cur.swap(fresh);
    s.outLen = 0;
    for (size_t k = 0; k < s.old.size(); k++) {
      int rows = EXIT_ROW - s.old[k].fy;
      if (rows < 1) rows = 1;
      int need = (int)k + (rows + 1) / 2;
      if (need > s.outLen) s.outLen = need;
    }
    s.inLen = 0;
    if (!s.cur.empty()) s.inLen = s.cur.back().start + (int)s.cur.back().path.size();
    s.t0 = now + (uint32_t)delay;
    s.anim = true;
    s.digit = value;
  }

  // cell(xPixel, yPixel, kind): disegna una cella 4x4 con l'angolo in alto a sinistra in (x,y)
  template <class F>
  void render(uint32_t now, const int *xs, F &&cell) const {
    for (int i = 0; i < SLOTS; i++) {
      const Slot &s = slot[i];
      int x0 = xs[i];
      int rel = 0;
      bool animating = s.anim && (int32_t)(now - (s.t0 + (uint32_t)(s.outLen + s.inLen))) < 0;
      if (animating) rel = (int)(int32_t)(now - s.t0);
      if (!animating) {
        drawFinal(s.cur, x0, 0, cell);
      } else if (rel < 0) {
        drawFinal(s.old, x0, 0, cell);
      } else if (rel < s.outLen) {
        for (size_t k = 0; k < s.old.size(); k++) {
          int off = rel < (int)k ? 0 : 2 * (rel - (int)k);
          drawPiece(s.old[k], 0, s.old[k].fx, s.old[k].fy + off, x0, cell);
        }
      } else {
        int t = rel - s.outLen;
        for (size_t k = 0; k < s.cur.size(); k++) {
          const Piece &p = s.cur[k];
          int st = t - p.start;
          if (st < 0) continue;
          if (st >= (int)p.path.size()) st = (int)p.path.size() - 1;
          const Step &S = p.path[st];
          drawPiece(p, S.j, S.x, S.y, x0, cell);
        }
      }
    }
  }

 private:
  template <class F>
  static void drawPiece(const Piece &p, int j, int gx, int gy, int x0, F &cell) {
    for (int i = 0; i < p.n; i++)
      cell(x0 + CELL * (gx + p.o[j][i].x), GLYPH_Y + CELL * (gy + p.o[j][i].y), (int)p.kind);
  }
  template <class F>
  static void drawFinal(const std::vector<Piece> &v, int x0, int off, F &cell) {
    for (size_t k = 0; k < v.size(); k++) drawPiece(v[k], 0, v[k].fx, v[k].fy + off, x0, cell);
  }
};

}  // namespace tet
