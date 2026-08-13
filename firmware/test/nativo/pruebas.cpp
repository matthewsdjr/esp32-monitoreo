// ============================================================================
// Pruebas de la lógica pura del firmware — se ejecutan en el HOST
// ============================================================================
//   cd firmware && make test
//
// Cubren filtros, calibración y máquina de estados: las tres partes donde un
// error produce datos incorrectos sin ningún síntoma visible. No requieren
// ESP32 ni sensores conectados.
// ============================================================================

#include <cstdio>
#include <cmath>
#include <cstring>

#include "../../src/sensores/filtros.h"
#include "../../src/sensores/calibracion.h"
#include "../../src/sensores/canal.h"
#include "../../src/modelo/contrato.h"
#include "../../src/almacen/bufer.h"

static int fallos = 0;
static int total = 0;

static void afirmar(bool cond, const char* msg) {
  total++;
  if (cond) {
    std::printf("  \033[32mok\033[0m   %s\n", msg);
  } else {
    fallos++;
    std::printf("  \033[31mFALLA\033[0m %s\n", msg);
  }
}

static void casi(float a, float b, float tol, const char* msg) {
  afirmar(std::fabs(a - b) <= tol, msg);
}

// ============================================================================
static void pruebasMediana() {
  std::printf("\n── Mediana\n");

  filtros::Mediana<5> m;
  afirmar(std::isnan(m.valor()), "sin muestras devuelve NaN");

  m.agregar(10);
  casi(m.valor(), 10, 0.001f, "con una muestra devuelve esa muestra");

  m.agregar(12);
  m.agregar(11);
  casi(m.valor(), 11, 0.001f, "usa las muestras disponibles antes de llenarse");

  m.agregar(10);
  m.agregar(11);
  afirmar(m.lista(), "la ventana se llena a las 5 muestras");
  casi(m.valor(), 11, 0.001f, "mediana de {10,12,11,10,11} = 11");

  // El caso que justifica usar mediana: un golpe en la estructura.
  filtros::Mediana<5> p;
  for (int i = 0; i < 4; i++) p.agregar(100.0f);
  p.agregar(9000.0f);   // pico enorme
  casi(p.valor(), 100.0f, 0.001f,
       "un pico de 9000 sobre 100 NO desplaza la mediana");

  // Contraste con una media, para dejar clara la razón del orden del filtrado.
  float media = (100 * 4 + 9000) / 5.0f;
  afirmar(media > 1800.0f, "la MEDIA sí se dispara con ese pico (por eso va después)");

  // Dos valores extremos ya sí desplazan la mediana: N=5 tolera hasta 2.
  filtros::Mediana<5> q;
  q.agregar(100); q.agregar(100); q.agregar(100);
  q.agregar(9000); q.agregar(9000);
  casi(q.valor(), 100.0f, 0.001f, "N=5 tolera hasta 2 valores extremos");

  filtros::Mediana<5> r;
  r.agregar(100); r.agregar(100);
  r.agregar(9000); r.agregar(9000); r.agregar(9000);
  casi(r.valor(), 9000.0f, 0.001f, "con 3 de 5 extremos, la mediana los sigue (correcto)");

  m.reiniciar();
  afirmar(!m.lista() && m.muestras() == 0, "reiniciar vacía la ventana");
}

// ============================================================================
static void pruebasEma() {
  std::printf("\n── Media exponencial\n");

  filtros::MediaExponencial e(0.5f);
  afirmar(std::isnan(e.valor()), "sin muestras devuelve NaN");

  // Arrancar en la primera muestra y no en cero: si empezara en cero, una
  // báscula con 1200 g mostraría una rampa artificial durante varios segundos.
  e.agregar(1000.0f);
  casi(e.valor(), 1000.0f, 0.001f, "se inicializa con la primera muestra, no en cero");

  e.agregar(2000.0f);
  casi(e.valor(), 1500.0f, 0.001f, "alfa=0.5 promedia a medio camino");

  filtros::MediaExponencial lento(0.1f);
  lento.agregar(0.0f);
  for (int i = 0; i < 50; i++) lento.agregar(100.0f);
  casi(lento.valor(), 100.0f, 1.0f, "converge al valor sostenido");

  filtros::MediaExponencial rapido(0.9f);
  rapido.agregar(0.0f);
  rapido.agregar(100.0f);
  afirmar(rapido.valor() > 85.0f, "alfa alto responde rápido");

  lento.reiniciar();
  afirmar(!lento.inicializada(), "reiniciar borra el estado");
}

// ============================================================================
static void pruebasEstabilidad() {
  std::printf("\n── Detector de estabilidad\n");

  filtros::Estabilidad est(2.0f, 5);
  afirmar(!est.estable(), "arranca inestable");

  for (int i = 0; i < 5; i++) est.agregar(100.0f);
  afirmar(est.estable(), "5 lecturas iguales -> estable");

  est.agregar(500.0f);
  afirmar(!est.estable(), "un salto grande rompe la estabilidad");

  for (int i = 0; i < 4; i++) est.agregar(500.0f);
  afirmar(est.estable(), "vuelve a ser estable en el nivel nuevo");

  // La referencia está ANCLADA a la primera lectura, no a la anterior. Esa
  // elección es la que detecta el arrastre lento: comparando contra la muestra
  // previa, una rampa de 0.3 por lectura parecería estable indefinidamente
  // porque cada paso individual es diminuto. Tarar sobre ese arrastre fija un
  // cero equivocado que después contamina todo el histórico.
  filtros::Estabilidad arrastre(2.0f, 5);
  float v = 100.0f;
  for (int i = 0; i < 10; i++) { arrastre.agregar(v); v += 0.3f; }
  afirmar(!arrastre.estable(),
          "un arrastre acumulado de 2.7 con tolerancia 2.0 NO es estable");

  // Ruido acotado alrededor de un mismo nivel sí debe considerarse estable.
  filtros::Estabilidad ruido(2.0f, 5);
  const float zigzag[] = {100.0f, 101.2f, 99.1f, 100.8f, 99.5f, 100.3f, 101.0f};
  for (float x : zigzag) ruido.agregar(x);
  afirmar(ruido.estable(),
          "ruido de ±1.2 alrededor de un nivel fijo SÍ es estable");
}

// ============================================================================
static void pruebasCalibracion() {
  std::printf("\n── Calibración de la báscula\n");

  sensores::Calibracion c(0, 420.0f);
  casi(c.aGramos(42000), 100.0f, 0.01f, "42000 cuentas / 420 = 100 g");

  c.tarar(8000);
  afirmar(c.offset() == 8000, "tarar fija el offset");
  casi(c.aGramos(8000), 0.0f, 0.01f, "tras tarar, las cuentas de tara dan 0 g");
  casi(c.aGramos(50000), 100.0f, 0.01f, "el offset se descuenta correctamente");

  // Peso negativo: NO se recorta a cero. Ver deriva de tara es información útil.
  afirmar(c.aGramos(4000) < 0.0f, "un peso por debajo de la tara sale negativo");

  // Calibración con patrón
  sensores::Calibracion k(1000, 1.0f);
  afirmar(k.calibrar(101000, 1000.0f), "calibrar con patrón de 1000 g");
  casi(k.factor(), 100.0f, 0.01f, "factor = (101000-1000)/1000 = 100");
  casi(k.aGramos(101000), 1000.0f, 0.1f, "tras calibrar, el patrón lee su peso");

  // Casos que deben rechazarse
  sensores::Calibracion m(1000, 420.0f);
  afirmar(!m.calibrar(1050, 1000.0f),
          "rechaza calibrar si no se colocó el peso (delta minúsculo)");
  casi(m.factor(), 420.0f, 0.01f, "un rechazo NO altera el factor previo");

  afirmar(!m.calibrar(500000, 0.0f), "rechaza un peso patrón de cero");
  afirmar(!m.calibrar(500000, -50.0f), "rechaza un peso patrón negativo");

  // Celda cableada al revés: el delta va en sentido contrario.
  sensores::Calibracion inv(1000, 420.0f);
  afirmar(!inv.calibrar(-99000, 1000.0f),
          "rechaza factor negativo (celda invertida: hay que corregir cableado)");

  // Factor inválido
  sensores::Calibracion z;
  z.fijarFactor(0.0f);
  afirmar(z.factor() > 0.0f, "no acepta un factor de cero");
  z.fijarFactor(-5.0f);
  afirmar(z.factor() > 0.0f, "no acepta un factor negativo");
}

// ============================================================================
static void pruebasCanal() {
  std::printf("\n── Máquina de estados del canal\n");

  using sensores::Canal;
  using sensores::Estado;

  Canal tc;
  tc.configurar(contrato::Canal::TC1, {0.0f, 1024.0f, 1000});

  afirmar(tc.estado() == Estado::SIN_DATO, "arranca SIN_DATO");
  afirmar(std::isnan(tc.valorPublicable()), "sin dato publica NaN");

  tc.publicar(180.0f, 1000);
  afirmar(tc.estado() == Estado::OK, "una lectura válida pasa a OK");
  casi(tc.valorPublicable(), 180.0f, 0.01f, "publica el valor");
  afirmar(tc.bitSiFalla() == 0, "en OK no aporta bit de falla");

  // Envejecimiento
  tc.envejecer(1500);
  afirmar(tc.estado() == Estado::OK, "a 500 ms sigue OK");
  tc.envejecer(2500);
  afirmar(tc.estado() == Estado::OBSOLETO, "a 1500 ms pasa a OBSOLETO");
  afirmar(std::isnan(tc.valorPublicable()), "un dato obsoleto NO se publica");
  afirmar(tc.bitSiFalla() == contrato::bitFalla(contrato::Canal::TC1),
          "OBSOLETO aporta el bit de falla del canal");

  tc.publicar(185.0f, 3000);
  afirmar(tc.estado() == Estado::OK, "una lectura nueva lo recupera");

  // Fuera del rango físico
  tc.publicar(3000.0f, 3500);
  afirmar(tc.estado() == Estado::FUERA_RANGO,
          "3000 °C en un MAX6675 que satura en 1024 es FUERA_RANGO");
  afirmar(std::isnan(tc.valorPublicable()), "fuera de rango no se publica");
  casi(tc.valorCrudo(), 3000.0f, 0.01f, "el crudo se conserva para diagnóstico");

  // Falla explícita
  tc.publicarFalla(4000);
  afirmar(tc.estado() == Estado::FALLA, "publicarFalla marca FALLA");

  // Una FALLA no debe degradarse a OBSOLETO: es información más precisa.
  tc.envejecer(999999);
  afirmar(tc.estado() == Estado::FALLA,
          "una FALLA no se degrada a OBSOLETO por envejecer");

  // Desactivado
  Canal off;
  off.configurar(contrato::Canal::HUM, {0.0f, 100.0f, 1000}, false);
  afirmar(off.estado() == Estado::DESACTIVADO, "se puede nacer desactivado");
  off.publicar(50.0f, 100);
  afirmar(off.estado() == Estado::DESACTIVADO, "un canal desactivado ignora lecturas");
  afirmar(off.bitSiFalla() == 0, "un canal desactivado NO aporta bit de falla");

  off.activar(true);
  afirmar(off.estado() == Estado::SIN_DATO, "al activarlo vuelve a SIN_DATO");

  // Límite exacto del rango: debe aceptarse, no rechazarse.
  Canal h;
  h.configurar(contrato::Canal::HUM, {0.0f, 100.0f, 1000});
  h.publicar(100.0f, 10);
  afirmar(h.estado() == Estado::OK, "el extremo superior del rango es válido");
  h.publicar(0.0f, 20);
  afirmar(h.estado() == Estado::OK, "el extremo inferior del rango es válido");
  h.publicar(100.01f, 30);
  afirmar(h.estado() == Estado::FUERA_RANGO, "apenas por encima ya es fuera de rango");
}

// ============================================================================
static void pruebasContrato() {
  std::printf("\n── Contrato con el backend\n");

  // Estos valores están replicados en docs/API.md, en el backend y en el
  // dashboard. Si alguien los cambia aquí sin cambiarlos allá, las fallas se
  // atribuyen al sensor equivocado.
  afirmar(contrato::bitFalla(contrato::Canal::PESO) == 1,     "peso     -> bit 0 (1)");
  afirmar(contrato::bitFalla(contrato::Canal::TEMP_AMB) == 2, "temp_amb -> bit 1 (2)");
  afirmar(contrato::bitFalla(contrato::Canal::HUM) == 4,      "hum      -> bit 2 (4)");
  afirmar(contrato::bitFalla(contrato::Canal::TC1) == 8,      "tc1      -> bit 3 (8)");
  afirmar(contrato::bitFalla(contrato::Canal::TC2) == 16,     "tc2      -> bit 4 (16)");

  afirmar(contrato::NUM_CANALES == 5, "son 5 canales");

  afirmar(std::strcmp(contrato::clave(contrato::Canal::PESO), "peso_g") == 0,
          "la clave JSON del peso es peso_g");
  afirmar(std::strcmp(contrato::slug(contrato::Canal::TC1), "tc1") == 0,
          "el slug del termopar 1 es tc1");

  // Máscara combinada, como en el ejemplo de docs/API.md §1
  uint16_t ambos = contrato::bitFalla(contrato::Canal::TC1) |
                   contrato::bitFalla(contrato::Canal::TC2);
  afirmar(ambos == 24, "ambos termopares en falla -> faults = 24");

  // Una muestra recién construida no tiene valores válidos.
  contrato::Muestra m;
  afirmar(std::isnan(m.valor[0]), "una muestra nueva arranca con NaN");
  afirmar(m.faults == 0, "una muestra nueva arranca sin fallas");
}

// ============================================================================
static void pruebasCadenaCompleta() {
  std::printf("\n── Cadena completa: crudo -> filtros -> gramos -> estado\n");

  sensores::Calibracion cal(8000, 420.0f);
  filtros::Mediana<5> med;
  filtros::MediaExponencial ema(0.3f);
  sensores::Canal canal;
  canal.configurar(contrato::Canal::PESO, {-100.0f, 50000.0f, 1000});

  // 1200 g reales con ruido de ±3 cuentas y un golpe en la muestra 7
  const int32_t crudoBase = 8000 + static_cast<int32_t>(1200.0f * 420.0f);
  uint32_t t = 0;
  for (int i = 0; i < 20; i++) {
    int32_t crudo = crudoBase + ((i % 3) - 1) * 3;
    if (i == 7) crudo += 900000;              // golpe en la estructura
    med.agregar(static_cast<float>(crudo));
    float suave = ema.agregar(med.valor());
    canal.publicar(cal.aGramos(static_cast<int32_t>(suave)), t);
    t += 100;
  }

  afirmar(canal.estado() == sensores::Estado::OK, "la cadena termina en OK");
  casi(canal.valorPublicable(), 1200.0f, 1.0f,
       "el golpe de 900 000 cuentas no contamina el resultado (±1 g)");

  // Sin la mediana, el mismo golpe sí contamina: es la razón del orden.
  filtros::MediaExponencial sinMediana(0.3f);
  for (int i = 0; i < 20; i++) {
    int32_t crudo = crudoBase + ((i % 3) - 1) * 3;
    if (i == 7) crudo += 900000;
    sinMediana.agregar(static_cast<float>(crudo));
  }
  float pesoSinMediana = cal.aGramos(static_cast<int32_t>(sinMediana.valor()));

  const float errorConMediana = std::fabs(canal.valorPublicable() - 1200.0f);
  const float errorSinMediana = std::fabs(pesoSinMediana - 1200.0f);

  std::printf("       error con mediana: %.2f g · sin mediana: %.2f g\n",
              errorConMediana, errorSinMediana);

  // 12 muestras después del golpe, la EMA con alfa=0.3 todavía arrastra
  // 0.7^12 ≈ 1.4 % del pico: unos 9 g sobre una lectura de 1200 g. No es
  // catastrófico, pero es un 0.75 % de error introducido por UN solo golpe,
  // y en una báscula de inocuidad eso es inaceptable.
  afirmar(errorSinMediana > 5.0f,
          "sin mediana, un solo golpe deja error residual de varios gramos");
  afirmar(errorConMediana < errorSinMediana / 5.0f,
          "la mediana reduce ese error al menos 5 veces (justifica el orden)");
}


// ============================================================================
static void pruebasAnillo() {
  std::printf("\n── Búfer offline (anillo en RAM)\n");

  auto muestra = [](uint32_t t) {
    almacen::Pendiente p{};
    p.marcaMs = t;
    for (int i = 0; i < contrato::NUM_CANALES; i++) p.valor[i] = (float)t;
    p.faults = 0;
    return p;
  };

  almacen::AnilloRam<5> a;
  afirmar(a.vacio(), "arranca vacío");

  for (uint32_t i = 1; i <= 3; i++) a.agregar(muestra(i));
  afirmar(a.tamano() == 3, "guarda 3 muestras");

  // asomar NO retira: descartar antes de confirmar la entrega es la forma
  // clásica de perder datos cuando la respuesta se pierde en un timeout.
  almacen::Pendiente salida[10];
  size_t n = a.asomar(salida, 10);
  afirmar(n == 3, "asomar devuelve las 3");
  afirmar(a.tamano() == 3, "asomar NO las retira");
  afirmar(salida[0].marcaMs == 1 && salida[2].marcaMs == 3,
          "salen en orden FIFO: la más antigua primero");

  a.descartarFrente(2);
  afirmar(a.tamano() == 1, "descartarFrente retira solo las confirmadas");
  a.asomar(salida, 10);
  afirmar(salida[0].marcaMs == 3, "queda la más reciente sin confirmar");

  // Desbordamiento
  almacen::AnilloRam<5> b;
  for (uint32_t i = 1; i <= 8; i++) b.agregar(muestra(i));
  afirmar(b.tamano() == 5, "no crece más allá de su capacidad");
  afirmar(b.descartadas() == 3, "contabiliza las descartadas");
  b.asomar(salida, 10);
  afirmar(salida[0].marcaMs == 4 && salida[4].marcaMs == 8,
          "al desbordarse conserva las MÁS RECIENTES (4..8)");

  // Vuelta completa del índice: el caso donde fallan las implementaciones
  // ingenuas, porque inicio y fin cruzan el final del arreglo.
  almacen::AnilloRam<4> c;
  for (uint32_t i = 1; i <= 3; i++) c.agregar(muestra(i));
  c.descartarFrente(2);            // inicio queda en 2
  for (uint32_t i = 4; i <= 6; i++) c.agregar(muestra(i));  // fin da la vuelta
  afirmar(c.tamano() == 4, "tamaño correcto tras dar la vuelta");
  c.asomar(salida, 10);
  afirmar(salida[0].marcaMs == 3 && salida[1].marcaMs == 4 &&
          salida[2].marcaMs == 5 && salida[3].marcaMs == 6,
          "el orden se mantiene aunque el índice dé la vuelta");

  // Descartar más de lo que hay no debe corromper el estado.
  c.descartarFrente(99);
  afirmar(c.vacio(), "descartar de más deja el anillo vacío, no corrupto");
  c.agregar(muestra(77));
  c.asomar(salida, 10);
  afirmar(c.tamano() == 1 && salida[0].marcaMs == 77,
          "sigue usable tras un descarte excesivo");

  // Escenario del criterio de aceptación: 30 min sin red a una muestra cada 5 s
  // son 360 muestras. Con 720 posiciones, no se pierde ninguna.
  almacen::AnilloRam<720> caida;
  for (uint32_t i = 0; i < 360; i++) caida.agregar(muestra(i));
  afirmar(caida.descartadas() == 0,
          "una caída de 30 min no descarta ninguna muestra");
  afirmar(caida.tamano() == 360, "las 360 quedan disponibles para el drenado");
}

// ============================================================================
int main() {
  std::printf("\n=== Pruebas de la lógica del firmware ===\n");

  pruebasMediana();
  pruebasEma();
  pruebasEstabilidad();
  pruebasCalibracion();
  pruebasCanal();
  pruebasContrato();
  pruebasCadenaCompleta();
  pruebasAnillo();

  std::printf("\n%s%d/%d aserciones pasaron\033[0m\n\n",
              fallos == 0 ? "\033[32m" : "\033[31m", total - fallos, total);
  return fallos == 0 ? 0 : 1;
}
