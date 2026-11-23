// Server HTTP leggero basato su esp_http_server (ESP-IDF).
// Stessa interfaccia minima di WebServer (on, arg, hasArg, send), ma gestisce piu' connessioni
// insieme: una connessione inattiva del browser non blocca piu' le altre richieste.
#pragma once
#include <Arduino.h>
#include <esp_http_server.h>
#include <lwip/sockets.h>
#include <functional>
#include <vector>

class FastServer {
 public:
  typedef std::function<void()> Handler;

  void on(const char *uri, int m, Handler h) { routes.push_back({uri, m, h}); }
  void onNotFound(Handler h) { notFound = h; }

  void begin() {
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    cfg.stack_size = 10240;
    cfg.core_id = 0;
    cfg.max_open_sockets = 6;
    cfg.lru_purge_enable = true;  // chiude da sola le connessioni vecchie
    cfg.recv_wait_timeout = 4;
    cfg.send_wait_timeout = 4;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    // risposte subito, senza aspettare l'ACK ritardato del browser (Nagle): meno latenza sui comandi
    cfg.open_fn = [](httpd_handle_t, int fd) -> esp_err_t {
      int one = 1;
      setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
      return ESP_OK;
    };
    cfg.max_uri_handlers = 4;
    if (httpd_start(&h, &cfg) != ESP_OK) return;
    httpd_uri_t g = {"/*", HTTP_GET, dispatch, this, false, false, nullptr};
    httpd_uri_t p = {"/*", HTTP_POST, dispatch, this, false, false, nullptr};
    httpd_register_uri_handler(h, &g);
    httpd_register_uri_handler(h, &p);
  }

  bool hasArg(const char *name) const { return find(name, nullptr); }
  String arg(const char *name) const {
    String v;
    find(name, &v);
    return v;
  }

  void send(int code, const char *type, const String &body) {
    if (!req) return;
    httpd_resp_set_status(req, statusText(code));
    httpd_resp_set_type(req, type);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, body.c_str(), body.length());
  }
  void send(int code, const char *type, const char *body) { send(code, type, String(body)); }

  void sendGz(const uint8_t *data, unsigned len) {
    if (!req) return;
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_send(req, (const char *)data, len);
  }

 private:
  struct Route {
    const char *uri;
    int method;
    Handler fn;
  };
  std::vector<Route> routes;
  Handler notFound;
  httpd_handle_t h = nullptr;
  httpd_req_t *req = nullptr;
  String params;  // "a=1&b=2" (query + corpo)

  static const char *statusText(int c) {
    switch (c) {
      case 200: return "200 OK";
      case 400: return "400 Bad Request";
      case 404: return "404 Not Found";
      case 409: return "409 Conflict";
      default: return "500 Internal Server Error";
    }
  }

  static int hexv(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  }
  static String decode(const String &in) {
    String o;
    o.reserve(in.length());
    for (unsigned i = 0; i < in.length(); i++) {
      char c = in[i];
      if (c == '+') o += ' ';
      else if (c == '%' && i + 2 < in.length() + 0 && hexv(in[i + 1]) >= 0 && hexv(in[i + 2]) >= 0) {
        o += (char)(hexv(in[i + 1]) * 16 + hexv(in[i + 2]));
        i += 2;
      } else o += c;
    }
    return o;
  }
  bool find(const char *name, String *out) const {
    unsigned n = strlen(name), i = 0, L = params.length();
    while (i < L) {
      int amp = params.indexOf('&', i);
      unsigned end = amp < 0 ? L : (unsigned)amp;
      int eq = params.indexOf('=', i);
      unsigned ke = (eq < 0 || (unsigned)eq > end) ? end : (unsigned)eq;
      if (ke - i == n && strncmp(params.c_str() + i, name, n) == 0) {
        if (out) *out = (ke < end) ? decode(params.substring(ke + 1, end)) : String("");
        return true;
      }
      i = end + 1;
    }
    return false;
  }

  static esp_err_t dispatch(httpd_req_t *r) {
    FastServer *s = (FastServer *)r->user_ctx;
    s->req = r;
    s->params = "";
    // percorso senza query
    String path = r->uri;
    int q = path.indexOf('?');
    if (q >= 0) {
      s->params = path.substring(q + 1);
      path = path.substring(0, q);
    }
    // corpo (POST, form urlencoded), massimo 2 KB
    if (r->method == HTTP_POST && r->content_len > 0) {
      size_t n = r->content_len > 2048 ? 2048 : r->content_len;
      char *buf = (char *)malloc(n + 1);
      if (buf) {
        size_t got = 0;
        while (got < n) {
          int k = httpd_req_recv(r, buf + got, n - got);
          if (k <= 0) {
            if (k == HTTPD_SOCK_ERR_TIMEOUT) continue;
            break;
          }
          got += k;
        }
        buf[got] = 0;
        if (s->params.length()) s->params += '&';
        s->params += buf;
        free(buf);
      }
    }
    for (auto &rt : s->routes)
      if (path == rt.uri && rt.method == (int)r->method) {
        rt.fn();
        s->req = nullptr;
        return ESP_OK;
      }
    if (s->notFound) s->notFound();
    else s->send(404, "text/plain", "Non trovato");
    s->req = nullptr;
    return ESP_OK;
  }
};
