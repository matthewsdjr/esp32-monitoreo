// ============================================================================
// MAX6675 — driver con detección de falla diferenciada
// ============================================================================
// Dos problemas del código original:
//
// 1. `readCelsius()` sin control de cadencia. El MAX6675 tarda 170–220 ms en
//    convertir; leerlo antes devuelve el valor ANTERIOR sin avisar. El síntoma
//    es una gráfica que parece correcta pero va retrasada, o que se queda
//    congelada en un valor sin que nada indique el problema.
//
// 2. La librería estándar colapsa todos los errores en NaN. Aquí se lee la
//    trama cruda de 16 bits para distinguir dos fallas con acciones correctivas
//    distintas: termopar abierto (cambiar la sonda) y fallo de bus SPI
//    (revisar cableado o alimentación).
//
// Formato de la trama:
//   bit 15    dummy (siempre 0)
//   bits 14-3 temperatura, 12 bits, resolución 0.25 °C
//   bit 2     termopar abierto
//   bit 1     identificador del dispositivo (siempre 0)
//   bit 0     estado tri-state
// ============================================================================

#pragma once
#include <Arduino.h>

namespace sensores {

enum class FallaTermopar : uint8_t {
  NINGUNA,
  ABIERTO,       // sonda desconectada o rota  -> cambiar termopar
  BUS,           // trama toda 0 o toda 1      -> revisar cableado / alimentación
  DEMASIADO_PRONTO
};

inline const char* nombre(FallaTermopar f) {
  switch (f) {
    case FallaTermopar::NINGUNA:           return "ok";
    case FallaTermopar::ABIERTO:           return "termopar abierto";
    case FallaTermopar::BUS:               return "fallo de bus SPI";
    case FallaTermopar::DEMASIADO_PRONTO:  return "lectura demasiado pronto";
    default:                               return "?";
  }
}

/**
 * Un MAX6675. Varios comparten SCK y SO; cada uno tiene su propio CS.
 * El chip pone SO en alta impedancia mientras su CS está en alto, así que el
 * bus compartido es correcto siempre que nunca se active más de un CS a la vez.
 */
class Termopar {
 public:
  void iniciar(uint8_t pinSck, uint8_t pinCs, uint8_t pinSo) {
    pinSck_ = pinSck; pinCs_ = pinCs; pinSo_ = pinSo;
    pinMode(pinCs_, OUTPUT);
    pinMode(pinSck_, OUTPUT);
    pinMode(pinSo_, INPUT);
    digitalWrite(pinCs_, HIGH);
    digitalWrite(pinSck_, LOW);
  }

  /**
   * Intenta leer. Devuelve false si aún no pasó el tiempo mínimo de conversión
   * o si hubo falla; en ese caso `falla()` explica cuál.
   */
  bool leer(float& celsius) {
    const uint32_t ahora = millis();

    // Respetar la conversión no es opcional: leer antes devuelve el valor
    // anterior en silencio, y eso produce una gráfica plausible pero falsa.
    if (ultimaMs_ != 0 && (ahora - ultimaMs_) < MS_CONVERSION) {
      falla_ = FallaTermopar::DEMASIADO_PRONTO;
      return false;
    }
    ultimaMs_ = ahora;

    const uint16_t trama = leerTrama();

    // Todo ceros o todo unos: no hay chip respondiendo. Con SO flotante se lee
    // 0xFFFF; con SO a tierra, 0x0000. Ninguna de las dos es una temperatura.
    if (trama == 0xFFFF || trama == 0x0000) {
      falla_ = FallaTermopar::BUS;
      return false;
    }

    if (trama & 0x0004) {          // bit 2: termopar abierto
      falla_ = FallaTermopar::ABIERTO;
      return false;
    }

    falla_ = FallaTermopar::NINGUNA;
    celsius = static_cast<float>(trama >> 3) * 0.25f;
    ultimaTemp_ = celsius;
    return true;
  }

  FallaTermopar falla() const { return falla_; }
  float ultimaTemp() const { return ultimaTemp_; }

  /** true cuando ya pasó el tiempo de conversión y vale la pena leer. */
  bool listo() const {
    return ultimaMs_ == 0 || (millis() - ultimaMs_) >= MS_CONVERSION;
  }

 private:
  // Hoja de datos: 170–220 ms. Se usa 250 ms con margen para la deriva del
  // oscilador interno con la temperatura ambiente.
  static constexpr uint32_t MS_CONVERSION = 250;

  uint16_t leerTrama() {
    uint16_t v = 0;
    digitalWrite(pinCs_, LOW);
    delayMicroseconds(2);        // tiempo de establecimiento tras activar CS

    for (int8_t i = 15; i >= 0; i--) {
      digitalWrite(pinSck_, HIGH);
      delayMicroseconds(2);
      if (digitalRead(pinSo_)) v |= (1u << i);
      digitalWrite(pinSck_, LOW);
      delayMicroseconds(2);
    }

    digitalWrite(pinCs_, HIGH);
    return v;
  }

  uint8_t pinSck_ = 0, pinCs_ = 0, pinSo_ = 0;
  uint32_t ultimaMs_ = 0;
  float ultimaTemp_ = 0.0f;
  FallaTermopar falla_ = FallaTermopar::NINGUNA;
};

}  // namespace sensores
