# FM1 Sampler para Akai MPC One

El motor de sampler del **FM1 Sampler** (la app Android de este repositorio, `engine/`) convertido en un
**plugin instrumento oficial para MPC**, hecho con el SDK de plugins de Akai/inMusic
([`mpc-plugin-builder-sdk`](https://github.com/inmusic-software/mpc-plugin-builder-sdk)). Va en la tarjeta SD junto a
los plugins de [LooperX](https://github.com/Koolkatze/AKAI-LooperX-MPC-ONE).

Cada instancia del plugin es **una pista del FM1**: un sample tocado con los 6 algoritmos del FM1, su envolvente, LFO,
arpegiador, escalas/acordes, color de cinta y sus dos efectos de envío. Varias instancias en varias pistas del MPC
equivalen a las pistas del FM1; lo que la app hacía alrededor del motor (secuenciador, looper, escenas, SONG,
metrónomo, sampleo) lo hace el propio MPC.

**Sonido idéntico al del móvil**: las pruebas de este repo comparan el port con el `engine/dsp` original muestra a muestra:
Melodic, One-Shot, Looped y Kit dan exactamente la misma salida, también a polifonía completa con robo de voces;
Granular difiere a −137 dB (redondeo de coma flotante). Paulstretch es el mismo algoritmo (fases aleatorias, así que
nunca hay dos salidas iguales ni en el móvil). Basado en el `engine/dsp` de **FM1 Sampler 0.42.7**.

## Instalar

Requisitos: **MPC OS 3.10.1 o posterior** y, en el MPC, **Preferences → Enhancements (Beta) → Enable Enhancements** y
**External Plugin Support** activados.

1. Descomprime `FM1_LooperX_MPC_SD.zip` y copia su contenido a la **raíz** de la tarjeta SD (o de un USB):
   ```
   <SD>/VST2 Plugins/FM1 - VST - FM1 Sampler/      el plugin (con sus 17 samples de fábrica)
   <SD>/VST2 Plugins/LooperX - VST - .../          los 22 plugins de LooperX
   <SD>/FM1 Samples/                               tus WAV para el FM1 (subcarpetas permitidas)
   <SD>/LooperX/Samples, <SD>/LooperX/Loops        carpetas de LooperX
   ```
2. Mete la SD (o reinicia el MPC). En una pista nueva: **Plugin → External Plugins → FM1 → FM1 Sampler**.

> Los plugins de terceros corren dentro del MPC sin protección: un fallo puede colgarlo. Este plugin está probado con el
> simulador del SDK y con las pruebas de este repo, **todavía no en una MPC real**. Guarda el proyecto a menudo.
> Para quitarlo, borra su carpeta de `VST2 Plugins`.

## Páginas

Los Q-Links van **por filas de cuatro**: cada fila es una página K1–K4 del FM-1, justo los 4 Q-Links de la MPC One.

| Página | Fila 1 | Fila 2 | Fila 3 | Fila 4 |
|---|---|---|---|---|
| **SAMPLE** | Folder, Sample, Algorithm, Volume | Start, End, Speed, Pitch (= HOME del FM1) | Threshold, Kit Note (Kit) · Grains, Scatter (Granular) | Stretch, Width (Paulstretch) · Root Note, Auto Root |
| **ENV/LFO** | Attack, Decay, Sustain, Release | LFO Rate, Depth, Target (Pitch/Filter/Volume/Pan), Shape | | |
| **ARP** | Rate (Off, 1 … 64, tresillos), Pattern (Up/Down/UpDn/Conv/Rand), Octaves, Length | | | |
| **TAPE** | Exciter, Lo-Fi, Wobble (= página SEQ/cinta del FM1) | | | |
| **FX** | Reverb Send, Reverb Size, Delay Send, Delay Time | Delay Sync (Free o 1/16 … 1/1 al tempo del MPC), Delay FB | | |
| **KEY** | Scale, Key Root, Octave, Key Mode (Key/Chord) (= GLO del FM1) | Pan, Bend Range, FM-1 Ctrl, Polyphony | | |

Los controles de cada algoritmo solo aparecen con ese algoritmo, como los K3/K4 del FM1.

**Algoritmos**: *Melodic* (afinado por la nota, ADSR), *One-Shot* (toca la ventana entera), *Looped* (repite la ventana
mientras mantienes), *Kit/Slice* (corta el sample en 16 golpes; 16 pads seguidos desde **Kit Note** tocan los 16 trozos;
**Threshold** decide cuántos golpes detecta), *Granular* (nube de granos; **Grains** a la vez, **Scatter** cuánto se
dispersan), *Paulstretch* (estira la ventana hasta un pad infinito; **Stretch** 2×–64×, **Width** estéreo).

**Speed / Pitch** (Melodic, One-Shot, Looped) hacen como en el FM1 un estiramiento offline WSOLA de la ventana
(cambia duración o tono sin tocar lo otro); se calcula en segundo plano unos 0,3 s después de soltar el control.
**Auto Root** afina las notas al tono detectado del sample (YIN), como el autotune del FM1.

**Polifonía** (0.42.7): Melodic, One-Shot y Looped 64 voces, Kit 32, Granular 16, Paulstretch 8. Una nota nueva toma
la primera voz libre y, si no hay, la más antigua, como en el móvil. **Polyphony** (página KEY) pone un techo opcional por
debajo de esos límites: *FM1* (por defecto, los del móvil), 32, 16 u 8. Sirve si muchos releases largos se solapan y la
MPC One va justa de CPU; 16 es un buen valor en ese caso.

**12 presets** con los sonidos de fábrica: Tape Key, Acoustic Bass, Shiny Keys, Trap Kit, 707 Kit, OP Drums, Glow Grains,
Light Paul, Classic Loop, Koto Arp, Fuzz Lo-Fi, Phonk Bass.

## Samples

**Folder** recorre las carpetas en este orden: las de fábrica (Bass, Drums, Keys, Pads) y luego, en la unidad del plugin
y en el disco interno del MPC, `FM1 Samples` (y sus subcarpetas), `LooperX/Loops` y `LooperX/Samples`
(en pantalla `SD:FM1`, `SD:LX Loops`, `INT:FM1`…). **Sample** recorre los WAV de esa carpeta. Las carpetas se releen al
cambiar de Folder/Sample. WAV de 8/16/24/32 bits o float, mono o estéreo, hasta 32 s (más largo se corta).

El proyecto del MPC guarda **la ruta** del sample, no solo su número: si añades archivos a la carpeta, el proyecto
sigue encontrando el suyo.

### Grabar con LooperX y tocarlo con el FM1

Un instrumento del MPC no recibe audio, así que el FM1 no puede grabar por sí mismo (en el móvil grababa el micro).
Con LooperX sí: graba con el **Looper**, usa **Export WAV** y los loops aparecen en `LooperX/Loops`; elígelos en el FM1
(Folder `SD:LX Loops`) y tócalos con Kit/Slice, Granular o Paulstretch. También puedes guardar cualquier sample del
MPC en `FM1 Samples`.

## FM-1 como controlador (experimental)

Con **FM-1 Ctrl = On**, un M-VAVE FM-1 con el firmware del FM1 Sampler conectado al USB de la MPC y enrutado a la pista
controla el plugin como controlaba la app: teclas = notas (ya con la escala del firmware), HOME/FX/ENV/LFO/ARP/SEQ/EDIT/GLO
cambian de página, K1–K4 editan esa página, SELECT = Folder, PRESETS = Sample, ALGORITHM = algoritmo, el potenciómetro =
volumen. La pantalla del FM-1 no se actualiza (un plugin del MPC no puede enviar MIDI). No probado en hardware.

## Qué no se ha portado, y por qué

| Del FM1 Sampler | En la MPC |
|---|---|
| Looper de 16 pistas, escenas, SONG, exportar | Pistas, secuencias y song del MPC, y LooperX Looper |
| Secuenciador de 8 pistas, metrónomo, tap tempo | Los del MPC (el arpegiador y el delay siguen el tempo del MPC) |
| Grabar del micro | Un instrumento no recibe audio: sampler del MPC o LooperX, y luego `FM1 Samples` / `LooperX/Loops` |
| Proyectos, backups, recuperación de sesión (0.42.6) | El proyecto del MPC guarda el estado del plugin (todos los parámetros + ruta del sample) |
| Sidechain entre pistas | Cada instancia está aislada (usa el compresor con sidechain del MPC) |
| Pantalla y LEDs del FM-1 | Un plugin del MPC no puede enviar MIDI |

## Diferencias técnicas con el motor del móvil

Solo las necesarias para el contrato del host del MPC (el hilo de audio no puede reservar memoria, bloquear ni leer
archivos) y para la CPU de la MPC One (Cortex-A17 de 32 bits, mucho más lenta que un móvil actual):

- Carga de WAV, normalización, detección de golpes y de tono, slicing y el WSOLA de Speed/Pitch: en un hilo de trabajo;
  el audio recibe los resultados por buzones sin bloqueos y devuelve los objetos viejos para borrarlos fuera del audio.
- Paulstretch: las FFT (4 de 4096 puntos por salto, ~3 ms en un A17) se calculan en un hilo propio, en otro núcleo,
  unos saltos por delante; el audio solo lee y remuestrea. Un buffer no se libera hasta que ese hilo confirma que ya no lo lee.
- Arpegiador sin `std::set`/vectores; granos con ventana Hann por recurrencia y bucles por grano; `tanh` racional
  (error < −80 dB) y wow/flutter a tasa de control; reverb y delay se duermen cuando su cola baja de −100 dB.
- All-notes-off (STOP del MPC) suelta las notas con su release; all-sound-off las corta.
- Kit: el trozo es (nota − Kit Note) mod 16 en vez de nota − 60, para que encaje con los pads.
- Polyphony: techo opcional bajo los límites de voces de la 0.42.7 (no existe en el móvil; por defecto no limita nada).

CPU estimada en la MPC One (simulador del SDK × 20, la regla que da Akai; sin medir aún en una MPC real):

| Caso | CPU |
|---|---|
| Reposo (sin notas, colas acabadas) | ~0,5 % |
| Melodic 8 notas (igual que SimpleSynth, el sinte de 8 voces de ejemplo del SDK) | ~9 % |
| Melodic 64 notas, techo FM1 / 16 / 8 | ~31 % / ~11 % / ~8 % |
| Kit 32 golpes · Looped · Arp | 7–8 % |
| Granular 8 notas · 16 notas (techo FM1) · 16 notas con techo 8 | ~23 % · ~41 % · ~24 % |
| Paulstretch 8 notas | ~19 % más su hilo en otro núcleo |

El coste depende de las voces que suenan, no del límite: con pocas notas a la vez da igual el techo.

## Compilar y probar

```sh
python3 mpc/build_sdcard.py --looperx <LooperX_MPC_Plugins_1.0.0.zip>   # clona el SDK si hace falta, compila, prueba y monta mpc/dist/
bash mpc/tests/run_tests.sh mpc/.sdk/mpc-plugin-builder-sdk               # pruebas funcionales, CPU y fidelidad contra el motor del móvil
```

`build_sdcard.py` compila con el SDK (nativo para pruebas, ARM32 para la MPC One, ARM64 para las MPC nuevas), genera la
interfaz, valida, pasa el simulador del MPC y escribe `mpc/dist/SD/` y el zip para la tarjeta.

La fidelidad se comprueba contra `mpc/tests/reference/`, copia sin tocar del `engine/dsp` de la 0.42.7 (al portar una
versión nueva del motor, se actualiza esa copia y las pruebas dicen si el port sigue igual).

Código: `mpc/FM1Sampler/src/FM1Sampler.cpp` (plugin, hilos, parámetros, FM-1), `src/fm1/` (el motor del FM1 adaptado; cada
archivo cambiado lo explica en su cabecera), `src/kissfft/`.

## Créditos y licencias

- Motor: FM1 Sampler, [Koolkatze/fm1-sampler](https://github.com/Koolkatze/fm1-sampler), fork de
  [ferluht/fm1-sampler](https://github.com/ferluht/fm1-sampler). El repositorio original **no declara licencia**: para uso
  propio no hay problema, pero antes de distribuir el plugin públicamente conviene pedir permiso a su autor.
- Samples de fábrica: los de `samples/` de este repositorio.
- kissfft: BSD-3-Clause (`src/kissfft/COPYING`).
- Framework del SDK de Akai/inMusic: MIT. "VST" es marca de Steinberg; no se usa código del VST SDK.
- El firmware del FM-1 (`firmware/`) tiene su propia licencia y no forma parte del plugin.
