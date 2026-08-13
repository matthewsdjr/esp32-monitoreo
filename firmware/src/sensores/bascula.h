// ============================================================================
// HX711 — driver no bloqueante
// ============================================================================
// Sustituye a `scale.get_units(10)`, que bloquea ~1 segundo esperando 10
// conversiones a 10 SPS. Con la pila de red activa (Fase 3), un segundo sin
// ceder CPU basta para que se pierdan paquetes y se caiga la conexión TLS.
//
// Aquí solo se lee cuando el chip AVISA que tiene dato listo (DOUT en bajo), y
// la lectura completa toma ~60 µs.
// ============================================================================

#pragma once
#include <Arduino.h>
#include "filtros.h"
#include "calibracion.h"

namespace sensores {

class Bascula {
 public:
  void iniciar(uint8_t pinDatos, uint8_t pinReloj) {
    pinDatos_ = pinDatos;
    pinReloj_ = pinReloj;
    pinMode(pinDatos_, INPUT_PULLUP);
    pinMode(pinReloj_, OUTPUT);
    digitalWrite(pinReloj_, LOW);
    despertar();
  }

  /** El HX711 señala "dato listo" poniendo DOUT en bajo. */
  bool listo() const { return digitalRead(pinDatos_) == LOW; }

  /**
   * Lee una conversión SIN esperar. Devuelve false de inmediato si no hay dato.
   *
   * Esta es la diferencia con la librería estándar: `read()` se queda esperando
   * hasta que haya conversión, y a 10 SPS eso son hasta 100 ms por llamada.
   */
  bool leerCrudo(int32_t& salida) {
    if (!listo()) return false;

    uint32_t valor = 0;

    // ---------------------------------------------------------------------
    // SECCIÓN CRÍTICA — el detalle que rompe este driver bajo FreeRTOS
    // ---------------------------------------------------------------------
    // El HX711 entra en apagado si su pin de reloj permanece en ALTO más de
    // 60 µs. Sin protección, el planificador puede expropiar esta tarea a la
    // mitad de un pulso: el chip se apaga, la trama sale corrupta y el síntoma
    // es una lectura de basura ocasional, imposible de reproducir a voluntad.
    //
    // Los ~60 µs que dura la sección son aceptables: no afectan al WiFi, que
    // trabaja con ventanas mucho mayores.
    portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
    portENTER_CRITICAL(&mux);

    for (uint8_t i = 0; i < 24; i++) {
      digitalWrite(pinReloj_, HIGH);
      delayMicroseconds(1);
      valor = (valor << 1) | (digitalRead(pinDatos_) == HIGH ? 1u : 0u);
      digitalWrite(pinReloj_, LOW);
      delayMicroseconds(1);
    }

    // Pulsos extra: seleccionan canal y ganancia de la SIGUIENTE conversión.
    // 1 = canal A ganancia 128, que es la configuración de una celda de carga.
    for (uint8_t i = 0; i < ganancia_; i++) {
      digitalWrite(pinReloj_, HIGH);
      delayMicroseconds(1);
      digitalWrite(pinReloj_, LOW);
      delayMicroseconds(1);
    }

    portEXIT_CRITICAL(&mux);
    // ---------------------------------------------------------------------

    // Extensión de signo de 24 a 32 bits (complemento a dos).
    if (valor & 0x800000) valor |= 0xFF000000;
    salida = static_cast<int32_t>(valor);

    // Saturación: con la celda desconectada o el amplificador saturado, el
    // HX711 devuelve el extremo del rango. Es una falla de cableado, no un peso.
    if (salida == static_cast<int32_t>(0xFF800000) || salida == 0x7FFFFF) {
      saturaciones_++;
      return false;
    }

    saturaciones_ = 0;
    ultimoCrudo_ = salida;
    return true;
  }

  /**
   * Procesa una lectura si la hay. Devuelve true cuando produjo un valor nuevo.
   * Se llama con frecuencia; internamente solo trabaja cuando el chip tiene dato.
   */
  bool actualizar() {
    int32_t crudo;
    if (!leerCrudo(crudo)) return false;

    mediana_.agregar(static_cast<float>(crudo));
    const float suave = ema_.agregar(mediana_.valor());
    crudoFiltrado_ = static_cast<int32_t>(suave);

    estabilidad_.agregar(suave);
    gramos_ = cal_.aGramos(crudoFiltrado_);
    return true;
  }

  /** Varias saturaciones seguidas indican celda desconectada o mal cableada. */
  bool sospechaDesconexion() const { return saturaciones_ >= 5; }

  float gramos() const { return gramos_; }
  int32_t crudoFiltrado() const { return crudoFiltrado_; }
  int32_t ultimoCrudo() const { return ultimoCrudo_; }
  bool estable() const { return estabilidad_.estable(); }

  Calibracion& calibracion() { return cal_; }
  const Calibracion& calibracion() const { return cal_; }

  /**
   * Tara. Falla si la señal no está estable.
   *
   * Rechazar aquí es importante: una tara tomada mientras la báscula oscila fija
   * un cero equivocado, y ese error se propaga a todo el histórico posterior sin
   * dejar rastro de su origen.
   */
  bool tarar() {
    if (!estabilidad_.estable()) return false;
    cal_.tarar(crudoFiltrado_);
    gramos_ = cal_.aGramos(crudoFiltrado_);
    return true;
  }

  /** Calibración con peso patrón. Exige señal estable, igual que la tara. */
  bool calibrar(float pesoConocidoG) {
    if (!estabilidad_.estable()) return false;
    return cal_.calibrar(crudoFiltrado_, pesoConocidoG);
  }

  void dormir() {
    digitalWrite(pinReloj_, LOW);
    digitalWrite(pinReloj_, HIGH);
    delayMicroseconds(70);   // >60 µs en alto apaga el chip
  }

  void despertar() {
    digitalWrite(pinReloj_, LOW);
    delayMicroseconds(70);
  }

 private:
  uint8_t pinDatos_ = 0;
  uint8_t pinReloj_ = 0;
  uint8_t ganancia_ = 1;      // 1 = canal A, ganancia 128

  filtros::Mediana<9> mediana_;
  filtros::MediaExponencial ema_{0.25f};
  // Tolerancia en CUENTAS crudas, no en gramos: el detector trabaja antes de la
  // conversión, así que no depende del factor de calibración vigente.
  filtros::Estabilidad estabilidad_{150.0f, 10};

  Calibracion cal_;
  int32_t ultimoCrudo_ = 0;
  int32_t crudoFiltrado_ = 0;
  float gramos_ = 0.0f;
  uint8_t saturaciones_ = 0;
};

}  // namespace sensores
