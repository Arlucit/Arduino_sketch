#include <cstdio>
#include <cstring>
#include <set>
#include "TetrisEngine.h"  // test nativo: g++ -std=c++11 -Isrc tools/engine_test.cpp -o /tmp/engine_test && /tmp/engine_test
#include "TextUtil.h"
using namespace tet;

static_assert(sizeof(ASCII_L1) - 1 == 64, "L1 len");
static_assert(sizeof(ASCII_LA) - 1 == 128, "LA len");

int main() {
  // --- TextUtil ---
  char b[64];
  const char *tests[][2] = {{"Forl\xC3\xAC", "Forli"}, {"\xC5\x81\xC3\xB3" "d\xC5\xBA", "Lodz"},
                            {"S\xC3\xA3o Paulo", "Sao Paulo"}, {"Z\xC3\xBCrich", "Zurich"},
                            {"Reykjav\xC3\xADk", "Reykjavik"}, {"Plain", "Plain"}, {"\xE4\xB8\xAD", "?"}};
  int bad = 0;
  for (auto &t : tests) {
    asciiFold(t[0], b, sizeof(b));
    if (strcmp(b, t[1])) { printf("FOLD FAIL %s -> %s\n", t[1], b); bad++; }
  }
  asciiFold("Abcdefghij", b, 5);
  if (strcmp(b, "Abcd")) { printf("trunc fail %s\n", b); bad++; }
  printf("fold tests: %s\n", bad ? "FAIL" : "ok");

  // --- Engine invariants ---
  long total = 0, tetro = 0, pieces = 0, nonStraight = 0, ticksSum = 0, maxTicks = 0, digitsN = 0;
  int errors = 0;
  int kindCount[8] = {0};
  for (uint32_t seed = 1; seed <= 3000; seed++) {
    Rng rng(seed * 2654435761u);
    for (int d = 0; d < 10; d++) {
      std::vector<Piece> v;
      if (!buildDigit(d, rng, v)) { printf("build fail d=%d\n", d); errors++; continue; }
      bool g[GH][GW];
      int cells = 0;
      for (int y = 0; y < GH; y++) for (int x = 0; x < GW; x++) { g[y][x] = FONT[d][y][x] == 'X'; cells += g[y][x]; }
      bool cover[GH][GW]; memset(cover, 0, sizeof(cover));
      bool occ[GH][GW]; memset(occ, 0, sizeof(occ));
      int last = 0;
      for (size_t k = 0; k < v.size(); k++) {
        const Piece &p = v[k];
        pieces++; kindCount[p.kind]++;
        if (p.kind > 6) { errors++; printf("kind range\n"); }
        for (size_t z = 0; z < k; z++) if (v[z].kind == p.kind) { errors++; printf("colore ripetuto d=%d\n", d); }
        if (p.n == 4) tetro++;
        total++;
        // celle finali = glifo, senza sovrapposizioni
        for (int i = 0; i < p.n; i++) {
          int x = p.fx + p.o[0][i].x, y = p.fy + p.o[0][i].y;
          if (x < 0 || x >= GW || y < 0 || y >= GH || !g[y][x] || cover[y][x]) { errors++; printf("cover err d=%d\n", d); }
          cover[y][x] = true;
        }
        // percorso: nessuna collisione, ultimo stato = finale, y mai decrescente (tranne rotazioni che ricentrano)
        const auto &P = p.path;
        if (P.empty()) { errors++; continue; }
        const Step &L = P.back();
        if (L.j != 0 || L.x != p.fx || L.y != p.fy) { errors++; printf("end err\n"); }
        bool straight = true;
        for (size_t s = 0; s < P.size(); s++) {
          if (hit(p, P[s].j, P[s].x, P[s].y, occ)) { errors++; printf("collision d=%d k=%zu s=%zu\n", d, k, s); }
          if (P[s].j != 0 || P[s].x != p.fx) straight = false;
          if (s) {
            if (P[s].y < P[s-1].y && (P[s].j == P[s-1].j || P[s-1].y - P[s].y > 1)) { errors++; if (errors < 20) printf("up move\n"); }
            if (abs(P[s].x - P[s-1].x) > 2) { errors++; printf("big shift\n"); }
            if (P[s].j != P[s-1].j && P[s].j != ((P[s-1].j + 1) & 3)) { errors++; printf("bad rot\n"); }
          }
        }
        if (!straight) nonStraight++;
        // la posizione di comparsa deve essere in vista: riga >= -1
        if (P[0].y != SPAWN_TOP) { errors++; printf("spawn row\n"); }
        for (int i = 0; i < p.n; i++) occ[p.fy + p.o[0][i].y][p.fx + p.o[0][i].x] = true;
        if (p.start != last) { errors++; printf("start err\n"); }
        last += (int)P.size();
      }
      int cc = 0; for (int y = 0; y < GH; y++) for (int x = 0; x < GW; x++) cc += cover[y][x];
      if (cc != cells) { errors++; printf("cell count %d vs %d\n", cc, cells); }
      ticksSum += last; if (last > maxTicks) maxTicks = last; digitsN++;
    }
  }
  printf("digits=%ld pieces=%ld tetromino=%.1f%% animated(non-straight)=%.1f%% avgTicks/digit=%.1f maxTicks=%ld errors=%d\n",
         digitsN, pieces, 100.0 * tetro / pieces, 100.0 * nonStraight / pieces, (double)ticksSum / digitsN, maxTicks, errors);
  printf("colori 0..7: "); for (int k = 0; k < 8; k++) printf("%d ", kindCount[k]); printf("\n");

  // --- Engine runtime: cambia cifra, OUT poi IN, controlla che alla fine resti la cifra giusta ---
  Engine e; e.seed(7);
  int xs[4] = {3, 16, 37, 50};
  for (int i = 0; i < 4; i++) e.setDigit(i, i + 1, 0, i * 4);
  uint32_t t = 0;
  while (e.animating(t) && t < 1000) t++;
  printf("intro finished at tick %u\n", t);
  e.setDigit(3, 8, t, 0);
  uint32_t t1 = t; while (e.animating(t) && t < t1 + 1000) t++;
  printf("change finished after %u ticks\n", t - t1);
  std::set<std::pair<int,int>> got;
  e.render(t, xs, [&](int x, int y, int) { got.insert({x, y}); });
  std::set<std::pair<int,int>> want;
  int digs[4] = {1,2,3,8};
  for (int i = 0; i < 4; i++) for (int y = 0; y < GH; y++) for (int x = 0; x < GW; x++) if (FONT[digs[i]][y][x]=='X') want.insert({xs[i]+x*CELL, GLYPH_Y+y*CELL});
  printf("final render matches glyphs: %s (%zu cells)\n", got == want ? "yes" : "NO", got.size());
  // out-of-panel check during OUT phase and visible cells during IN
  e.setDigit(0, 5, t, 0);
  int maxVisible = 0;
  for (uint32_t u = t; e.animating(u) && u < t + 500; u++) {
    int n = 0; e.render(u, xs, [&](int, int y, int) { if (y < 32 && y + 2 >= 0) n++; });
    if (n > maxVisible) maxVisible = n;
  }
  printf("max visible cells during anim: %d\n", maxVisible);
  return errors || bad;
}
