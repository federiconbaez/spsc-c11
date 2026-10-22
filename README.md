# spsc-c11

Ring buffer lock-free de un productor y un consumidor en C11 (`<stdatomic.h>`), en un solo
header. Capacidad potencia de dos, contadores `size_t` de corrida libre, índices en líneas de
caché separadas con copia cacheada del índice del otro lado, operaciones por lote y peek
sin copia.

*English below.*

## Español

### API

```c
#include "spsc.h"

uint64_t storage[1024];
spsc_ring r;
spsc_init(&r, storage, 1024, sizeof(uint64_t));   // -1 si no es potencia de 2

spsc_push(&r, &v);            // productor: false si está lleno
spsc_push_n(&r, src, n);      // escribe hasta n, devuelve cuántos
spsc_pop(&r, &v);             // consumidor: false si está vacío
spsc_pop_n(&r, dst, n);
const void *p = spsc_front(&r); /* ... */ spsc_consume(&r);
```

Un solo hilo productor y un solo hilo consumidor. El productor publica con un store
*release* de `head`; el consumidor lo lee con *acquire* (y simétrico para `tail`), lo que
ordena la copia del elemento antes de la actualización del índice.

### Tests

`tests/test_spsc.c`:

- validación de `init` (0, no potencia de 2, `elem_size` 0), capacidad 1;
- FIFO, lleno/vacío y 1000 pasos intercalados;
- lotes parciales que cruzan el final del buffer;
- desborde de los contadores: arrancan en `SIZE_MAX - 4` y siguen 50 vueltas;
- `front`/`consume` sin copia;
- estrés de dos hilos con lotes aleatorios 1..32: 2 M elementos por un ring de 1024 y 500 k
  por un ring de **2** (lleno/vacío constante). Cada elemento lleva `seq`, `2·seq` y un
  checksum, así detecta desorden, pérdidas y copias rotas.

**Control negativo:** el mismo test compilado con `SPSC_ACQUIRE/SPSC_RELEASE` en
`memory_order_relaxed` **debe** hacer saltar ThreadSanitizer (`data race`); ctest falla si no.
Así el job de TSan demuestra que detecta el bug que el ordenamiento correcto evita.

CI: gcc y clang × (TSan, ASan+UBSan, Release con 20 M elementos) en x86-64 **y AArch64**
(modelo de memoria débil, donde un *relaxed* mal puesto sí se nota).

### Rendimiento medido

`bench/bench.c`, mediana de 7 corridas, 20 M de `uint64_t`, capacidad 4096, hilos fijados a
CPUs 0→1, gcc 14 `-O3`, VM de 8 vCPU (1 hilo por núcleo):

| caso | resultado |
|---|---:|
| spsc push/pop de a uno | 7.0 M elem/s |
| spsc lotes de 64 | 312 M elem/s |
| ring con mutex | 1.5 M elem/s |
| ping-pong ida y vuelta | 469 ns |

Con CPUs 0→4: 8.3 M, 339 M, 1.4 M y 480 ns. Lectura honesta: en esta VM mover una línea de
caché entre vCPUs cuesta ~230 ns (mitad del ping-pong), así que de a uno el ring queda
dominado por esa transferencia; por lotes la amortiza y rinde ~45× más. En hardware físico
los números de a uno deberían ser bastante mayores; no lo medí.

### Compilar

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build && ctest --test-dir build --output-on-failure
./build/bench_spsc 0 1            # CPU productor, CPU consumidor, [elementos]
# -DSPSC_SANITIZER=thread | address
```

### Límites

- Exactamente un productor y un consumidor; para varios, ver un MPMC.
- Elementos copiados con `memcpy` (deben ser trivialmente copiables).
- Sin espera bloqueante: `push`/`pop` devuelven falso y el llamador decide si reintenta,
  cede o duerme.

## English

Single-header lock-free single-producer/single-consumer ring in C11. Power-of-two capacity,
free-running `size_t` counters, producer and consumer indices on separate cache lines with a
cached copy of the other side's index, batch operations and zero-copy peek. Release/acquire
handoff on `head` and `tail`.

**Tests:** init validation and capacity 1; FIFO/full/empty; partial batches across the
buffer end; counter overflow starting at `SIZE_MAX - 4`; `front`/`consume`; a two-thread
stress with random batch sizes (2 M items through 1024 slots, 500 k through **2** slots),
each item carrying a sequence number and checksum to catch reordering, loss and torn copies.
**Negative control:** the same test built with relaxed orderings must trigger a
ThreadSanitizer data race, or ctest fails. CI runs gcc and clang × (TSan, ASan+UBSan,
Release with 20 M items) on x86-64 and AArch64.

**Measured** (median of 7, 20 M `uint64_t`, capacity 4096, pinned CPUs 0→1, gcc 14 `-O3`,
8-vCPU VM): single push/pop 7.0 M items/s, batches of 64 312 M items/s, mutex ring
1.5 M items/s, ping-pong round trip 469 ns. On this VM a cache-line transfer between vCPUs
costs ~230 ns, which dominates the one-at-a-time case; batching amortises it (~45×).
Bare-metal numbers were not measured.

**Limits:** exactly one producer and one consumer; trivially copyable elements; no blocking
wait.

## License

Apache-2.0.
