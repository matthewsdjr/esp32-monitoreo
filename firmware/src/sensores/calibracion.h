// ============================================================================
// Conversión de cuentas crudas del HX711 a gramos
// ============================================================================
// Lógica pura, separada del driver de hardware para poder probarla en el host.
// Es el punto donde un error se convierte en pesos incorrectos durante meses sin
// que nadie lo note, así que merece pruebas propias.
// ============================================================================

#pragma once
#include <stdint.h>

namespace sensores {

/**
 * peso = (crudo - offset) / factor
 *
 *   offset  cuentas que entrega la celda descargada (la "tara")
 *   factor  cuentas por gramo
 *
 * Ambos viven en NVS y sobreviven a los reinicios. Sin persistencia, cada corte
 * de energía obligaría a recalibrar, y en la práctica eso significa operar con
 * una báscula descalibrada.
 */
class Calibracion {
 public:
  Calibracion() = default;
  Calibracion(int32_t offset, float factor) : offset_(offset), factor_(factor) {}

  void fijarOffset(int32_t o) { offset_ = o; }
  void fijarFactor(float f) {
    // Un factor de cero o negativo produce división entre cero o pesos con el
    // signo invertido. Se rechaza en vez de aceptarlo y fallar después de forma
    // silenciosa a mitad de la producción.
    if (f > 1e-6f) factor_ = f;
  }

  int32_t offset() const { return offset_; }
  float factor() const { return factor_; }

  bool valida() const { return factor_ > 1e-6f; }

  /** Convierte cuentas crudas a gramos. */
  float aGramos(int32_t crudo) const {
    if (!valida()) return 0.0f / 0.0f;  // NaN: mejor sin dato que un dato falso
    return (static_cast<float>(crudo) - static_cast<float>(offset_)) / factor_;
  }

  /**
   * Tara: toma las cuentas actuales como el nuevo cero.
   * Quien la invoca es responsable de comprobar que la señal esté estable y la
   * báscula vacía; esta función no puede saberlo.
   */
  void tarar(int32_t crudoActual) { offset_ = crudoActual; }

  /**
   * Calibración con un peso patrón.
   *
   * Devuelve false —y no modifica nada— si la lectura con el patrón no difiere
   * lo suficiente de la tara. Ese caso significa que no se colocó el peso, o que
   * la celda no responde; aceptarlo produciría un factor absurdo que hace
   * inservible la báscula hasta que alguien lo note.
   */
  bool calibrar(int32_t crudoConPeso, float pesoConocidoG) {
    if (pesoConocidoG <= 0.0f) return false;

    const int32_t delta = crudoConPeso - offset_;
    // 1000 cuentas sobre 24 bits es un margen conservador: por debajo de eso la
    // relación señal/ruido no permite un factor confiable.
    if (delta < 1000 && delta > -1000) return false;

    const float f = static_cast<float>(delta) / pesoConocidoG;
    if (f <= 1e-6f) return false;   // celda invertida: hay que corregir cableado

    factor_ = f;
    return true;
  }

 private:
  int32_t offset_ = 0;
  float factor_ = 420.0f;  // valor de arranque; se sustituye al calibrar
};

}  // namespace sensores
