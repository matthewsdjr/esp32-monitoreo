// ============================================================================
// Persistencia en NVS
// ============================================================================
// Sin esto, cada corte de energía obliga a recalibrar la báscula. En la práctica
// eso significa que nadie recalibra y la planta opera con una báscula fuera de
// ajuste durante meses.
//
// Aquí NO van credenciales de red: esas las gestiona el portal cautivo en su
// propio espacio de nombres (Fase 3), para que borrar la calibración no borre
// el WiFi ni al revés.
// ============================================================================

#pragma once
#include <Arduino.h>
#include <Preferences.h>

namespace almacen {

struct AjustesBascula {
  int32_t offset;   // cuentas con la celda descargada
  float factor;     // cuentas por gramo
  uint32_t tarasRealizadas;
  uint32_t ultimaTaraEpoch;   // 0 si nunca, o si se taró sin hora sincronizada
};

class Ajustes {
 public:
  void iniciar() { prefs_.begin("monitoreo", false); }

  AjustesBascula leerBascula() {
    AjustesBascula a;
    a.offset = prefs_.getInt("bas_off", 0);
    a.factor = prefs_.getFloat("bas_fac", 420.0f);
    a.tarasRealizadas = prefs_.getUInt("bas_ntara", 0);
    a.ultimaTaraEpoch = prefs_.getUInt("bas_ttara", 0);

    // Un factor inválido guardado (por corrupción o por una versión anterior con
    // un error) produciría división entre cero en cada lectura. Se corrige aquí
    // en vez de propagarlo a todo el sistema.
    if (!(a.factor > 1e-6f)) a.factor = 420.0f;
    return a;
  }

  void guardarBascula(const AjustesBascula& a) {
    prefs_.putInt("bas_off", a.offset);
    prefs_.putFloat("bas_fac", a.factor);
    prefs_.putUInt("bas_ntara", a.tarasRealizadas);
    prefs_.putUInt("bas_ttara", a.ultimaTaraEpoch);
  }

  /**
   * Solo el offset. Se usa tras una tara.
   *
   * La NVS del ESP32 soporta del orden de 100 000 ciclos de borrado por sector:
   * escribir únicamente lo que cambió, y no el bloque completo, alarga
   * notablemente la vida de la memoria en un equipo que tara varias veces al día.
   */
  void guardarOffset(int32_t offset, uint32_t epoch) {
    prefs_.putInt("bas_off", offset);
    prefs_.putUInt("bas_ntara", prefs_.getUInt("bas_ntara", 0) + 1);
    if (epoch > 0) prefs_.putUInt("bas_ttara", epoch);
  }

  void guardarFactor(float factor) {
    if (factor > 1e-6f) prefs_.putFloat("bas_fac", factor);
  }

  /** Identificador estable del equipo, para diagnóstico. */
  String slugEquipo() { return prefs_.getString("dev_slug", "planta-01"); }
  void fijarSlugEquipo(const String& s) { prefs_.putString("dev_slug", s); }

  /** Contador de arranques: delata reinicios inesperados en operación. */
  uint32_t registrarArranque() {
    uint32_t n = prefs_.getUInt("arranques", 0) + 1;
    prefs_.putUInt("arranques", n);
    return n;
  }

  void borrarTodo() { prefs_.clear(); }

 private:
  Preferences prefs_;
};

}  // namespace almacen
