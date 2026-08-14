# Tara y calibración de la báscula

Dos operaciones distintas que suelen confundirse. Se hacen **en este orden** y
solo la primera es rutinaria.

| | Qué fija | Cuándo |
|---|---|---|
| **Tara** | El **cero**: qué lectura corresponde a "vacío" | Al instalar, y cuando el cero derive |
| **Calibración** | La **escala**: cuántas cuentas equivalen a un gramo | Al instalar, y cada 3 meses |

Ambas se guardan en la memoria del equipo y **sobreviven a los cortes de
energía**. No hay que repetirlas en cada arranque.

---

## Cómo funciona por dentro

El HX711 no entrega gramos: entrega un número crudo de 24 bits proporcional a la
deformación de la celda. Convertirlo a peso necesita dos constantes:

```
peso_en_gramos = (lectura_cruda − offset) ÷ factor
                                   ↑          ↑
                                 tara    calibración
```

- El **offset** son las cuentas que da la celda descargada. Incluye el peso del
  plato, la estructura y la tensión mecánica del montaje.
- El **factor** son las cuentas por gramo. Depende de la celda, de la tensión de
  excitación y de la geometría del montaje.

Por eso el orden importa: la calibración se calcula **a partir del cero**. Si
taras después de calibrar, el factor sigue siendo válido; si calibras sin haber
tarado, el factor sale mal.

---

## Tara — procedimiento

### 1. Preparar

- **Báscula completamente vacía.** Nada encima, ni siquiera el recipiente si
  después vas a pesar producto dentro de él.

  > Si siempre pesas el producto dentro de una charola, tara **con la charola
  > puesta**: así el peso que leas es el del producto, no el del conjunto.

- **Sin vibración.** Espera a que se detenga cualquier motor, banda o compresor
  acoplado a la misma estructura.
- **Térmicamente asentada.** Tras encender el equipo, espera **1–2 minutos**. La
  celda deriva mientras el puente se estabiliza.

### 2. Ejecutar

Tres vías equivalentes:

| Vía | Cómo |
|---|---|
| **Puerto serie** | `pio device monitor -b 115200`, escribe `t` |
| **Página local** | `http://monitoreo.local` → botón *Tarar* |
| **Dashboard público** | Botón *Tarar báscula* (pide nombre y PIN de operador) |

Desde el dashboard tarda hasta 30 s: la orden viaja en la respuesta a la
siguiente sincronización del equipo. Verás *"Tara en curso"* mientras tanto.

### 3. Confirmar

```
[bascula] tara aplicada · offset=8421
```

La lectura debe quedar en **±1 g de cero**. Si no, repite.

---

## Si la rechaza

```
[bascula] TARA RECHAZADA: la señal no está estable. Espera a que se asiente y reintenta.
```

**Esto es intencional y conviene entender por qué.** El firmware exige 10
lecturas consecutivas dentro de una banda estrecha antes de aceptar la tara. La
referencia está anclada a la primera lectura, no a la anterior, de modo que
detecta también el **arrastre lento**: una deriva de una cuenta por lectura
parecería estable comparando cada muestra con la previa, pero acumula error.

Tarar sobre una señal que deriva fija un cero equivocado, y ese error se propaga
a **todo el histórico posterior sin dejar rastro de su origen**. Meses después
aparece como "la báscula mide mal" sin ninguna pista de cuándo empezó.

Causas habituales del rechazo:

| Causa | Solución |
|---|---|
| Vibración mecánica | Detén motores y bandas acoplados a la estructura |
| Aún caliente/fría | Espera 2 minutos tras encender |
| Corriente de aire | Cubre la báscula si está bajo un ventilador o extractor |
| Cable de la celda moviéndose | Fíjalo; su peso y tensión afectan la lectura |
| Fuente insuficiente | Se necesita **1 A**; el WiFi pica a 500 mA |

---

## Calibración — procedimiento

Solo hace falta al instalar, tras un cambio mecánico, o en la revisión
trimestral.

### 1. Tarar primero

Con la báscula vacía, como arriba. **No sigas si la tara fue rechazada.**

### 2. Colocar la pesa patrón

Usa un peso conocido **del mismo orden de magnitud que lo que vas a pesar
normalmente**. Calibrar con 100 g una báscula que trabajará a 5 kg amplifica el
error 50 veces.

Colócala **centrada** en el plato.

### 3. Ejecutar

```
cal 1000
```

Donde `1000` es el peso real en gramos de tu patrón.

```
[bascula] calibrada · factor=425.318 cuentas/g
```

### 4. Verificar

| Acción | Lectura esperada |
|---|---|
| Retirar el peso | ~0 g (±1 g) |
| Volver a colocarlo | El valor real (±2 g) |
| Colocarlo descentrado | Mismo valor. Si cambia, el montaje tiene juego |
| Poner el doble de peso | El doble (comprueba la linealidad) |

---

## Cuando algo sale mal

**El peso sale negativo al cargar** — los cables `A+` y `A−` de la celda están
intercambiados. El firmware rechaza un factor negativo justamente para que se
detecte al calibrar y no meses después.

**`CALIBRACIÓN RECHAZADA`** — o la señal está inestable, o el patrón no está
puesto. El firmware exige que la lectura con peso difiera al menos 1000 cuentas
de la tara: por debajo de eso la relación señal/ruido no permite un factor
confiable, y aceptarlo daría una báscula inservible sin avisar.

**El cero deriva con las horas** — normal en cierta medida por temperatura. Si
supera unos gramos al día, revisa el montaje mecánico y el blindaje del cable.
Ninguna cantidad de filtrado corrige una celda mal sujeta.

**Lecturas ruidosas** — cable blindado con la malla a tierra en **un solo
extremo**, lejos de cables de potencia, y condensador de 100 nF junto al módulo.
Ver [`CABLEADO.md`](CABLEADO.md).

---

## Mantenimiento

| Cada | Qué |
|---|---|
| Turno | Verificar el cero con la báscula vacía. Si se desvía, tarar |
| Trimestre | Comprobar con pesa patrón. Si el error supera ±0.5 %, recalibrar |
| Cambio mecánico | Tarar y calibrar de nuevo, siempre |

Cada tara queda registrada con quién la hizo y cuándo, así que el histórico
permite correlacionar un salto en los datos con la intervención que lo causó.
