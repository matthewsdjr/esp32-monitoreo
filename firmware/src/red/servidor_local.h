// ============================================================================
// Página local de servicio
// ============================================================================
// Accesible solo desde la red de planta (http://<ip>). Existe porque hay tareas
// que no pueden depender de internet:
//
//   · Calibrar la báscula estando físicamente frente a ella.
//   · Diagnosticar por qué el equipo no publica, justamente cuando no publica.
//
// Es HTTP plano y sin autenticación: vive en la LAN, no se expone a internet, y
// exigir credenciales a un técnico que está de pie junto al equipo solo lograría
// que nadie la use. La tara remota —la que sí es alcanzable desde fuera— sigue
// exigiendo PIN a través de la Edge Function.
// ============================================================================

#pragma once
#include <Arduino.h>
#include <WebServer.h>
#include <ESPmDNS.h>

namespace red {

/** Datos que la página muestra. El firmware los rellena en cada petición. */
struct EstadoServicio {
  const char* fw;
  bool wifiConectado;
  int32_t rssi;
  bool horaSincronizada;
  uint32_t uptimeS;
  uint32_t heapLibre;
  uint32_t pendientesRam;
  uint32_t pendientesFlash;
  uint32_t lotesOk;
  uint32_t lotesFallo;
  uint32_t vivosOk;
  uint32_t vivosFallo;
  float peso;
  bool bascEstable;
  int32_t offset;
  float factor;
  const char* estadoCanal[contrato::NUM_CANALES];
  float valorCanal[contrato::NUM_CANALES];
};

class ServidorLocal {
 public:
  using Callback = void (*)();
  using CallbackFloat = void (*)(float);

  void iniciar(EstadoServicio* (*obtenerEstado)(),
               Callback alTarar, CallbackFloat alCalibrar) {
    obtenerEstado_ = obtenerEstado;
    alTarar_ = alTarar;
    alCalibrar_ = alCalibrar;

    // mDNS evita tener que averiguar la IP que asignó el router: el técnico
    // escribe siempre la misma dirección.
    if (MDNS.begin("monitoreo")) MDNS.addService("http", "tcp", 80);

    srv_.on("/", [this]() { srv_.send(200, "text/html; charset=utf-8", pagina()); });

    srv_.on("/tara", HTTP_POST, [this]() {
      if (alTarar_) alTarar_();
      srv_.sendHeader("Location", "/");
      srv_.send(303);
    });

    srv_.on("/calibrar", HTTP_POST, [this]() {
      const float p = srv_.arg("peso").toFloat();
      if (p > 0 && alCalibrar_) alCalibrar_(p);
      srv_.sendHeader("Location", "/");
      srv_.send(303);
    });

    srv_.on("/reiniciar", HTTP_POST, [this]() {
      srv_.send(200, "text/plain", "reiniciando");
      delay(200);
      ESP.restart();
    });

    srv_.begin();
  }

  void atender() { srv_.handleClient(); }

 private:
  String pagina() {
    EstadoServicio* e = obtenerEstado_();

    String h = F("<!doctype html><meta charset='utf-8'>"
      "<meta name='viewport' content='width=device-width,initial-scale=1'>"
      "<meta http-equiv='refresh' content='5'>"
      "<title>Equipo de monitoreo</title>"
      "<style>body{font-family:system-ui;max-width:40rem;margin:0 auto;padding:1.2rem;"
      "background:#f9f9f7;color:#0b0b0b}h2{margin:0 0 1rem;font-size:1.1rem}"
      "table{width:100%;border-collapse:collapse;font-size:.85rem;margin-bottom:1.2rem}"
      "td{padding:.35rem 0;border-bottom:1px solid #e1e0d9}"
      "td:last-child{text-align:right;font-weight:600}"
      ".ok{color:#0ca30c}.mal{color:#d03b3b}.adv{color:#b45309}"
      "form{display:inline}button{padding:.6rem 1rem;border:1px solid #c3c2b7;"
      "background:#fff;border-radius:.4rem;font-size:.9rem;margin-right:.4rem}"
      "input{padding:.5rem;border:1px solid #c3c2b7;border-radius:.4rem;width:6rem}"
      "</style><h2>Equipo de monitoreo</h2><table>");

    auto fila = [&](const char* k, const String& v, const char* cls = "") {
      h += "<tr><td>"; h += k; h += "</td><td class='"; h += cls; h += "'>";
      h += v; h += "</td></tr>";
    };

    fila("Firmware", e->fw);
    fila("WiFi", e->wifiConectado ? String(e->rssi) + " dBm" : "SIN CONEXIÓN",
         e->wifiConectado ? "ok" : "mal");
    fila("Hora", e->horaSincronizada ? "sincronizada" : "SIN SINCRONIZAR",
         e->horaSincronizada ? "ok" : "adv");
    fila("Activo", String(e->uptimeS / 60) + " min");
    fila("Memoria libre", String(e->heapLibre / 1024) + " KB");
    fila("Pendientes en RAM", String(e->pendientesRam));
    fila("Pendientes en flash", String(e->pendientesFlash),
         e->pendientesFlash > 0 ? "adv" : "");
    fila("Lotes enviados", String(e->lotesOk) + " ok / " + String(e->lotesFallo) + " fallo",
         e->lotesFallo > e->lotesOk ? "mal" : "ok");
    fila("Tiempo real", String(e->vivosOk) + " ok / " + String(e->vivosFallo) + " fallo");

    h += F("</table><h2>Sensores</h2><table>");
    for (uint8_t i = 0; i < contrato::NUM_CANALES; i++) {
      const auto c = static_cast<contrato::Canal>(i);
      const bool ok = String(e->estadoCanal[i]) == "OK";
      String v = isnan(e->valorCanal[i]) ? String("—") : String(e->valorCanal[i], 1);
      v += " · "; v += e->estadoCanal[i];
      fila(contrato::slug(c), v, ok ? "ok" : "mal");
    }

    h += F("</table><h2>Báscula</h2><table>");
    fila("Peso", String(e->peso, 1) + " g");
    fila("Señal", e->bascEstable ? "estable" : "inestable",
         e->bascEstable ? "ok" : "adv");
    fila("Tara (offset)", String(e->offset));
    fila("Factor", String(e->factor, 3) + " cuentas/g");
    h += F("</table>"
      "<form method='POST' action='/tara'><button>Tarar</button></form>"
      "<form method='POST' action='/calibrar'>"
      "<input name='peso' type='number' step='0.1' placeholder='gramos'>"
      "<button>Calibrar</button></form>"
      "<form method='POST' action='/reiniciar' "
      "onsubmit='return confirm(\"¿Reiniciar el equipo?\")'>"
      "<button>Reiniciar</button></form>"
      "<p style='color:#898781;font-size:.75rem;margin-top:1.5rem'>"
      "Tarar con la báscula vacía y la señal estable. Para calibrar, coloca un "
      "peso patrón e indica su valor en gramos.</p>");
    return h;
  }

  WebServer srv_{80};
  EstadoServicio* (*obtenerEstado_)() = nullptr;
  Callback alTarar_ = nullptr;
  CallbackFloat alCalibrar_ = nullptr;
};

}  // namespace red
