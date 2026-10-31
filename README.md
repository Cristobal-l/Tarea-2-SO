# Tarea 2

Implementación de simulador multijugador en tiempo real para el juego **Komikan**, $1$ Puma contra $12$ Perros en un tablero de $28$ casillas conformado por una cuadrícula, utilizando primitivas de sincronización POSIX  y memoria compartida.

---

## Cómo Funciona

1. **Servidor y Emparejamiento:**
   * Acepta la llegada concurrente de $N$ clientes y los empareja en grupos de $2$ (un jugador Puma y un jugador Perros).
   * Regula mediante un semáforo POSIX que nunca se supere el máximo de $M$ mesas activas simultáneamente. Si todas las mesas están ocupadas, los pares restantes esperan de forma pasiva.

2. **Competencia en Tiempo Real:**
   * Cada partida ejecuta $13$ hilos de fichas independientes ($1$ Puma y $12$ Perros) que compiten simultáneamente tomando decisiones con retardos aleatorios entre $50$ y $300\text{ ms}$ (o según `config.txt`).
   * Para evitar condiciones de carrera e interbloqueos, cada ficha bloquea los mutexes de las casillas involucradas en estricto orden ascendente de índice antes de efectuar su jugada.

3. **Juez Árbitro y Condiciones de Victoria:**
   * Cada mesa cuenta con un hilo **Juez** que inicializa el tablero compartido, valida las reglas de adyacencia y salto, y registra el estado visual del tablero tras cada movimiento.
   * **Victoria del Puma:** Capturar a $6$ o más perros saltando sobre ellos hacia una casilla vacía adyacente.
   * **Victoria de los Perros:** Acorralar completamente al Puma dejándolo sin movimientos ni saltos válidos.

4. **Resiliencia y Cierre Limpio:**
   * Si un jugador falla o aborta durante la partida, el Juez detecta la anomalía por inactividad, aborta la partida declarando ganador al oponente por abandono y libera la mesa.
   * Al recibir la señal `Ctrl+C`, el servidor detiene nuevas llegadas, finaliza ordenadamente las partidas activas, cierra archivos y libera mutexes, variables de condición, semáforos y memoria compartida sin dejar fugas de memoria.

---

## Estructura del Proyecto

* `torneo.cpp`: Código fuente completo del servidor, mesas, juez árbitro y piezas concurrentes.
* `Makefile`: Script de compilación con las banderas exigidas (`g++ -Wall -Wextra -std=c++17`).
* `config.txt`: Archivo de configuración opcional (tiempos de retardo, posiciones iniciales y semilla).

---

## Compilación y Ejecución

### 1. Compilar el proyecto
En una terminal Linux/Unix, ejecutar:

~~~bash
make
~~~

Esto compilará el binario `torneo`.

### 2. Ejecutar el servidor
El formato de invocación es `./torneo N M [config.txt]`:

~~~bash
# Ejecutar con 6 jugadores, máximo 2 mesas simultáneas y archivo de configuración
./torneo 6 2 config.txt

# Ejecutar usando los parámetros por defecto (sin config.txt)
./torneo 4 2
~~~

* **`N`**: Número total de clientes/jugadores a procesar.
* **`M`**: Número máximo de mesas concurrentes activas.
* **`config.txt`** *(opcional)*: Define `DELAY_MIN_MS`, `DELAY_MAX_MS`, `SEED`, `PUMA_POS` y `PERROS_POS`.

### 3. Ver el registro visual de las partidas
Cada mesa escribe el estado del tablero y cada evento en su propio archivo de registro:

~~~bash
cat mesa_0.log
~~~

### 4. Verificar memoria y limpiar archivos generados
Para comprobar cero fugas de memoria con Valgrind o limpiar los binarios y logs:

~~~bash
make valgrind
make clean
~~~
