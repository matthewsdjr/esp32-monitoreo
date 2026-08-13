// ============================================================================
// Estado de un canal de medición
// ============================================================================
// Lógica pura y probable en el host. Es el equivalente en firmware de
// web/src/logica/estado.ts, y el ORDEN de evaluación debe ser el mismo en
// ambos: si difieren, el equipo enciende un buzzer por una condición que el
// dashboard clasifica de otra manera, y nadie entiende cuál tiene razón.
// ============================================================================

#pragma once
#include <stdint.h>
#include "../modelo/contrato.h"

namespace sensores {

enum class Estado : uint8_t {
  SIN_DATO,      // todavía no ha llegado ninguna lectura
  OK,            // lectura válida y dentro del rango físico
  OBSOLETO,      // sin lectura nueva desde hace demasiado
  FUERA_RANGO,   // fuera del rango físico del sensor: casi siempre cableado
  FALLA,         // error de comunicación o termopar abierto
  DESACTIVADO    // canal apagado por configuración
};

inline const char* nombre(Estado e) {
  switch (e) {
    case Estado::SIN_DATO:    return "SIN_DATO";
    case Estado::OK:          return "OK";
    case Estado::OBSOLETO:    return "OBSOLETO";
    case Estado::FUERA_RANGO: return "FUERA_RANGO";
    case Estado::FALLA:       return "FALLA";
    case Estado::DESACTIVADO: return "DESACTIVADO";
    default:                  return "?";
  }
}

/** Un estado cuenta como falla —y enciende su bit en `faults`— salvo OK. */
inline bool esFalla(Estado e) {
  return e != Estado::OK && e != Estado::DESACTIVADO;
}

struct Limites {
  float minFisico;
  float maxFisico;
  uint32_t msObsoleto;   // antigüedad a partir de la cual el dato es viejo
};

/**
 * Máquina de estados de un canal.
 *
 * `ahoraMs` se recibe como parámetro en vez de leer millis() por dentro: así la
 * clase es determinista y se puede probar el envejecimiento del dato sin
 * esperar en tiempo real.
 */
class Canal {
 public:
  Canal() = default;

  void configurar(contrato::Canal id, Limites lim, bool activo = true) {
    id_ = id;
    lim_ = lim;
    estado_ = activo ? Estado::SIN_DATO : Estado::DESACTIVADO;
  }

  void activar(bool v) {
    if (!v) { estado_ = Estado::DESACTIVADO; return; }
    if (estado_ == Estado::DESACTIVADO) estado_ = Estado::SIN_DATO;
  }

  /** Registra una lectura válida del sensor. */
  void publicar(float valor, uint32_t ahoraMs) {
    if (estado_ == Estado::DESACTIVADO) return;

    ultimoMs_ = ahoraMs;
    tieneDato_ = true;

    // El rango físico se evalúa aquí y no en la capa de red: una lectura de
    // 3000 °C en un MAX6675 que satura en 1024 no es un dato del proceso, es un
    // cable suelto. Guardarla contaminaría el histórico y dispararía alarmas.
    if (valor < lim_.minFisico || valor > lim_.maxFisico) {
      valor_ = valor;              // se conserva para diagnóstico por consola
      estado_ = Estado::FUERA_RANGO;
      return;
    }

    valor_ = valor;
    estado_ = Estado::OK;
  }

  /** Registra una falla explícita del sensor (bus caído, termopar abierto). */
  void publicarFalla(uint32_t ahoraMs) {
    if (estado_ == Estado::DESACTIVADO) return;
    ultimoMs_ = ahoraMs;
    estado_ = Estado::FALLA;
  }

  /**
   * Envejece el estado. Debe llamarse periódicamente aunque no lleguen
   * lecturas: es precisamente cuando el sensor deja de responder que hace falta
   * notarlo, y si solo se evaluara al publicar, un canal muerto se quedaría
   * congelado en OK para siempre.
   */
  void envejecer(uint32_t ahoraMs) {
    if (estado_ == Estado::DESACTIVADO || estado_ == Estado::SIN_DATO) return;

    // Una FALLA explícita es información más precisa que la simple antigüedad,
    // así que no se degrada a OBSOLETO. Mismo criterio que el dashboard.
    if (estado_ == Estado::FALLA) return;

    if (ahoraMs - ultimoMs_ > lim_.msObsoleto) estado_ = Estado::OBSOLETO;
  }

  Estado estado() const { return estado_; }
  contrato::Canal id() const { return id_; }
  uint32_t ultimoMs() const { return ultimoMs_; }

  /**
   * Valor a publicar. Devuelve NaN en cualquier estado que no sea OK: enviar un
   * número junto con el bit de falla activo es contradictorio, y el contrato
   * (docs/API.md §1) obliga a mandar null.
   */
  float valorPublicable() const {
    if (estado_ != Estado::OK || !tieneDato_) return 0.0f / 0.0f;  // NaN
    return valor_;
  }

  /** Último valor crudo, aunque el estado no sea OK. Solo para diagnóstico. */
  float valorCrudo() const { return tieneDato_ ? valor_ : 0.0f / 0.0f; }

  /** Bit que este canal aporta a la máscara `faults`. */
  uint16_t bitSiFalla() const {
    return esFalla(estado_) ? contrato::bitFalla(id_) : 0;
  }

 private:
  contrato::Canal id_ = contrato::Canal::PESO;
  Limites lim_ = {-1e30f, 1e30f, 15000};
  Estado estado_ = Estado::SIN_DATO;
  float valor_ = 0.0f;
  bool tieneDato_ = false;
  uint32_t ultimoMs_ = 0;
};

}  // namespace sensores
