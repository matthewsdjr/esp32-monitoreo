// ============================================================================
// Actualización remota por HTTPS
// ============================================================================
// Sin OTA, cada ajuste del firmware obliga a ir a planta con un cable. Con un
// equipo instalado dentro de una línea de producción eso significa, en la
// práctica, que el firmware no se actualiza nunca.
//
// PARTICIÓN DUAL Y REVERSIÓN: la imagen nueva se escribe en la ranura libre y
// el arranque se conmuta al reiniciar. Si la versión nueva no llega a marcarse
// como válida —porque se cuelga antes—, el gestor de arranque vuelve solo a la
// anterior. Sin eso, un firmware defectuoso deja el equipo inservible hasta que
// alguien va físicamente a reprogramarlo.
// ============================================================================

#pragma once
#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Update.h>
#include <esp_ota_ops.h>

#include "certificados.h"

namespace red {

class Ota {
 public:
  /**
   * Confirma que este arranque fue bueno.
   *
   * Debe llamarse SOLO cuando el equipo ya demostró funcionar: conectado a la
   * red y publicando. Marcarlo válido al inicio de setup() anularía la
   * protección, porque una imagen que se cuelga a los diez segundos ya se habría
   * declarado buena y no habría marcha atrás.
   */
  static void confirmarArranque() {
    const esp_partition_t* p = esp_ota_get_running_partition();
    esp_ota_img_states_t estado;
    if (esp_ota_get_state_partition(p, &estado) != ESP_OK) return;

    if (estado == ESP_OTA_IMG_PENDING_VERIFY) {
      esp_ota_mark_app_valid_cancel_rollback();
      Serial.println("[ota] arranque confirmado: la imagen nueva queda fijada");
    }
  }

  /** true si esta imagen todavía está a prueba y puede revertirse. */
  static bool enPeriodoDePrueba() {
    const esp_partition_t* p = esp_ota_get_running_partition();
    esp_ota_img_states_t estado;
    if (esp_ota_get_state_partition(p, &estado) != ESP_OK) return false;
    return estado == ESP_OTA_IMG_PENDING_VERIFY;
  }

  /**
   * Descarga e instala una imagen. Devuelve true si quedó lista; el llamador
   * debe reiniciar.
   *
   * Bloquea varios minutos, así que se invoca desde la tarea de red y NUNCA
   * desde una tarea de sensores: dejaría de medirse durante la descarga.
   */
  bool actualizar(const String& url) {
    Serial.printf("[ota] descargando %s\n", url.c_str());

    WiFiClientSecure cliente;
    cliente.setCACert(CA_RAICES);   // misma validación estricta que la ingesta
    cliente.setTimeout(20);

    HTTPClient http;
    http.setTimeout(20000);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);  // GitHub redirige
    if (!http.begin(cliente, url)) {
      Serial.println("[ota] no se pudo iniciar la descarga");
      return false;
    }

    const int codigo = http.GET();
    if (codigo != HTTP_CODE_OK) {
      Serial.printf("[ota] respuesta %d\n", codigo);
      http.end();
      return false;
    }

    const int total = http.getSize();
    // Sin longitud declarada no se puede verificar que la imagen llegó completa.
    // Escribir una imagen truncada es peor que no actualizar: el equipo
    // arrancaría en una ranura corrupta y dependería de la reversión.
    if (total <= 0) {
      Serial.println("[ota] el servidor no declaró el tamaño; se aborta");
      http.end();
      return false;
    }

    if (!Update.begin(total)) {
      Serial.printf("[ota] no cabe en la ranura libre (%d bytes)\n", total);
      http.end();
      return false;
    }

    const size_t escritos = Update.writeStream(*http.getStreamPtr());
    http.end();

    if (escritos != static_cast<size_t>(total)) {
      Serial.printf("[ota] descarga incompleta: %u de %d bytes\n",
                    (unsigned)escritos, total);
      Update.abort();
      return false;
    }

    if (!Update.end(true)) {
      Serial.printf("[ota] error al finalizar: %s\n", Update.errorString());
      return false;
    }

    Serial.println("[ota] imagen instalada · reiniciando para arrancar con ella");
    return true;
  }
};

}  // namespace red
