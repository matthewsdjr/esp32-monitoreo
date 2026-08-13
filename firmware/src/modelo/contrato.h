// ============================================================================
// Contrato de datos — DEBE coincidir con docs/API.md
// ============================================================================
// Este archivo es el lado del firmware del contrato congelado en la Fase 1.
// Sus tres copias tienen que moverse juntas:
//
//   supabase/functions/_shared/contrato.ts   (backend)
//   firmware/src/modelo/contrato.h           (este archivo)
//   web/src/tipos.ts                         (dashboard)
//
// Cambiar BIT_FALLA aquí sin cambiarlo allá hace que las fallas se atribuyan al
// sensor equivocado: el dashboard mostraría "termopar 1 desconectado" cuando en
// realidad se cayó la báscula.
// ============================================================================

#pragma once
#include <stdint.h>

namespace contrato {

/** Canales del equipo. El orden fija el bit que ocupa cada uno en `faults`. */
enum class Canal : uint8_t {
  PESO = 0,
  TEMP_AMB = 1,
  HUM = 2,
  TC1 = 3,
  TC2 = 4,
  _TOTAL = 5
};

constexpr uint8_t NUM_CANALES = static_cast<uint8_t>(Canal::_TOTAL);

/** Bit que cada canal ocupa en la máscara `faults`. */
constexpr uint16_t bitFalla(Canal c) {
  return static_cast<uint16_t>(1u << static_cast<uint8_t>(c));
}

/** Nombre corto del canal, tal como lo espera el backend. */
inline const char* slug(Canal c) {
  switch (c) {
    case Canal::PESO:     return "peso";
    case Canal::TEMP_AMB: return "temp_amb";
    case Canal::HUM:      return "hum";
    case Canal::TC1:      return "tc1";
    case Canal::TC2:      return "tc2";
    default:              return "?";
  }
}

/** Clave JSON del valor de cada canal en el payload. */
inline const char* clave(Canal c) {
  switch (c) {
    case Canal::PESO:     return "peso_g";
    case Canal::TEMP_AMB: return "temp_amb_c";
    case Canal::HUM:      return "hum_pct";
    case Canal::TC1:      return "tc1_c";
    case Canal::TC2:      return "tc2_c";
    default:              return "?";
  }
}

/** Una muestra instantánea de todos los canales. */
struct Muestra {
  int64_t  tsMs;                      // época en ms (UTC), o 0 si aún no hay NTP
  float    valor[NUM_CANALES];        // NaN cuando el canal no tiene dato válido
  uint16_t faults;                    // máscara de bits por canal

  Muestra() : tsMs(0), faults(0) {
    for (uint8_t i = 0; i < NUM_CANALES; i++) valor[i] = 0.0f / 0.0f; // NaN
  }
};

/** Comandos que el equipo puede recibir. Ver docs/API.md §6. */
enum class Comando : uint8_t { TARA, CALIBRAR, REINICIAR, RECARGAR_UMBRALES, DESCONOCIDO };

inline Comando comandoDesde(const char* s) {
  if (!s) return Comando::DESCONOCIDO;
  if (!__builtin_strcmp(s, "tara"))              return Comando::TARA;
  if (!__builtin_strcmp(s, "calibrar"))          return Comando::CALIBRAR;
  if (!__builtin_strcmp(s, "reiniciar"))         return Comando::REINICIAR;
  if (!__builtin_strcmp(s, "recargar_umbrales")) return Comando::RECARGAR_UMBRALES;
  return Comando::DESCONOCIDO;
}

}  // namespace contrato
