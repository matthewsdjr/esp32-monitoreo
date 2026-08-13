// ============================================================================
// Filtros para señales de sensor
// ============================================================================
// Lógica pura, sin dependencias de Arduino: se compila y se prueba en el host.
// Es donde viven los errores sutiles (índices, arranque en frío, división entre
// cero), así que conviene poder ejercitarla sin subir nada al equipo.
// ============================================================================

#pragma once
#include <stdint.h>
#include <stddef.h>

namespace filtros {

/**
 * Mediana móvil.
 *
 * Va PRIMERO en la cadena, antes de cualquier promedio. Un pico aislado —el
 * arranque de un motor acoplado a la estructura, un golpe en la tolva— desplaza
 * una media pero NO desplaza una mediana. Promediar primero contamina la
 * ventana entera con el pico durante N muestras.
 *
 * N impar a propósito: con N par la mediana es el promedio de los dos centrales
 * y vuelve a ser sensible a un valor extremo.
 */
template <size_t N>
class Mediana {
  static_assert(N % 2 == 1, "La ventana de mediana debe ser impar");

 public:
  void agregar(float v) {
    buf_[idx_] = v;
    idx_ = (idx_ + 1) % N;
    if (llenas_ < N) llenas_++;
  }

  bool lista() const { return llenas_ == N; }
  size_t muestras() const { return llenas_; }

  /**
   * Mediana de las muestras disponibles. Con la ventana a medio llenar usa solo
   * las que hay, para que el sensor entregue un valor útil desde el arranque en
   * vez de esperar N ciclos en silencio.
   */
  float valor() const {
    if (llenas_ == 0) return 0.0f / 0.0f;  // NaN

    float orden[N];
    for (size_t i = 0; i < llenas_; i++) orden[i] = buf_[i];

    // Inserción: N es pequeño (típicamente 5..9) y no asigna memoria.
    for (size_t i = 1; i < llenas_; i++) {
      float x = orden[i];
      size_t j = i;
      while (j > 0 && orden[j - 1] > x) { orden[j] = orden[j - 1]; j--; }
      orden[j] = x;
    }
    return orden[llenas_ / 2];
  }

  void reiniciar() { idx_ = 0; llenas_ = 0; }

 private:
  float buf_[N] = {0};
  size_t idx_ = 0;
  size_t llenas_ = 0;
};

/**
 * Media exponencial (EMA).
 *
 * Va DESPUÉS de la mediana: suaviza el ruido gaussiano que la mediana no quita.
 * `alfa` cercano a 1 responde rápido y filtra poco; cercano a 0 es muy estable
 * pero lento. Se inicializa con la primera muestra en vez de con cero: arrancar
 * en cero produce una rampa artificial de varios segundos que en la báscula se
 * lee como si el material estuviera cayendo.
 */
class MediaExponencial {
 public:
  explicit MediaExponencial(float alfa = 0.2f) : alfa_(alfa) {}

  void configurar(float alfa) { alfa_ = alfa; }

  float agregar(float v) {
    if (!inicializada_) {
      valor_ = v;
      inicializada_ = true;
    } else {
      valor_ = alfa_ * v + (1.0f - alfa_) * valor_;
    }
    return valor_;
  }

  float valor() const { return inicializada_ ? valor_ : 0.0f / 0.0f; }
  bool inicializada() const { return inicializada_; }
  void reiniciar() { inicializada_ = false; valor_ = 0.0f; }

 private:
  float alfa_;
  float valor_ = 0.0f;
  bool inicializada_ = false;
};

/**
 * Detector de estabilidad.
 *
 * Indica si la señal lleva `ciclos` lecturas consecutivas dentro de una banda
 * de ±`tolerancia`. La tara solo debe ejecutarse sobre una señal estable: tarar
 * mientras la báscula oscila fija un cero equivocado que después contamina todo
 * el histórico y no deja rastro de por qué.
 */
class Estabilidad {
 public:
  Estabilidad(float tolerancia, uint16_t ciclos)
      : tolerancia_(tolerancia), objetivo_(ciclos) {}

  void agregar(float v) {
    if (!tieneRef_) { ref_ = v; tieneRef_ = true; consecutivas_ = 1; return; }

    float d = v - ref_;
    if (d < 0) d = -d;

    if (d <= tolerancia_) {
      if (consecutivas_ < 0xFFFF) consecutivas_++;
    } else {
      // La referencia se mueve al valor nuevo: si la carga cambió, se empieza a
      // contar estabilidad alrededor del nivel nuevo, no del viejo.
      ref_ = v;
      consecutivas_ = 1;
    }
  }

  bool estable() const { return consecutivas_ >= objetivo_; }
  uint16_t consecutivas() const { return consecutivas_; }
  void reiniciar() { tieneRef_ = false; consecutivas_ = 0; }

 private:
  float tolerancia_;
  uint16_t objetivo_;
  float ref_ = 0.0f;
  bool tieneRef_ = false;
  uint16_t consecutivas_ = 0;
};

}  // namespace filtros
