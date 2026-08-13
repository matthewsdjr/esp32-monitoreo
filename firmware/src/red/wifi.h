// ============================================================================
// Conexión WiFi y aprovisionamiento
// ============================================================================
// Portal cautivo al primer arranque: el equipo levanta su propio punto de acceso
// y sirve un formulario donde se cargan la red y las credenciales del backend.
// Todo queda en NVS, y NADA de eso toca el repositorio, que es público.
//
// No se usa WiFiManager: se necesita capturar cinco campos propios (URL, anon
// key, slug, token y JWT), y el formulario propio es más claro para quien lo
// llena en planta desde un celular que una lista de parámetros genéricos.
// ============================================================================

#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include "credenciales.h"

namespace red {

class GestorWiFi {
 public:
  void iniciar(AlmacenCredenciales* almacen) {
    almacen_ = almacen;
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    // El ahorro de energía introduce latencias de cientos de ms en la respuesta
    // y provoca timeouts esporádicos en TLS. En un equipo alimentado de la red
    // no aporta nada.
    WiFi.setSleep(false);
  }

  /** Intenta conectar con lo que haya en NVS. No bloquea más de `msMax`. */
  bool conectar(uint32_t msMax = 20000) {
    String ssid = prefsRed().getString("ssid", "");
    String pass = prefsRed().getString("pass", "");
    if (ssid.isEmpty()) return false;

    WiFi.begin(ssid.c_str(), pass.c_str());
    const uint32_t inicio = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - inicio < msMax) {
      delay(200);
    }
    if (WiFi.status() == WL_CONNECTED) {
      reconexiones_++;
      return true;
    }
    return false;
  }

  bool conectado() const { return WiFi.status() == WL_CONNECTED; }
  uint32_t reconexiones() const { return reconexiones_; }
  int32_t rssi() const { return WiFi.RSSI(); }

  /**
   * Mantiene la conexión. Backoff exponencial con tope de 60 s: reintentar cada
   * segundo contra un router apagado consume energía y llena el registro sin
   * ninguna posibilidad de éxito.
   *
   * Tras 15 min sin red, reinicio controlado. Es la salida ante estados del
   * stack de WiFi de los que no se puede salir por software.
   */
  void mantener() {
    if (conectado()) {
      sinRedDesdeMs_ = 0;
      esperaMs_ = 1000;
      return;
    }

    if (sinRedDesdeMs_ == 0) sinRedDesdeMs_ = millis();

    if (millis() - sinRedDesdeMs_ > 15UL * 60UL * 1000UL) {
      Serial.println("[wifi] 15 min sin red: reinicio controlado");
      delay(100);
      ESP.restart();
    }

    if (millis() - ultimoIntentoMs_ < esperaMs_) return;
    ultimoIntentoMs_ = millis();

    Serial.printf("[wifi] reintentando (espera %u ms)\n", (unsigned)esperaMs_);
    WiFi.disconnect();
    delay(50);
    conectar(8000);

    esperaMs_ = esperaMs_ * 2;
    if (esperaMs_ > 60000) esperaMs_ = 60000;
  }

  // -------------------------------------------------------------------------
  // Portal cautivo
  // -------------------------------------------------------------------------
  /**
   * Levanta el punto de acceso de configuración y atiende hasta que se guarden
   * las credenciales. Bloquea a propósito: sin configurar, el equipo no tiene
   * nada útil que hacer.
   */
  void portalCautivo() {
    const String ap = String("Monitoreo-") + String((uint32_t)ESP.getEfuseMac(), HEX);

    WiFi.mode(WIFI_AP);
    WiFi.softAP(ap.c_str());
    delay(200);

    DNSServer dns;
    dns.start(53, "*", WiFi.softAPIP());   // redirige todo: eso lo hace "cautivo"

    WebServer srv(80);
    bool guardado = false;

    srv.on("/", [&]() { srv.send(200, "text/html; charset=utf-8", paginaPortal()); });

    srv.on("/guardar", HTTP_POST, [&]() {
      prefsRed().putString("ssid", srv.arg("ssid"));
      prefsRed().putString("pass", srv.arg("pass"));

      Credenciales c;
      c.urlSupabase    = srv.arg("url");
      c.anonKey        = srv.arg("anon");
      c.slugEquipo     = srv.arg("slug");
      c.tokenIngesta   = srv.arg("token");
      c.jwtDispositivo = srv.arg("jwt");
      almacen_->guardar(c);

      srv.send(200, "text/html; charset=utf-8",
               "<meta charset='utf-8'><body style='font-family:system-ui;padding:2rem'>"
               "<h2>Guardado</h2><p>El equipo se reiniciará y se conectará.</p></body>");
      guardado = true;
    });

    srv.onNotFound([&]() {   // cualquier URL abre el formulario
      srv.send(200, "text/html; charset=utf-8", paginaPortal());
    });

    srv.begin();
    Serial.printf("\n[portal] Conéctate a la red WiFi \"%s\" y abre http://%s\n\n",
                  ap.c_str(), WiFi.softAPIP().toString().c_str());

    while (!guardado) {
      dns.processNextRequest();
      srv.handleClient();
      delay(5);
    }

    delay(1500);
    ESP.restart();
  }

  /** Borra red y credenciales. Se usa para reasignar el equipo a otra planta. */
  void olvidar() {
    prefsRed().remove("ssid");
    prefsRed().remove("pass");
    almacen_->borrar();
  }

 private:
  Preferences& prefsRed() {
    if (!abierto_) { prefs_.begin("wifi", false); abierto_ = true; }
    return prefs_;
  }

  static String paginaPortal() {
    // Sin recursos externos: el celular está conectado al AP del equipo y no
    // tiene salida a internet, así que cualquier CSS o fuente remota no cargaría.
    return F(
      "<!doctype html><meta charset='utf-8'>"
      "<meta name='viewport' content='width=device-width,initial-scale=1'>"
      "<title>Configurar equipo</title>"
      "<style>body{font-family:system-ui;max-width:34rem;margin:0 auto;padding:1.5rem;"
      "background:#f9f9f7;color:#0b0b0b}h2{margin:0 0 .3rem}"
      "p.n{color:#52514e;font-size:.85rem;margin:.2rem 0 1.2rem}"
      "label{display:block;font-size:.8rem;color:#52514e;margin:.9rem 0 .2rem}"
      "input{width:100%;padding:.6rem;border:1px solid #c3c2b7;border-radius:.4rem;"
      "font-size:1rem;box-sizing:border-box}"
      "button{margin-top:1.5rem;width:100%;padding:.8rem;background:#2a78d6;color:#fff;"
      "border:0;border-radius:.4rem;font-size:1rem;font-weight:600}"
      "small{color:#898781;display:block;margin-top:.2rem;font-size:.75rem}</style>"
      "<h2>Configurar equipo</h2>"
      "<p class='n'>Estos datos se guardan cifrados en el equipo y nunca salen de él.</p>"
      "<form method='POST' action='/guardar'>"
      "<label>Red WiFi</label><input name='ssid' required>"
      "<label>Contraseña WiFi</label><input name='pass' type='password'>"
      "<label>URL de Supabase</label>"
      "<input name='url' placeholder='https://xxxx.supabase.co' required>"
      "<label>Clave anónima</label><input name='anon'>"
      "<small>Necesaria solo para el canal en vivo.</small>"
      "<label>Identificador del equipo</label>"
      "<input name='slug' value='planta-01' required>"
      "<label>Token de ingesta</label><input name='token' required>"
      "<small>Lo entrega registrar-equipo.mjs. Se muestra una sola vez.</small>"
      "<label>JWT del equipo</label><input name='jwt'>"
      "<small>Lo entrega emitir-jwt.mjs. Sin él funciona el histórico, "
      "pero no el tiempo real.</small>"
      "<button type='submit'>Guardar y conectar</button></form>");
  }

  AlmacenCredenciales* almacen_ = nullptr;
  Preferences prefs_;
  bool abierto_ = false;
  uint32_t reconexiones_ = 0;
  uint32_t sinRedDesdeMs_ = 0;
  uint32_t ultimoIntentoMs_ = 0;
  uint32_t esperaMs_ = 1000;
};

}  // namespace red
