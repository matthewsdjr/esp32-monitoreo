// ============================================================================
// Hora del equipo
// ============================================================================
// EL PROBLEMA QUE RESUELVE ESTE ARCHIVO
//
// El ESP32 arranca sin saber la fecha. Entre el encendido y la primera
// sincronización NTP pasan segundos o minutos —más si el WiFi tarda—, y durante
// ese tiempo el equipo ya está midiendo. Si esas muestras se guardaran con
// marca cero o con la hora del momento del envío, el histórico mostraría todo
// el arranque comprimido en un instante, o directamente el backend las
// rechazaría por `ts_antiguo`.
//
// SOLUCIÓN: el búfer guarda `millis()` (monótono, disponible desde el arranque)
// y la conversión a época ocurre en el momento de PUBLICAR, usando el desfase
// que se estableció al sincronizar. Una muestra tomada antes del NTP recupera
// así su hora real en cuanto la hora se conoce.
// ============================================================================

#pragma once
#include <Arduino.h>
#include <time.h>

namespace red {

class Tiempo {
 public:
  /** Lanza la sincronización. No bloquea: NTP responde cuando puede. */
  void iniciar() {
    // Dos servidores: si el primero no responde, el segundo cubre. `pool.ntp.org`
    // resuelve por geolocalización y suele ser el más cercano.
    configTime(0, 0, "pool.ntp.org", "time.google.com");
  }

  /**
   * Comprueba si ya hay hora válida y, la primera vez, fija el desfase entre el
   * reloj monótono y la época.
   */
  bool actualizar() {
    if (sincronizado_) return true;

    const time_t ahora = time(nullptr);
    // Antes de sincronizar, time() devuelve un valor cercano a cero. El umbral
    // corresponde a 2023-01-01: cualquier cosa anterior es reloj sin ajustar.
    if (ahora < 1672531200L) return false;

    const uint32_t ms = millis();
    desfaseMs_ = static_cast<int64_t>(ahora) * 1000LL - static_cast<int64_t>(ms);
    sincronizado_ = true;
    ultimaSyncMs_ = ms;
    sincronizaciones_++;
    return true;
  }

  /** Resincroniza cada 6 h para corregir la deriva del oscilador del ESP32. */
  void resincronizarSiToca() {
    if (!sincronizado_) return;
    if (millis() - ultimaSyncMs_ < 6UL * 3600UL * 1000UL) return;
    sincronizado_ = false;   // actualizar() volverá a fijar el desfase
    iniciar();
  }

  bool sincronizado() const { return sincronizado_; }
  uint32_t sincronizaciones() const { return sincronizaciones_; }

  /**
   * Convierte una marca monótona (millis) a época en milisegundos.
   * Devuelve 0 si todavía no hay hora: quien publica debe descartar esas
   * muestras en vez de inventarles una fecha.
   */
  int64_t aEpocaMs(uint32_t marcaMs) const {
    if (!sincronizado_) return 0;
    return static_cast<int64_t>(marcaMs) + desfaseMs_;
  }

  int64_t ahoraEpocaMs() const { return aEpocaMs(millis()); }

  /** ISO-8601 en UTC, el formato que exige el contrato (docs/API.md). */
  void iso8601(int64_t epocaMs, char* salida, size_t largo) const {
    if (epocaMs <= 0) { if (largo) salida[0] = '\0'; return; }

    const time_t seg = static_cast<time_t>(epocaMs / 1000);
    const int ms = static_cast<int>(epocaMs % 1000);
    struct tm t;
    gmtime_r(&seg, &t);
    snprintf(salida, largo, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
             t.tm_hour, t.tm_min, t.tm_sec, ms);
  }

  /**
   * Ajusta el reloj con la hora que devuelve el servidor en cada respuesta de
   * ingesta. Evita depender solo de NTP: si la red bloquea el puerto 123 —cosa
   * habitual en redes corporativas—, el equipo se sincroniza igual por HTTPS.
   */
  void ajustarDesdeServidor(int64_t epocaMsServidor) {
    if (epocaMsServidor <= 0) return;
    const uint32_t ms = millis();
    const int64_t nuevoDesfase =
        epocaMsServidor - static_cast<int64_t>(ms);

    if (!sincronizado_) {
      desfaseMs_ = nuevoDesfase;
      sincronizado_ = true;
      ultimaSyncMs_ = ms;
      sincronizaciones_++;
      return;
    }

    // Ya sincronizado: solo se corrige si la diferencia es grande. Perseguir
    // cada milisegundo de latencia de red produciría saltos constantes en las
    // marcas de tiempo, y eso rompe el índice único (device_id, ts).
    const int64_t deriva = nuevoDesfase - desfaseMs_;
    if (deriva > 5000 || deriva < -5000) {
      desfaseMs_ = nuevoDesfase;
      ultimaSyncMs_ = ms;
    }
  }

 private:
  bool sincronizado_ = false;
  int64_t desfaseMs_ = 0;      // época(ms) = millis() + desfaseMs_
  uint32_t ultimaSyncMs_ = 0;
  uint32_t sincronizaciones_ = 0;
};

}  // namespace red
