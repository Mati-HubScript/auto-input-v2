# Auto Input — by pulidoxgd

Mod de Geode (5.11.2) que juega solo: lee las hitboxes del nivel y la hitbox del jugador, y decide si **clickear**, **mantener** o **espamear**.

## Cómo compilarlo

1. Instala el [Geode CLI](https://docs.geode-sdk.org/getting-started/) y el SDK (`geode sdk install`, luego `geode sdk update` y `geode sdk install-binaries`).
2. En esta carpeta:
   ```
   geode build
   ```
3. Se genera `pulidoxgd.auto-input.geode`. Cópialo a `Geometry Dash/geode/mods/` y reinicia el juego.

También puedes subir la carpeta a GitHub: el workflow de `.github/workflows/build.yml` compila el mod y deja el `.geode` en los artefactos.

## Ajustes

| Ajuste | Qué hace |
|---|---|
| Enable Auto Input | Interruptor general. |
| Detection mode | **Performance**: lee solo las hitboxes cercanas al jugador. **Full level**: guarda todas las hitboxes del nivel al empezar (más memoria y carga, muy ligero por frame). |
| What to detect | **Spikes & blocks**: solo pinchos/sierras y bloques. **Everything**: también slopes y bloques rompibles. |
| Detection range | Hasta qué distancia por delante lee hitboxes (30 unidades = 1 bloque). |
| Automatic reaction distance | Calcula solo a qué distancia reaccionar según velocidad y modo de juego. |
| Reaction distance | Distancia manual (si el automático está apagado). |
| Reaction time | Retraso de 0 ms a 1000 ms entre detectar y actuar. |
| Safety margin | Espacio extra alrededor de cada hitbox. |
| Don't save completions | Si el bot hizo clicks, el nivel no se guarda (como un test). Viene activado. |

## Cómo decide

- **Cubo / robot:** click para saltar; **mantiene** presionado mientras sigue habiendo pinchos o bloques por delante (así re-salta al caer).
- **Bola / spider:** click corto (pulso) cuando hay obstáculo.
- **Nave / wave:** busca el hueco libre más cercano y sube o baja hacia él; en espacios estrechos alterna rápido (**espamea**).
- **UFO:** clicks cortos repetidos para subir.
- La hitbox del jugador se lee en cada frame, así que se ajusta a mini, vehículo, etc.

## Limitaciones

- Es un bot **reactivo**, no una simulación de física: en niveles difíciles puede fallar. Ajusta *Reaction distance* si salta muy pronto o tarde.
- No usa orbes, pads ni portales todavía.
- Solo controla al jugador 1 y no considera objetos que se mueven por triggers.
- Si `handleButton` no compila con tu SDK, cambia la función `sendButton` en `src/main.cpp` (hay un comentario con la alternativa).
