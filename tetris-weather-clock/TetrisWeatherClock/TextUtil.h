// Utilita' di testo senza dipendenze.
// Il font del pannello e' solo ASCII: toglie gli accenti dai nomi di citta' (UTF-8 -> ASCII).
#pragma once
#include <stddef.h>
#include <stdint.h>

static const char ASCII_L1[] = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";  // U+00C0..U+00FF
static const char ASCII_LA[] =
    "AaAaAaCcCcCcCcDd"
    "DdEeEeEeEeEeGgGg"
    "GgGgHhHhIiIiIiIi"
    "IiIiJjKkkLlLlLlL"
    "lLlNnNnNnnNnOoOo"
    "OoOoRrRrRrSsSsSs"
    "SsTtTtTtUuUuUuUu"
    "UuUuWwYyYZzZzZzs";  // U+0100..U+017F

inline void asciiFold(const char *in, char *out, size_t outSize) {
  if (outSize == 0) return;
  size_t o = 0;
  const unsigned char *p = (const unsigned char *)in;
  while (*p && o + 1 < outSize) {
    uint32_t cp = *p;
    int extra = 0;
    if (cp < 0x80) {
      extra = 0;
    } else if ((cp & 0xE0) == 0xC0) {
      cp &= 0x1F;
      extra = 1;
    } else if ((cp & 0xF0) == 0xE0) {
      cp &= 0x0F;
      extra = 2;
    } else if ((cp & 0xF8) == 0xF0) {
      cp &= 0x07;
      extra = 3;
    } else {
      p++;
      continue;
    }
    p++;
    for (int i = 0; i < extra && (*p & 0xC0) == 0x80; i++, p++) cp = (cp << 6) | (*p & 0x3F);
    char c;
    if (cp < 0x20) continue;
    if (cp < 0x7F)
      c = (char)cp;
    else if (cp == 0xA0)
      c = ' ';
    else if (cp >= 0xC0 && cp <= 0xFF)
      c = ASCII_L1[cp - 0xC0];
    else if (cp >= 0x100 && cp <= 0x17F)
      c = ASCII_LA[cp - 0x100];
    else if (cp == 0x2018 || cp == 0x2019)
      c = '\'';
    else if (cp == 0x2013 || cp == 0x2014)
      c = '-';
    else
      c = '?';
    out[o++] = c;
  }
  out[o] = 0;
}
