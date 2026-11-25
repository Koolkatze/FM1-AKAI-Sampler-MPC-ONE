# FM1 AKAI Sampler · MPC One

El **FM1 Sampler** convertido en **plugin instrumento oficial para la Akai MPC One**, listo para la tarjeta SD junto a
los plugins de [LooperX](https://github.com/Koolkatze/AKAI-LooperX-MPC-ONE).

El FM1 Sampler es una app Android con un M-VAVE FM-1 como controlador ([Koolkatze/fm1-sampler](https://github.com/Koolkatze/fm1-sampler)).
Aquí su motor de sonido (`engine/dsp`, versión **0.42.7**) corre dentro de la MPC: seis formas de tocar un sample,
envolvente, LFO, arpegiador, escalas y acordes, color de cinta, reverb y delay. **Suena igual que en el móvil**: las
pruebas comparan el plugin con el motor original muestra a muestra (ver [Fidelidad](#fidelidad-y-cpu)).

- Plugin: [`mpc/FM1Sampler`](mpc/FM1Sampler) · tarjeta SD lista: [`mpc/release/FM1_LooperX_MPC_SD.zip`](mpc/release/FM1_LooperX_MPC_SD.zip)
- Documentación técnica (hilos, código, pruebas, cómo portar versiones nuevas del motor): [`mpc/README.md`](mpc/README.md)
- README de la app Android original: [`README_FM1_ANDROID.md`](README_FM1_ANDROID.md)

> **Estado**: probado con el simulador del SDK de Akai y con las pruebas de este repositorio, **todavía no en una MPC One
> real**. Los plugins de terceros corren dentro del MPC sin protección y un fallo puede colgarlo: guarda el proyecto a
> menudo las primeras veces. Lo que esta guía dice del hardware sale de la documentación del SDK de Akai.

---

## 1. Instalar

1. La MPC necesita **MPC OS 3.10.1 o posterior** (Preferences → Info).
2. **Preferences → Enhancements (Beta)**: activa **Enable Enhancements** y **External Plugin Support**.
3. Descomprime [`FM1_LooperX_MPC_SD.zip`](mpc/release/FM1_LooperX_MPC_SD.zip) y copia **todo** a la raíz de la tarjeta SD
   (o de un USB):
   ```
   <SD>/VST2 Plugins/FM1 - VST - FM1 Sampler/    el plugin, con los 17 samples de fábrica del FM1
   <SD>/VST2 Plugins/LooperX - VST - .../        los 22 plugins de LooperX
   <SD>/FM1 Samples/                             tus WAV para el FM1 (puede tener subcarpetas)
   <SD>/LooperX/Samples · <SD>/LooperX/Loops     carpetas de LooperX (el FM1 también las lee)
   ```
4. Mete la SD o reinicia. En una pista nueva: **Plugin → External Plugins → FM1 → FM1 Sampler**. Suena *Tape Key*.

Para desinstalar, borra la carpeta `FM1 - VST - FM1 Sampler` de `VST2 Plugins`.

## 2. Los controles de la MPC One con este plugin

### Pantalla táctil

- **Pestañas de abajo**: `SAMPLE`, `ENV/LFO`, `ARP`, `TAPE`, `FX`, `KEY` (son las páginas del FM-1; ver la sección 3).
- **Tocar un control** lo selecciona y lleva los Q-Links a su fila (aparece un marco alrededor de esa fila).
- **Doble toque** abre el editor grande (una rueda) o, en las listas largas (Algorithm, Arp Rate, Arp Pattern,
  Delay Sync, Scale, Key Root), un menú con todas las opciones.
- **Interruptores** (Auto Root, LFO Target, LFO Shape, Key Mode, FM-1 Ctrl, Polyphony): un toque.
- Algunos controles **solo aparecen cuando sirven**: Threshold y Kit Note con *Kit/Slice*, Grains y Scatter con *Granular*,
  Stretch y Width con *Paulstretch*, Delay Time con *Delay Sync = Free*. Es lo que hacían K3/K4 en el FM-1.

### Q-Links (los 4 knobs)

El FM-1 tenía 4 knobs por página (K1–K4) y la MPC One tiene 4 Q-Links, así que el plugin está organizado **en filas
de 4 controles**: cada fila es un "K1–K4" y los 4 Q-Links manejan **la fila seleccionada**, en orden.

- **Cambiar de fila**: toca cualquier control de otra fila, o usa el botón **Q-LINK** para pasar de banco
  (fila 1 → 2 → 3 → 4 de la página).
- Al cambiar de pestaña, los Q-Links siguen a la página visible (modo de Q-Links *Screen*).
- Si tienes los Q-Links en modo *Program/Track*, controlan los 16 primeros parámetros del plugin: Volume, Folder, Sample,
  Algorithm, Start, End, Speed, Pitch, Threshold, Kit Note, Grains, Scatter, Stretch, Width, Root Note, Auto Root.

### Rueda de datos

- La **rueda** cambia el control seleccionado; con **SHIFT** el ajuste es fino.
- **Pulsar la rueda** abre el editor grande o el menú del control seleccionado (como el doble toque).
- En las opciones (Algorithm, Scale, Arp Rate…) la rueda va de una en una.

### Pads

- Los 16 pads, sensibles a la velocidad, **tocan el FM1** cuando su pista está seleccionada. La velocidad es el volumen
  de la nota, como en el FM1.
- *Melodic, One-Shot, Looped, Granular, Paulstretch*: cada pad es una nota; con **Root Note** (60 por defecto) el sample
  suena tal como se grabó. **Auto Root** afina al tono que detecta en el sample.
- *Kit/Slice*: 16 pads seguidos tocan los 16 trozos del sample. El primer trozo cae en **Kit Note** (36 = C1 por
  defecto, la nota del primer pad, igual que LooperX Pads). Si tu primer pad suena en otro trozo, cambia Kit Note.
  Cualquier grupo de 16 notas seguidas recorre los 16 trozos, así que los demás bancos de pads también sirven.
- **NOTE REPEAT** y **FULL LEVEL** funcionan como con cualquier instrumento (son notas MIDI).
- Si usas las escalas de pads del propio MPC, deja **Scale = Chrom** en el plugin para no cuantizar dos veces.
- Un plugin no puede encender ni colorear los pads: el estado se ve en la pantalla.

### Transporte y tempo

- **PLAY / STOP**: el arpegiador y *Delay Sync* siguen el tempo del MPC (también TAP TEMPO).
- Al pulsar **STOP** el MPC manda *all notes off*: las notas hacen su release (no se cortan de golpe).
- **REC / OVERDUB** graban las notas en la secuencia como con cualquier instrumento; todos los parámetros se pueden
  automatizar (por ejemplo, mover Start o Stretch con un Q-Link mientras grabas).

### Presets y proyecto

- **12 presets** con los sonidos de fábrica, en el selector de presets del plugin: Tape Key, Acoustic Bass, Shiny Keys,
  Trap Kit, 707 Kit, OP Drums, Glow Grains, Light Paul, Classic Loop, Koto Arp, Fuzz Lo-Fi, Phonk Bass.
- Desde la pantalla de edición del plugin puedes guardar tus propios presets (función del MPC).
- El **proyecto del MPC guarda todo**: los 45 parámetros y la **ruta** del sample. Si luego añades archivos a la carpeta,
  el proyecto sigue encontrando el suyo.

### Del FM-1 a la MPC One

| En el FM-1 | En la MPC One |
|---|---|
| Teclas (27) | Pads (o un teclado MIDI); escala y acordes en la pestaña KEY |
| K1–K4 | Los 4 Q-Links sobre la fila seleccionada |
| HOME, FX, ENV, LFO, ARP, SEQ, EDIT, GLO | Pestañas SAMPLE, FX, ENV/LFO, ARP, TAPE, KEY (Volume y Pan del mezclador están en SAMPLE y KEY) |
| SELECT (pista 1–16) | Pistas del MPC: un FM1 Sampler por pista |
| PRESETS (biblioteca) | Folder y Sample (fila 1 de SAMPLE) |
| ALGORITHM | Algorithm (fila 1 de SAMPLE) |
| OCT − / + | Octave (pestaña KEY) o los bancos de pads |
| PLAY, REC, SAVE | El transporte del MPC y guardar el proyecto |

¿Tienes el FM-1 a mano? Con **FM-1 Ctrl = On** (pestaña KEY) puedes enchufarlo por USB a la MPC y usarlo como antes:
ver [sección 5](#5-el-fm-1-como-controlador-experimental).

## 3. Páginas

Cada fila de la tabla es un grupo de 4 Q-Links.

| Página | Fila 1 | Fila 2 | Fila 3 | Fila 4 |
|---|---|---|---|---|
| **SAMPLE** | Folder · Sample · Algorithm · Volume | Start · End · Speed · Pitch (el HOME del FM-1) | Threshold · Kit Note *(Kit)* · Grains · Scatter *(Granular)* | Stretch · Width *(Paulstretch)* · Root Note · Auto Root |
| **ENV/LFO** | Attack · Decay · Sustain · Release | LFO Rate · LFO Depth · LFO Target (Pitch/Filter/Volume/Pan) · LFO Shape (Sine/Tri/Square/S&H) | | |
| **ARP** | Arp Rate (Off, 1 … 64, tresillos) · Arp Pattern (Up/Down/UpDn/Conv/Rand) · Arp Octaves · Arp Length | | | |
| **TAPE** | Exciter · Lo-Fi · Wobble | | | |
| **FX** | Reverb Send · Reverb Size · Delay Send · Delay Time | Delay Sync (Free, 1/16 … 1/1) · Delay FB | | |
| **KEY** | Scale · Key Root · Octave · Key Mode (Key/Chord) | Pan · Bend Range · FM-1 Ctrl · Polyphony | | |

**Algoritmos** (los seis del FM1):

| Algorithm | Qué hace | Controles propios |
|---|---|---|
| Melodic | Afinado por la nota, con ADSR; toca la ventana Start–End una vez | Speed, Pitch |
| One-Shot | Toca la ventana entera aunque sueltes el pad | Speed, Pitch |
| Looped | Repite la ventana mientras mantienes el pad (con fundido en el bucle) | Speed, Pitch |
| Kit/Slice | Corta el sample en sus golpes (hasta 16); cada pad toca un trozo | Threshold (cuántos golpes), Kit Note, Pitch |
| Granular | Nube de granos cortos que recorre la ventana | Grains (a la vez), Scatter (dispersión) |
| Paulstretch | Estira la ventana hasta un pad infinito (espectral) | Stretch (2×–64×), Width (estéreo) |

**Speed y Pitch** (Melodic, One-Shot, Looped) cambian la duración o el tono de la ventana sin tocar lo otro: el plugin
recalcula una copia estirada (WSOLA, como el FM1) unos 0,3 s después de soltar el control.

**Polifonía** (de la 0.42.7): Melodic, One-Shot y Looped 64 voces, Kit 32, Granular 16, Paulstretch 8. Cuando se
acaban, la nota nueva se queda la voz más antigua. **Polyphony** (pestaña KEY) pone un techo opcional: *FM1* (por defecto),
32, 16 u 8. Si muchos releases largos se solapan y la MPC One va justa, ponlo en 16.

## 4. Samples, y grabar con LooperX

**Folder** recorre las carpetas en este orden: las de fábrica (`1 Bass`, `2 Drums`, `3 Keys`, `4 Pads`) y después, en
la tarjeta donde está el plugin y en el disco interno del MPC:

| Carpeta en la tarjeta | En pantalla |
|---|---|
| `FM1 Samples` (y cada subcarpeta) | `SD:FM1`, `SD:FM1/<subcarpeta>` |
| `LooperX/Loops` (lo que exporta LooperX Looper) | `SD:LX Loops` |
| `LooperX/Samples` | `SD:LX Smp` |
| las mismas en el disco interno | `INT:FM1`, `INT:LX Loops`… |

**Sample** recorre los WAV de la carpeta (8/16/24/32 bits o float, mono o estéreo, hasta 32 s). Las carpetas se releen al
cambiar de Folder o Sample, así que los archivos nuevos aparecen sin reiniciar.

**Grabar**: un instrumento del MPC no recibe audio, así que el FM1 no puede grabar por sí mismo como hacía con el micro
del móvil. Dos caminos:
1. **Con LooperX**: graba con el Looper y pulsa *Export WAV*. El loop aparece en `SD:LX Loops`; elígelo en el FM1 y
   tócalo con Kit/Slice, Granular o Paulstretch.
2. **Con el sampler de la MPC**: graba y guarda el sample en `FM1 Samples` de la tarjeta.

## 5. El FM-1 como controlador (experimental)

Con **FM-1 Ctrl = On**, un M-VAVE FM-1 con el firmware del FM1 Sampler, conectado por USB a la MPC y enrutado a la pista
del plugin, funciona como en la app:

| FM-1 | Plugin |
|---|---|
| Teclas | Notas (ya con la escala del firmware; deja Scale = Chrom) |
| HOME / FX / ENV / LFO / ARP / SEQ / EDIT / GLO | Eligen la página que editan K1–K4 |
| K1–K4 | Los 4 controles de esa página (en HOME, K3/K4 cambian con el algoritmo, como en el FM-1) |
| SELECT · PRESETS · ALGORITHM | Folder · Sample · Algorithm |
| Potenciómetro | Volume |

La pantalla del FM-1 no se actualiza: un plugin del MPC no puede enviar MIDI. Sin probar con hardware.

## 6. Cómo se ha adaptado el FM1 a la MPC

### Funciones

| Función del FM1 Sampler | En el plugin | Por qué |
|---|---|---|
| Sampler: 6 algoritmos, Start/End, Speed/Pitch, Kit, Granular, Paulstretch, autotune | **Igual** (mismo código, salida idéntica) | Es el corazón del FM1 |
| Polifonía de la 0.42.7 (64/32/16/8, robo de la voz más antigua) | **Igual**, más un techo opcional *Polyphony* | La MPC One es más lenta que un móvil |
| ENV, LFO (pitch/filtro/volumen/pan), ARP, escalas y acordes, cinta, reverb y delay de envío | **Igual** | |
| Arpegiador y delay al tempo | Siguen el **tempo del MPC**; el delay suma *Delay Sync* | El reloj es el del MPC |
| Teclas del FM-1 con escala | **Scale / Key Root / Octave / Key Mode** cuantizan las notas de los pads | Los pads mandan notas cromáticas |
| Kit: 16 trozos desde la nota 60 | 16 trozos desde **Kit Note** (36 por defecto) | Para que caigan en los pads |
| Biblioteca (PRESETS) y paquetes | **Folder + Sample** sobre fábrica, `FM1 Samples` y carpetas de LooperX | No hay navegador de archivos propio |
| 16 pistas | **Una instancia por pista** del MPC | Cada pista del MPC es una pista del FM1 |
| Volumen / pan por pista | Volume y Pan en el plugin + mezclador del MPC | |
| Sidechain entre pistas | No (las instancias están aisladas) | Usa el compresor con sidechain del MPC |
| Looper de 16 pistas, escenas, SONG, exportar | **El MPC** (secuencias, song) y **LooperX Looper** | Ya existe en la MPC |
| Secuenciador de 8 pistas, metrónomo, tap tempo | **El MPC** | Ya existe en la MPC |
| Grabar del micro | **Sampler del MPC o LooperX** → `FM1 Samples` / `LooperX/Loops` | Un instrumento no recibe audio |
| Proyectos, backups, recuperación de sesión (0.42.6), NEW PROJECT | **El proyecto del MPC** guarda los 45 parámetros y la ruta del sample; *Init* = preset Tape Key | La MPC ya guarda los plugins en el proyecto |
| Pantalla y LEDs del FM-1, monitor de metrónomo | No | Un plugin no puede enviar MIDI |

### Cambios internos

Solo los necesarios para las reglas del MPC (el hilo de audio no puede reservar memoria, bloquear ni leer archivos) y
para la CPU de la MPC One (un ARM Cortex-A17 de 32 bits, varias veces más lento que un móvil actual):

- **Carga y cálculos pesados fuera del audio**: leer y normalizar el WAV, detectar golpes y tono, cortar los trozos y el
  estiramiento de Speed/Pitch van en un hilo de trabajo. El audio recibe los resultados sin bloqueos.
- **Paulstretch en otro núcleo**: cada paso cuesta cuatro FFT de 4096 puntos (~3 ms en la MPC One, un bloque de audio
  entero). Se calculan en un hilo propio unos pasos por delante y el audio solo los lee.
- **Arpegiador** sin estructuras que reservan memoria. **Granos** con su ventana calculada por recurrencia y cada grano
  en su propio bucle.
- **Saturación** (cinta, delay, salida) con una `tanh` rápida de error < −80 dB, y wow/flutter calculados por bloque.
- **Reverb y delay en reposo**: se paran cuando su cola baja de −100 dB, así una instancia en silencio no gasta CPU.
- **STOP** suelta las notas con su release en vez de cortarlas (evita clics); *all sound off* sí corta.
- **El proyecto guarda la ruta del sample**, no solo su número.

## Fidelidad y CPU

**Fidelidad**: las pruebas renderizan lo mismo con el motor del móvil 0.42.7 sin tocar
([`mpc/tests/reference`](mpc/tests/reference)) y con el plugin, y comparan muestra a muestra:

| Algoritmo | 3 notas | Polifonía completa (70 notas / 40 golpes / 20 notas) |
|---|---|---|
| Melodic, One-Shot, Looped, Kit | idéntico | idéntico (también el robo de voces) |
| Granular | −137 dB (redondeo) | −137 dB |
| Paulstretch | mismo algoritmo; sus fases son aleatorias también en el móvil | |

**CPU** estimada en la MPC One (medida en el simulador × 20, la regla que da Akai; sin medir aún en la máquina):

| Caso | CPU |
|---|---|
| En silencio | ~0,5 % |
| Melodic, 8 notas (igual que el sinte de ejemplo de 8 voces del SDK) | ~9 % |
| Melodic, 64 notas · con Polyphony 16 · con 8 | ~31 % · ~11 % · ~8 % |
| Kit 32 golpes · Looped · Arp | 7–8 % |
| Granular 8 notas · 16 notas · 16 con Polyphony 8 | ~23 % · ~41 % · ~24 % |
| Paulstretch 8 notas | ~19 % + su hilo en otro núcleo |

Solo gastan las voces que suenan: con pocas notas a la vez el techo de polifonía da igual.

## Compilar y probar

```sh
python3 mpc/build_sdcard.py --looperx <LooperX_MPC_Plugins_1.0.0.zip>   # SDK, compilación ARM32/ARM64, validación, simulador y tarjeta
bash mpc/tests/run_tests.sh mpc/.sdk/mpc-plugin-builder-sdk               # pruebas, CPU y fidelidad
```

Detalles en [`mpc/README.md`](mpc/README.md).

## Créditos y licencias

- FM1 Sampler: [Koolkatze/fm1-sampler](https://github.com/Koolkatze/fm1-sampler), fork de
  [ferluht/fm1-sampler](https://github.com/ferluht/fm1-sampler). El repositorio original **no declara licencia**: para uso
  propio no hay problema; antes de distribuir el plugin públicamente conviene pedir permiso a su autor.
- Samples de fábrica: `samples/` de este repositorio. kissfft: BSD-3-Clause.
- SDK de plugins de Akai/inMusic: MIT. "VST" es marca de Steinberg; no se usa código del VST SDK.
- `firmware/` (firmware del FM-1) y `android/` son de la app original y tienen sus propias licencias; no forman parte del plugin.
