# VTX SDK — Resumen de rendimiento (para todo el equipo)

**Fecha:** 23 abril 2026 (revisado — ahora incluye medidas aisladas de las tres capas del API de accesores)
**Versión medida:** `main` después de los últimos dos PRs (prefetch tuning + fix del deadlock)

## ¿Qué hemos medido?

El SDK de VTX hace tres cosas con ficheros de replay de juegos: los **escribe**, los **lee** y **compara** lo que cambia entre dos instantes (frames). Hemos medido cómo de rápido hace cada una de esas tareas usando cuatro materiales de prueba:

- Un replay sintético pequeño (10 000 frames).
- Un replay real de **Counter-Strike 2** (92 MB, ~10 600 frames).
- Un replay real de **Rocket League** (5 MB, ~21 000 frames).
- Un replay sintético de nuestra "arena" (3 600 frames).

## Cómo organiza el SDK la lectura de propiedades (importante para el resto del resumen)

El SDK expone el acceso a propiedades (leer "¿cuánta vida tiene el Player 7 ahora mismo?") en **tres capas**. Importa porque cuando alguien pregunta "¿es rápido tu SDK?", la respuesta depende de qué capa esté preguntando. Ahora medimos cada una por separado.

Piénsalo como ir a una biblioteca:

| Concepto del SDK | Analogía de la biblioteca | Cuándo se paga el coste | Con qué frecuencia |
|---|---|---|---|
| **FrameAccessor** | Saber a qué biblioteca ir | Al arrancar la integración | Una vez por replay |
| **PropertyKey** | Apuntar el número de estantería de cada libro | Setup de la integración | Una vez por propiedad que te importe |
| **EntityView** | Abrir el libro y leer la respuesta | Cada vez que tu código lee un valor | Millones de veces por segundo |

## Los números que importan, en humano

| Tarea | Resultado | Lectura en plano |
|---|---|---|
| **Escribir un replay** | ~82 000 frames por segundo | Guardar una partida de 30 min tarda ~1.3 s de CPU. |
| **Leer el CS2 entero (mediana)** | ~5.6 segundos | Comparable a abrir un vídeo largo en un editor. |
| **Previsualizar los 1 000 primeros frames** | ~1 segundo | La UI muestra un preview sin lag apreciable. |
| **Saltar al 50 % y ver 300 frames** | ~0.9 segundos | Navegar por la timeline es fluido. |
| **Crear un FrameAccessor** | 6.77 microsegundos | Una vez por replay. Invisible. |
| **Resolver un PropertyKey** | ~74 nanosegundos cada uno | 10 propiedades ≈ 0.7 µs total. Invisible. |
| **Leer un valor con EntityView (aislado)** | **1.7 nanosegundos** | 585 millones de lecturas/s. Tope teórico. |
| **Leer propiedades en hot loop real** | ~80 ns por lectura (13 M/s) | Lo que se observa con el loop de entidades encima. |
| **Comparar dos frames** | 4 microsegundos | 267 000 comparaciones/s. Instantáneo. |

## Semáforo: qué va bien, qué va regular, qué va mal

De un vistazo, cómo está la salud del SDK hoy:

| Área | Veredicto | Por qué lo decimos |
|---|---|---|
| Escritura de replays | 🟢 Rápido | 82 k frames/s end-to-end. Grabación en tiempo real con ~10× de margen. |
| Lectura secuencial de un replay completo | 🟢 Suficientemente rápido | ~5.6 s para un CS de 92 MB (mediana). Comparable a herramientas de la industria. |
| Lectura de preview (primeros 1 000 frames) | 🟢 Rápido | 1 s — por debajo del umbral de percepción en UI. |
| Scrubbing con caché **bien dimensionada** | 🟢 Rápido | <4 s para 50 saltos aleatorios. |
| Scrubbing con caché **mal dimensionada** | 🔴 **Activamente malo** | 23 s para la misma carga. Hostil al usuario si no lo documentamos. |
| Creación de FrameAccessor | 🟢 Despreciable | 6.77 µs, una única vez por replay. |
| Resolución de PropertyKey | 🟢 Despreciable | 74 ns por clave × propiedades_que_te_importan. |
| EntityView en hot loop | 🟢 Excelente | 1.7 ns aislado / ~80 ns en loop realista. |
| Diff de frames consecutivos | 🟢 Instantáneo | 4 µs. No es preocupación. |
| Diff de frames idénticos (short-circuit) | 🟢 Instantáneo | Atajo funcionando como se diseñó. |
| Parseo de schema | 🟢 Despreciable | 200 µs, una única vez por fichero. |
| Elección de formato (FBS vs Proto) | 🟡 Depende del contexto | No hay ganador universal; medir por cliente. |
| Estabilidad del scan secuencial de CS | 🟡 Ruidoso | Una iteración outlier por run; la mediana es fiable, la media fluctúa. |

## Los tres hallazgos que conviene que todo el mundo conozca

### 1. Una caché pequeña es PEOR que ninguna caché

Cuando el usuario salta a puntos aleatorios del replay (por ejemplo scrubbing de la timeline), hay una **memoria temporal** configurable (la "cache window") que recuerda los trozos del fichero recientemente leídos. Si es demasiado pequeña, cada salto desaloja trozos que se iban a reutilizar → acabas leyendo lo mismo del disco varias veces.

Medido en el replay CS2:

- Caché en 0 (desactivada): 14.56 segundos para 50 saltos aleatorios.
- Caché en 2: **23.20 segundos** (¡un 59 % más lento!).
- Caché en 10: **3.27 segundos** (4.5× más rápido que sin caché — cabe el replay entero).

**Acción recomendada:** documentar claramente en la guía de usuario que la caché hay que dimensionarla a la ventana de navegación esperada, no a un valor pequeño por defecto.

### 2. El API de accesores tiene tres capas con costes muy distintos

- Coste de arranque de una integración (FrameAccessor + 10 PropertyKeys): **~8 microsegundos, una vez**. Nunca será un problema.
- Hot loop en estado estacionario (EntityView): **1.7 ns aislado, ~80 ns en un loop realista**.

**Acción recomendada:** cuando un cliente pregunte "¿cuál es el coste por lectura?", la respuesta honesta tiene tres partes. Liderar con el hot loop realista (~13 M lecturas/s), usar el número aislado (585 M/s) sólo si nos piden específicamente el "techo teórico".

### 3. No hay un "formato ganador" universal

El SDK soporta dos formatos internos (**FlatBuffers** y **Protobuf**). La pregunta típica "¿cuál es más rápido?" tiene una respuesta honesta: **depende del replay**.

- En CS2 (fichero grande, payload pesado por frame): Protobuf gana por 90 % (mediana).
- En Rocket League (fichero más pequeño, schema distinto): FlatBuffers gana por 3.5×.

**Acción recomendada:** en materiales de marketing y README no afirmar "X es más rápido que Y" sin contexto. Si un cliente pregunta, la respuesta correcta es: "medidlo en vuestra forma de replay".

## Qué tener en cuenta al leer estos números

- **Una sola máquina, Windows.** Números representativos, no estadísticamente rigurosos. Pendiente: repetir en Linux.
- **Outlier estructural en CS.** La lectura secuencial del CS2 tiene una iteración lenta que dispara la media. El outlier se mueve entre formatos entre runs, lo que indica que es ruido del SO (cache de ficheros), no un problema del SDK. **La mediana es el número fiable.**
- **"Items por segundo" en los gráficos técnicos mide tiempo de CPU**, no tiempo de reloj real. Para mensajes comerciales → tirar del tiempo de reloj.
- **Hay un bug conocido en un benchmark** (`BM_AccessorRandomWithinBucket` cuenta entidades por duplicado). Ya está flaggeado como tarea aparte. Todo lo demás es fiable.

## Dónde está el detalle completo

- **Informe técnico (ingeniería):** `docs/benchmarks/REPORT_2026-04-23.md`
- **Resumen ejecutivo en inglés (más detallado):** `docs/benchmarks/EXECUTIVE_SUMMARY_2026-04-23.md`
- **Datos en crudo (JSON, reutilizable para gráficas):** `docs/benchmarks/bench_20260423_162008.json`
- **Salida de consola tal cual:** `docs/benchmarks/bench_20260423_162008.txt`

## Próximos pasos sugeridos

1. Repetir en Linux (CI) para tener una base de comparación multi-plataforma.
2. Guardar estos números como **baseline**. Futuros cambios deberían comparar contra esta referencia y levantar alerta si empeoran más de un umbral (p. ej. 10 %).
3. Ampliar la documentación pública con la guía sobre el tamaño de caché (hallazgo #1) y el modelo de tres capas del accesor (hallazgo #2).
4. Arreglar el bug del benchmark (`BM_AccessorRandomWithinBucket`) para dejar los números limpios.
