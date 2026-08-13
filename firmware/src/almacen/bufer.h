// ============================================================================
// Búfer de muestras sin publicar
// ============================================================================
// Criterio de aceptación de la Fase 3: una desconexión de 30 min no debe dejar
// huecos en el histórico al reconectar. Eso obliga a guardar las muestras
// localmente y drenarlas después CON SU MARCA DE TIEMPO ORIGINAL, no con la del
// momento del envío.
//
// Dos niveles:
//   RAM      anillo rápido, cubre la caída típica (minutos)
//   LittleFS respaldo en flash cuando la caída se prolonga (horas)
//
// Se guarda `marcaMs` monótona, no la época: al arrancar todavía no hay hora, y
// convertir en el momento de publicar permite que las muestras previas al NTP
// recuperen su hora real. Ver red/tiempo.h.
// ============================================================================

#pragma once
#include <stdint.h>
#include <stddef.h>
#include "../modelo/contrato.h"

// El anillo en RAM es lógica pura y se prueba en el host (make test). Solo el
// respaldo en flash necesita las cabeceras de Arduino, así que se aíslan.
#ifdef ARDUINO
#include <Arduino.h>
#include <LittleFS.h>
#endif

namespace almacen {

/** Muestra pendiente de publicar. 28 bytes. */
struct Pendiente {
  uint32_t marcaMs;                        // millis() de la captura
  float valor[contrato::NUM_CANALES];
  uint16_t faults;
  uint16_t _relleno;
};

/**
 * Anillo en RAM.
 *
 * A una muestra cada 5 s, 720 posiciones cubren 1 hora y ocupan ~20 KB. Es el
 * compromiso entre cubrir la caída típica y dejar heap libre para TLS, que
 * necesita ~45 KB durante el saludo.
 */
template <size_t N>
class AnilloRam {
 public:
  void agregar(const Pendiente& p) {
    buf_[fin_] = p;
    fin_ = (fin_ + 1) % N;
    if (llenas_ == N) {
      // Anillo lleno: se descarta la MÁS ANTIGUA. Preferir el dato reciente es
      // deliberado — ante una caída muy larga, saber qué pasa ahora importa más
      // que completar un histórico que ya tiene un hueco irrecuperable.
      inicio_ = (inicio_ + 1) % N;
      descartadas_++;
    } else {
      llenas_++;
    }
  }

  size_t tamano() const { return llenas_; }
  bool vacio() const { return llenas_ == 0; }
  bool lleno() const { return llenas_ == N; }
  uint32_t descartadas() const { return descartadas_; }
  static constexpr size_t capacidad() { return N; }

  /** Copia hasta `max` muestras desde la más antigua, SIN retirarlas. */
  size_t asomar(Pendiente* salida, size_t max) const {
    const size_t n = llenas_ < max ? llenas_ : max;
    for (size_t i = 0; i < n; i++) salida[i] = buf_[(inicio_ + i) % N];
    return n;
  }

  /**
   * Retira `n` muestras del frente. Se llama SOLO tras confirmar la entrega:
   * retirar antes de la confirmación es la forma clásica de perder datos cuando
   * la respuesta se pierde en un timeout ambiguo.
   */
  void descartarFrente(size_t n) {
    if (n > llenas_) n = llenas_;
    inicio_ = (inicio_ + n) % N;
    llenas_ -= n;
  }

  void vaciar() { inicio_ = fin_ = llenas_ = 0; }

 private:
  Pendiente buf_[N];
  size_t inicio_ = 0, fin_ = 0, llenas_ = 0;
  uint32_t descartadas_ = 0;
};

#ifdef ARDUINO
// ---------------------------------------------------------------------------
/**
 * Respaldo en flash.
 *
 * Se activa cuando el anillo en RAM está por llenarse. Escribe registros
 * binarios de tamaño fijo, lo que permite leerlos en orden sin analizar nada.
 *
 * La flash del ESP32 tolera del orden de 100 000 borrados por sector, así que
 * el respaldo solo se usa en caídas prolongadas y NO como camino normal: volcar
 * cada muestra desgastaría la memoria en meses.
 */
class RespaldoFlash {
 public:
  bool iniciar() {
    montado_ = LittleFS.begin(true);   // true: formatea si está corrupta
    if (montado_) contarRegistros();
    return montado_;
  }

  bool disponible() const { return montado_; }
  uint32_t registros() const { return registros_; }

  /** Vuelca un bloque de muestras al final del archivo. */
  bool volcar(const Pendiente* p, size_t n) {
    if (!montado_ || n == 0) return false;

    // Tope de tamaño: sin él, una caída de días llenaría la partición y el
    // sistema de archivos dejaría de responder, que es peor que perder datos
    // antiguos de forma controlada.
    if (registros_ >= MAX_REGISTROS) return false;

    File f = LittleFS.open(RUTA, FILE_APPEND);
    if (!f) return false;
    const size_t escritos = f.write(reinterpret_cast<const uint8_t*>(p),
                                    n * sizeof(Pendiente));
    f.close();
    registros_ += escritos / sizeof(Pendiente);
    return escritos == n * sizeof(Pendiente);
  }

  /** Lee hasta `max` registros desde el desplazamiento indicado. */
  size_t leerDesde(uint32_t indice, Pendiente* salida, size_t max) {
    if (!montado_ || indice >= registros_) return 0;

    File f = LittleFS.open(RUTA, FILE_READ);
    if (!f) return 0;
    f.seek(indice * sizeof(Pendiente));

    size_t n = registros_ - indice;
    if (n > max) n = max;
    const size_t leidos = f.read(reinterpret_cast<uint8_t*>(salida),
                                 n * sizeof(Pendiente));
    f.close();
    return leidos / sizeof(Pendiente);
  }

  /**
   * Borra el archivo completo. Se llama cuando TODOS sus registros ya se
   * entregaron: LittleFS no permite recortar por delante de forma eficiente, y
   * reescribir el archivo entero en cada lote desgastaría la flash.
   */
  void limpiar() {
    if (!montado_) return;
    LittleFS.remove(RUTA);
    registros_ = 0;
  }

 private:
  static constexpr const char* RUTA = "/pend.bin";
  // Acotado al tamaño de la partición de archivos de min_spiffs (~190 KB).
  // 6 000 registros × 28 B ≈ 168 KB ≈ 8.3 h a una muestra cada 5 s. Sumadas a
  // la hora que cubre el anillo en RAM, dan más de 9 h de autonomía sin red.
  static constexpr uint32_t MAX_REGISTROS = 6000;

  void contarRegistros() {
    File f = LittleFS.open(RUTA, FILE_READ);
    registros_ = f ? (f.size() / sizeof(Pendiente)) : 0;
    if (f) f.close();
  }

  bool montado_ = false;
  uint32_t registros_ = 0;
};
#endif  // ARDUINO

}  // namespace almacen
