// ============================================================================
// Publicador — las dos rutas del contrato
// ============================================================================
//   Ruta A  broadcast cada 2 s   -> tiempo real, sin tocar la base de datos
//   Ruta B  lote cada 30 s       -> histórico, alarmas y comandos
//
// UNA SOLA CONEXIÓN TLS PARA AMBAS, y esto no es un detalle de estilo:
//
// El saludo TLS en un ESP32 tarda 1–2 s y consume ~45 KB de heap. Hacerlo cada
// 2 segundos es directamente imposible: no daría tiempo, y dos conexiones
// simultáneas dejarían al equipo al borde de quedarse sin memoria. Ambas rutas
// van al MISMO host, así que se serializan sobre un único socket con
// keep-alive y el saludo ocurre una vez cada muchos minutos.
// ============================================================================

#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

#include "certificados.h"
#include "credenciales.h"
#include "tiempo.h"
#include "../almacen/bufer.h"
#include "../modelo/contrato.h"

namespace red {

/** Comando recibido del servidor, pendiente de ejecutar. */
struct ComandoPendiente {
  int64_t id;
  contrato::Comando tipo;
  float parametro;    // peso patrón para calibrar; 0 en los demás
};

/** Resultado a acusar en el siguiente lote. */
struct AcuseComando {
  int64_t id;
  bool ok;
  char detalle[96];
};

struct Salud {
  int32_t rssi;
  uint32_t uptimeS;
  uint32_t heapLibre;
  uint32_t reconexiones;
};

class Publicador {
 public:
  void iniciar(const Credenciales& cred, Tiempo* tiempo) {
    cred_ = cred;
    tiempo_ = tiempo;

    // Validación estricta del certificado. NUNCA setInsecure(): sin ella,
    // cualquiera en la red podría suplantar al servidor y quedarse con el token
    // de ingesta del equipo.
    cliente_.setCACert(CA_RAICES);

    // El núcleo Arduino-ESP32 de esta versión no expone ajuste de los búferes
    // de mbedtls desde WiFiClientSecure, así que se usan los de por defecto.
    // Es la razón por la que hay UNA sola conexión y no dos: con los búferes
    // completos, dos sockets TLS simultáneos dejarían el heap al límite.
    cliente_.setTimeout(8);   // segundos
  }

  // -------------------------------------------------------------------------
  // Ruta A · tiempo real
  // -------------------------------------------------------------------------
  /**
   * Publica una muestra en el canal en vivo. Es efímera por diseño: si falla,
   * NO se reintenta ni se guarda. El histórico va por la Ruta B, así que perder
   * un broadcast solo significa que el dashboard se salta un refresco.
   */
  bool transmitirEnVivo(const contrato::Muestra& m) {
    if (!cred_.puedeTiempoReal() || WiFi.status() != WL_CONNECTED) return false;
    if (!tiempo_->sincronizado()) return false;   // sin hora no hay marca válida

    char ts[32];
    tiempo_->iso8601(m.tsMs, ts, sizeof(ts));

    JsonDocument doc;
    JsonObject msg = doc["messages"].add<JsonObject>();
    msg["topic"] = String("telemetria:") + cred_.slugEquipo;
    msg["event"] = "lectura";
    msg["private"] = true;    // obligatorio: activa la evaluación de RLS
    JsonObject p = msg["payload"].to<JsonObject>();
    p["ts"] = ts;
    for (uint8_t i = 0; i < contrato::NUM_CANALES; i++) {
      const auto c = static_cast<contrato::Canal>(i);
      if (isnan(m.valor[i])) p[contrato::clave(c)] = nullptr;
      else p[contrato::clave(c)] = serialized(String(m.valor[i], 2));
    }
    p["faults"] = m.faults;

    String cuerpo;
    serializeJson(doc, cuerpo);

    const int codigo = enviar("/realtime/v1/api/broadcast", cuerpo,
                              cred_.jwtDispositivo, true, nullptr);
    if (codigo >= 200 && codigo < 300) { vivosOk_++; return true; }
    vivosFallo_++;
    return false;
  }

  // -------------------------------------------------------------------------
  // Ruta B · lote
  // -------------------------------------------------------------------------
  /**
   * Envía un lote y recoge los comandos que el servidor devuelve.
   *
   * Devuelve el número de muestras aceptadas, o -1 ante error recuperable, o
   * -2 ante error definitivo (credenciales inválidas), que el llamador debe
   * tratar de forma distinta: reintentar en un caso, dejar de intentar en el otro.
   */
  int enviarLote(const almacen::Pendiente* muestras, size_t n,
                 const Salud& salud, const char* fw,
                 const AcuseComando* acuses, size_t nAcuses,
                 ComandoPendiente* comandosSalida, size_t maxComandos,
                 size_t& numComandos) {
    numComandos = 0;
    if (WiFi.status() != WL_CONNECTED) return -1;
    if (!tiempo_->sincronizado()) return -1;   // sin hora, el backend rechazaría

    JsonDocument doc;
    doc["device"] = cred_.slugEquipo;

    JsonArray arr = doc["samples"].to<JsonArray>();
    size_t incluidas = 0;
    for (size_t i = 0; i < n; i++) {
      const int64_t epoca = tiempo_->aEpocaMs(muestras[i].marcaMs);
      if (epoca <= 0) continue;   // capturada antes de tener hora: se descarta

      char ts[32];
      tiempo_->iso8601(epoca, ts, sizeof(ts));

      JsonObject s = arr.add<JsonObject>();
      s["ts"] = ts;
      for (uint8_t c = 0; c < contrato::NUM_CANALES; c++) {
        const auto id = static_cast<contrato::Canal>(c);
        if (isnan(muestras[i].valor[c])) s[contrato::clave(id)] = nullptr;
        else s[contrato::clave(id)] = serialized(String(muestras[i].valor[c], 2));
      }
      s["faults"] = muestras[i].faults;
      incluidas++;
    }
    if (incluidas == 0) return 0;

    JsonObject h = doc["health"].to<JsonObject>();
    h["rssi"] = salud.rssi;
    h["uptime_s"] = salud.uptimeS;
    h["free_heap"] = salud.heapLibre;
    h["reconnects"] = salud.reconexiones;
    h["fw"] = fw;

    if (nAcuses > 0) {
      JsonArray res = doc["resultados"].to<JsonArray>();
      for (size_t i = 0; i < nAcuses; i++) {
        JsonObject r = res.add<JsonObject>();
        r["id"] = acuses[i].id;
        r["ok"] = acuses[i].ok;
        r["detalle"]["mensaje"] = acuses[i].detalle;
      }
    }

    String cuerpo;
    serializeJson(doc, cuerpo);

    String respuesta;
    const int codigo = enviar("/functions/v1/ingest", cuerpo,
                              cred_.tokenIngesta, false, &respuesta);

    // 401 significa token inválido o equipo desactivado. Reintentar sería un
    // bucle infinito consumiendo cuota y batería sin ninguna posibilidad de
    // éxito: requiere intervención humana.
    if (codigo == 401) { lotesFallo_++; return -2; }

    if (codigo < 200 || codigo >= 300) {
      lotesFallo_++;
      // 4xx que no sea 429: el lote está mal formado y reenviarlo fallará
      // igual. Se reporta como aceptado para que el llamador lo descarte y no
      // bloquee la cola con datos irreparables.
      if (codigo >= 400 && codigo < 500 && codigo != 429) {
        Serial.printf("[red] lote rechazado (%d), se descarta: %s\n",
                      codigo, respuesta.c_str());
        return static_cast<int>(n);
      }
      return -1;
    }

    lotesOk_++;

    // --- Respuesta: hora del servidor y comandos ---------------------------
    JsonDocument rdoc;
    if (deserializeJson(rdoc, respuesta) == DeserializationError::Ok) {
      const char* sts = rdoc["servidor_ts"];
      if (sts) tiempo_->ajustarDesdeServidor(iso8601AEpocaMs(sts));

      JsonArray cmds = rdoc["comandos"].as<JsonArray>();
      for (JsonObject c : cmds) {
        if (numComandos >= maxComandos) break;
        ComandoPendiente cp;
        cp.id = c["id"] | 0;
        cp.tipo = contrato::comandoDesde(c["comando"] | "");
        cp.parametro = c["parametros"]["peso_conocido_g"] | 0.0f;
        if (cp.id > 0 && cp.tipo != contrato::Comando::DESCONOCIDO) {
          comandosSalida[numComandos++] = cp;
        }
      }
    }

    return static_cast<int>(n);
  }

  uint32_t vivosOk() const { return vivosOk_; }
  uint32_t vivosFallo() const { return vivosFallo_; }
  uint32_t lotesOk() const { return lotesOk_; }
  uint32_t lotesFallo() const { return lotesFallo_; }

 private:
  /**
   * POST con reutilización de conexión.
   * `esJwt` distingue el encabezado apikey, que Realtime exige y las Edge
   * Functions no necesitan.
   */
  int enviar(const char* ruta, const String& cuerpo, const String& autorizacion,
             bool esJwt, String* respuesta) {
    HTTPClient http;
    http.setReuse(true);          // clave: evita rehacer el saludo TLS cada vez
    http.setTimeout(8000);
    http.setConnectTimeout(8000);

    const String url = cred_.urlSupabase + ruta;
    if (!http.begin(cliente_, url)) return -1;

    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", String("Bearer ") + autorizacion);
    if (esJwt && cred_.anonKey.length()) http.addHeader("apikey", cred_.anonKey);

    const int codigo = http.POST(cuerpo);
    if (respuesta) *respuesta = (codigo > 0) ? http.getString() : String();
    http.end();
    return codigo;
  }

  /** Convierte ISO-8601 UTC a época en ms. Solo el formato que emite el backend. */
  static int64_t iso8601AEpocaMs(const char* s) {
    if (!s) return 0;
    int a, me, d, h, mi, se, ms = 0;
    if (sscanf(s, "%d-%d-%dT%d:%d:%d.%3d", &a, &me, &d, &h, &mi, &se, &ms) < 6) {
      return 0;
    }
    // timegm() no existe en la newlib del ESP32, y mktime() interpreta la
    // hora como LOCAL: usarla desplazaría todas las marcas por la zona horaria.
    // La conversión se hace explícitamente en UTC.
    const int64_t dias = diasDesdeEpoca(a, me, d);
    const int64_t seg = dias * 86400LL + h * 3600LL + mi * 60LL + se;
    return seg * 1000LL + ms;
  }

  /**
   * Días desde 1970-01-01 para una fecha del calendario gregoriano proléptico.
   * Algoritmo de Howard Hinnant: exacto y sin tablas ni bucles.
   */
  static int64_t diasDesdeEpoca(int a, int m, int d) {
    a -= (m <= 2);
    const int64_t era = (a >= 0 ? a : a - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(a - era * 400);            // [0, 399]
    const unsigned doy = (153u * (m + (m > 2 ? -3 : 9)) + 2u) / 5u + d - 1;  // [0, 365]
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;        // [0, 146096]
    return era * 146097LL + static_cast<int64_t>(doe) - 719468LL;
  }

  WiFiClientSecure cliente_;
  Credenciales cred_;
  Tiempo* tiempo_ = nullptr;

  uint32_t vivosOk_ = 0, vivosFallo_ = 0, lotesOk_ = 0, lotesFallo_ = 0;
};

}  // namespace red
