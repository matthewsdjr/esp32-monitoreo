// ============================================================================
// Configuración del equipo — SIN SECRETOS
// ============================================================================
// Este archivo SÍ va al repositorio, que es público. Las credenciales de red y
// los tokens se aprovisionan por portal cautivo y viven en NVS (Fase 3).
// ============================================================================

#pragma once
#include <stdint.h>

namespace config {

// ---------------------------------------------------------------------------
// Pines — coinciden con el prototipo original
// ---------------------------------------------------------------------------
constexpr uint8_t PIN_HX711_DT  = 4;
constexpr uint8_t PIN_HX711_SCK = 5;

constexpr uint8_t PIN_I2C_SDA = 21;
constexpr uint8_t PIN_I2C_SCL = 22;

// SPI por software, compartido entre los dos MAX6675. Cada chip pone SO en alta
// impedancia mientras su CS está en alto, así que compartir el bus es correcto
// siempre que nunca se active más de un CS a la vez.
constexpr uint8_t PIN_TERMO_SCK = 18;
constexpr uint8_t PIN_TERMO_SO  = 19;
constexpr uint8_t PIN_TERMO_CS1 = 23;
constexpr uint8_t PIN_TERMO_CS2 = 25;

// ⚠ NIVELES LÓGICOS DEL HX711
// Si el módulo se alimenta a 5 V, su pin DT entrega lógica de 5 V a un GPIO del
// ESP32, que NO es tolerante a 5 V. Puede funcionar meses y luego degradar el
// pin de forma permanente. Alimentar el HX711 a 3.3 V (opera bien en ese rango,
// con ligera pérdida de resolución) o intercalar un divisor resistivo en DT.
// Ver docs/ARQUITECTURA.md §10.

// ---------------------------------------------------------------------------
// Cadencias
// ---------------------------------------------------------------------------
constexpr uint32_t MS_TAREA_BASCULA = 20;    // sondeo del HX711 (10 SPS reales)
constexpr uint32_t MS_TAREA_AMBIENTE = 1000; // SHT31
constexpr uint32_t MS_TAREA_TERMOPAR = 260;  // > 250 ms de conversión del MAX6675
constexpr uint32_t MS_TAREA_AGREGADO = 5000; // una muestra consolidada cada 5 s
constexpr uint32_t MS_DIAGNOSTICO = 2000;    // volcado por consola

// ---------------------------------------------------------------------------
// Rangos físicos por canal
// ---------------------------------------------------------------------------
// Fuera de estos límites la lectura se marca FUERA_RANGO: casi siempre es
// cableado suelto o sensor dañado, no un valor real del proceso.
constexpr float PESO_MIN_G = -100.0f;     // el negativo delata deriva de tara
constexpr float PESO_MAX_G = 50000.0f;    // ajustar al alcance real de la celda

constexpr float TEMP_AMB_MIN_C = -40.0f;  // rango de operación del SHT31
constexpr float TEMP_AMB_MAX_C = 125.0f;

constexpr float HUM_MIN_PCT = 0.0f;
constexpr float HUM_MAX_PCT = 100.0f;

constexpr float TC_MIN_C = 0.0f;          // el MAX6675 no mide bajo cero
constexpr float TC_MAX_C = 1024.0f;       // satura aquí

// ---------------------------------------------------------------------------
// Antigüedad máxima antes de marcar el dato como obsoleto
// ---------------------------------------------------------------------------
// Aproximadamente tres periodos de muestreo: por debajo de eso el jitter normal
// generaría falsos OBSOLETO; por encima, una falla tardaría demasiado en verse.
constexpr uint32_t MS_OBSOLETO_BASCULA  = 2000;
constexpr uint32_t MS_OBSOLETO_AMBIENTE = 4000;
constexpr uint32_t MS_OBSOLETO_TERMOPAR = 1500;

// ---------------------------------------------------------------------------
// Identificación
// ---------------------------------------------------------------------------
constexpr const char* VERSION_FIRMWARE = "2.0.0-fase2";

}  // namespace config
