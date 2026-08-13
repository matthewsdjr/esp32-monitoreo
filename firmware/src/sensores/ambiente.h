// ============================================================================
// SHT31 — temperatura y humedad ambiente, con recuperación
// ============================================================================
// El código original hacía esto:
//
//     if (!sht31.begin(0x44)) { ... while (1) delay(10); }
//
// Un solo sensor caído dejaba muerto el equipo COMPLETO: se perdían también el
// peso y los dos termopares, y desde fuera parecía que el ESP32 se había
// apagado. Un sensor que falla debe degradar su propio canal, nunca tumbar el
// resto del sistema.
//
// Aquí una falla marca el canal y dispara reintentos periódicos de begin(), de
// modo que reconectar el cable I²C recupera el sensor solo, sin ir a planta a
// reiniciar el equipo.
// ============================================================================

#pragma once
#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_SHT31.h>

namespace sensores {

class Ambiente {
 public:
  bool iniciar(uint8_t sda, uint8_t scl, uint8_t direccion = 0x44) {
    sda_ = sda; scl_ = scl; direccion_ = direccion;
    Wire.begin(sda_, scl_);
    Wire.setTimeOut(50);   // sin esto, un bus colgado bloquea la tarea entera
    return intentarBegin();
  }

  /**
   * Lee ambos valores. Devuelve false si el sensor no responde, y en ese caso
   * programa un reintento de inicialización.
   */
  bool leer(float& tempC, float& humPct) {
    if (!iniciado_) {
      reintentarSiTocaBegin();
      return false;
    }

    const float t = sht_.readTemperature();
    const float h = sht_.readHumidity();

    if (isnan(t) || isnan(h)) {
      fallosSeguidos_++;
      // Tres fallos seguidos: se considera perdido y se reinicia el ciclo de
      // begin(). Uno solo puede ser una colisión puntual en el bus.
      if (fallosSeguidos_ >= 3) {
        iniciado_ = false;
        proximoIntentoMs_ = millis() + MS_REINTENTO;
      }
      return false;
    }

    fallosSeguidos_ = 0;
    tempC = t;
    humPct = h;
    return true;
  }

  bool disponible() const { return iniciado_; }
  uint16_t reinicios() const { return reinicios_; }

 private:
  static constexpr uint32_t MS_REINTENTO = 5000;

  bool intentarBegin() {
    // El bus I²C puede quedar colgado si el maestro se reinició a mitad de una
    // transacción y el esclavo sigue tirando de SDA. Reiniciar el periférico
    // antes de begin() recupera ese caso, que de otro modo requiere corte de
    // energía físico.
    Wire.end();
    delay(2);
    Wire.begin(sda_, scl_);
    Wire.setTimeOut(50);

    iniciado_ = sht_.begin(direccion_);
    if (iniciado_) {
      fallosSeguidos_ = 0;
      reinicios_++;
    }
    return iniciado_;
  }

  void reintentarSiTocaBegin() {
    const uint32_t ahora = millis();
    if (ahora < proximoIntentoMs_) return;
    proximoIntentoMs_ = ahora + MS_REINTENTO;
    intentarBegin();
  }

  Adafruit_SHT31 sht_;
  uint8_t sda_ = 21, scl_ = 22, direccion_ = 0x44;
  bool iniciado_ = false;
  uint8_t fallosSeguidos_ = 0;
  uint16_t reinicios_ = 0;
  uint32_t proximoIntentoMs_ = 0;
};

}  // namespace sensores
