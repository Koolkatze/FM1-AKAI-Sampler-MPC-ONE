# FM1 Sampler para MPC: documentación técnica

La guía de uso (instalación, controles de la MPC One, páginas, cómo se ha adaptado cada función) está en el
[README principal](../README.md). Aquí: cómo está hecho, cómo se compila, cómo se prueba y cómo portar versiones nuevas
del motor.

## Estructura

| Ruta | Qué es |
|---|---|
| `FM1Sampler/plugin.json`, `ui.json` | Identidad del plugin (`FM1 - VST - FM1 Sampler`, uid `Fm1S`) y su interfaz (6 pestañas, Q-Links por filas) |
| `FM1Sampler/src/FM1Sampler.cpp` | El plugin: 45 parámetros, presets, MIDI, cadena de audio, hilos, biblioteca de samples, estado, FM-1 |
| `FM1Sampler/src/fm1/` | El motor del FM1 (`engine/dsp`) adaptado; cada archivo cambiado explica en su cabecera qué y por qué |
| `FM1Sampler/src/fm1_engine.cpp` | Compila el motor y kissfft como una unidad (el SDK solo compila los `.cpp` de primer nivel de `src/`) |
| `build_sdcard.py` | Compila con el SDK, prueba con su simulador y monta `dist/SD` y el zip de la tarjeta |
| `sdcard/` | Los LEEME que van en la tarjeta |
| `tests/` | `fm1_test.cpp` (pruebas funcionales y CPU), `equivalence_*.cpp` + `scenario.h` + `reference/` (fidelidad), `run_tests.sh` |
| `release/` | La tarjeta SD lista para copiar |

Los samples de fábrica no se duplican: `build_sdcard.py` copia `samples/` del repositorio en `Content/Samples` del plugin.

## Cadena de audio

Por sub-bloques de 64 muestras (el móvil usa bloques de 256): MIDI → (FM-1 opcional) → escala/acordes → arpegiador
(tempo del MPC) → Sampler → filtro del LFO → inserto de cinta → volumen × LFO y panorama → salida y envíos post-fader →
reverb (retorno 0,8) y delay de cinta (retorno 0,7) → saturación de salida `tanh(0,9 x)`. Igual que `Session::block`.

## Hilos

| Hilo | Hace | Nunca |
|---|---|---|
| Audio (`process`) | Lee los parámetros por sub-bloque, toca, recoge objetos nuevos de tres buzones (sample, copia estirada, trozos) y devuelve los viejos por un anillo sin bloqueos | Reservar o liberar memoria, bloquear, archivos |
| Trabajo | Escanea carpetas, carga y normaliza WAV, onsets y tono (YIN), trozos, WSOLA de Speed/Pitch, borra objetos retirados, avisa al host de cambios hechos desde el FM-1 | |
| Paulstretch | Calcula los pasos espectrales de hasta 8 voces unos pasos por delante en FIFOs sin bloqueos | |

Un buffer retirado puede seguir leyéndose en un paso de Paulstretch a medias: el audio para los flujos antes de retirarlo
y el hilo de trabajo solo lo borra cuando todos los flujos han confirmado esas órdenes (seqlock con número de orden y acuse).

## Compilar

```sh
python3 mpc/build_sdcard.py [--sdk RUTA] [--looperx ZIP_O_CARPETA] [--no-test]
```

Busca el SDK en `--sdk`, `$MPC_SDK` o `mpc/.sdk/` (si no está, lo clona). El SDK compila con Zig: nativo para pruebas,
`arm-linux-gnueabihf` (Cortex-A17, MPC One) y `aarch64` (MPC nuevas); genera la interfaz y valida. Resultado en
`mpc/dist/SD/` y `mpc/dist/FM1_LooperX_MPC_SD.zip`.

## Probar

```sh
bash mpc/tests/run_tests.sh <ruta del SDK>
```

1. **`fm1_test`**: carga el plugin como lo hace el MPC; recorre los algoritmos con los presets; mide CPU por bloque
   (también a polifonía completa y con techo); comprueba tono, Speed 0,5× (doble de largo), estado y restauración por ruta,
   carpetas de usuario y de LooperX, trozos de Kit, control desde el FM-1 y bloques de 32/96/512.
2. **Fidelidad**: el motor del móvil sin tocar (`tests/reference`, de la 0.42.7) y el port renderizan el mismo escenario
   con los samples de fábrica, con 3 notas y con polifonía completa; la diferencia tiene que quedar por debajo de −100 dB.

El simulador del SDK (`host_sim`) se ejecuta en `build_sdcard.py`; su único aviso es esperado (el release de 2 s del FM1
sigue sonando medio segundo después de *all notes off*).

## Portar una versión nueva del FM1 Sampler

1. Mira qué cambia en `engine/dsp` (lo demás es de la app Android: interfaz, proyectos, looper).
2. Lleva el cambio a `FM1Sampler/src/fm1/` manteniendo las reglas de tiempo real.
3. Sustituye `tests/reference/` por los archivos nuevos de `engine/dsp` y ejecuta `run_tests.sh`: si la fidelidad pasa,
   el plugin suena como la versión nueva.
4. Sube la versión en `plugin.json` y en `makeInfo()`; los parámetros nuevos, siempre al final (los proyectos los guardan por id).

Historial: 1.0.0 = motor 0.42.6; 1.1.0 = polifonía de la 0.42.7 y parámetro Polyphony.
