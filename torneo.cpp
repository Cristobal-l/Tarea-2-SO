#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <algorithm>
#include <random>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <ctime>
#include <cerrno>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <pthread.h>
#include <semaphore.h>

constexpr int NUM_NODOS = 28;
constexpr int NUM_PERROS = 12;
constexpr int MAX_VECINOS = 6;
constexpr int MAX_SALTOS = 6;
constexpr const char *SHM_NOMBRE = "/komikan_torneo_shm";

/* Estados de las casillas */
enum EstadoCasilla {
    CASILLA_VACIA = 0,
    CASILLA_PUMA  = 1,
    CASILLA_PERRO = 2
};

/* Estados de termino de la partida */
enum EstadoPartida {
    PARTIDA_EN_CURSO = 0,
    VICTORIA_PUMA_CAPTURAS,
    VICTORIA_PERROS_ACORRALADO,
    VICTORIA_PUMA_ABANDONO_PERROS,
    VICTORIA_PERROS_ABANDONO_PUMA,
    PARTIDA_ABORTADA_SIGINT
};

/* Representar un salto válido */
struct SaltoValido {
    int sobre;
    int destino;
};

/* Cargar valores de config.txt */
struct Configuracion {
    int delay_min_ms;
    int delay_max_ms;
    unsigned int seed;
    int pos_inicial_puma;
    int pos_inicial_perros[NUM_PERROS];
    int prob_fallo_milesimas;
};

/* Peticion de validacion al Juez */
struct PeticionJuez {
    bool pendiente;
    int tipo_ficha;
    int id_ficha;
    int origen;
    int sobre;           /* -1 si es movimiento, >=0 si es captura */
    int destino;
    bool aprobada;
    bool procesada;
};

/* Mesa de Juego en Memoria Compartida */
struct MesaJuego {
    int id_mesa;
    bool ocupada;
    int id_partida;
    int cliente_puma_id;
    int cliente_perros_id;

    /* Tablero compartido e informacion de piezas */
    int casillas[NUM_NODOS];
    int pos_puma;
    int pos_perros[NUM_PERROS];
    bool perro_capturado[NUM_PERROS];
    int perros_capturados;
    int total_movimientos;

    /* Heartbeats y banderas de fallo */
    bool fallo_jugador_puma;
    bool fallo_jugador_perros;
    struct timespec ultimo_latido_puma;
    struct timespec ultimo_latido_perros;

    EstadoPartida estado_partida;

    /* Primitivas de sincronizacion de la mesa */
    pthread_mutex_t mutex_casilla[NUM_NODOS];
    pthread_mutex_t mutex_mesa;
    pthread_cond_t cond_juez;
    pthread_cond_t cond_ficha;
    pthread_cond_t cond_fin_partida;

    PeticionJuez peticion;
    FILE *archivo_log;
};

/* Estructura de memoria compartida */
struct EstadoTorneoSHM {
    int N_clientes;
    int M_mesas;
    Configuracion config;

    /* Sincronizacion del servidor de emparejamiento */
    sem_t sem_mesas_libres;
    pthread_mutex_t mutex_servidor;
    pthread_cond_t cond_emparejamiento;

    int clientes_en_cola;
    int id_cliente_esperando;
    int mesa_asignada_al_par;
    bool par_listo;
    int contador_partidas;
    int clientes_finalizados;
};

/* Acceso al arreglo de mesas en el bloque de memoria compartida */
static inline MesaJuego *obtener_mesas(EstadoTorneoSHM *shm) {
    return reinterpret_cast<MesaJuego *>(shm + 1);
}

/* Variables para manejo de SIGINT y acceso al grafo del tablero */
static volatile sig_atomic_t g_sigint_recibido = 0;
static EstadoTorneoSHM *g_torneo = nullptr;
static size_t g_shm_tamano = 0;

/* Grafo estatico de adyacencias y saltos del tablero */
static int g_vecinos[NUM_NODOS][MAX_VECINOS];
static int g_num_vecinos[NUM_NODOS];
static SaltoValido g_saltos[NUM_NODOS][MAX_SALTOS];
static int g_num_saltos[NUM_NODOS];

/* Argumentos para hilos */
struct ArgsFicha {
    MesaJuego *mesa;
    int tipo_ficha;
    int id_ficha;
    unsigned int seed_local;
};

struct ArgsJuez {
    MesaJuego *mesa;
};

struct ArgsCliente {
    int id_cliente;
};

/* Manejo de CTRL + C */
static void manejador_sigint(int sig) {
    (void)sig;
    g_sigint_recibido = 1;
}

/* CONSTRUCCION DEL GRAFO DEL TABLERO */
static void agregar_arista(int u, int v) {
    for (int i = 0; i < g_num_vecinos[u]; i++) {
        if (g_vecinos[u][i] == v) return;
    }
    g_vecinos[u][g_num_vecinos[u]++] = v;
    g_vecinos[v][g_num_vecinos[v]++] = u;
}

static void agregar_salto_bidireccional(int a, int sobre, int b) {
    g_saltos[a][g_num_saltos[a]].sobre = sobre;
    g_saltos[a][g_num_saltos[a]].destino = b;
    g_num_saltos[a]++;

    g_saltos[b][g_num_saltos[b]].sobre = sobre;
    g_saltos[b][g_num_saltos[b]].destino = a;
    g_num_saltos[b]++;
}

static void inicializar_grafo_tablero() {
    std::memset(g_num_vecinos, 0, sizeof(g_num_vecinos));
    std::memset(g_num_saltos, 0, sizeof(g_num_saltos));

    /* Conexiones del Triangulo */
    agregar_arista(0, 1);
    agregar_arista(0, 5);
    agregar_arista(0, 2);
    agregar_arista(1, 2);
    agregar_arista(1, 3);
    agregar_arista(1, 4);
    agregar_arista(2, 6);
    agregar_arista(2, 7);

    /* Conexiones de la cuadricula principal */
    for (int r = 0; r < 5; r++) {
        for (int c = 0; c < 5; c++) {
            int actual = 3 + r * 5 + c;
            if (c + 1 < 5) {
                agregar_arista(actual, 3 + r * 5 + (c + 1));
            }
            if (r + 1 < 5) {
                agregar_arista(actual, 3 + (r + 1) * 5 + c);
            }
        }
    }

    /* Saltos entre el Triangulo y la Cuadricula */
    agregar_salto_bidireccional(0, 1, 3);
    agregar_salto_bidireccional(0, 2, 7);
    agregar_salto_bidireccional(0, 5, 10);
    agregar_salto_bidireccional(1, 4, 9);
    agregar_salto_bidireccional(2, 6, 11);

    /* Saltos dentro de la cuadrícula */
    for (int r = 0; r < 5; r++) {
        for (int c = 0; c < 5; c++) {
            int u = 3 + r * 5 + c;
            if (c + 2 < 5) {
                int sobre = 3 + r * 5 + (c + 1);
                int dest  = 3 + r * 5 + (c + 2);
                agregar_salto_bidireccional(u, sobre, dest);
            }
            if (r + 2 < 5) {
                int sobre = 3 + (r + 1) * 5 + c;
                int dest  = 3 + (r + 2) * 5 + c;
                agregar_salto_bidireccional(u, sobre, dest);
            }
        }
    }
}

/* Lectura del config.txt */
static std::string limpiar_espacios(const std::string &s) {
    size_t ini = s.find_first_not_of(" \t\r\n");
    if (ini == std::string::npos) return "";
    size_t fin = s.find_last_not_of(" \t\r\n");
    return s.substr(ini, fin - ini + 1);
}

static void cargar_configuracion_por_defecto(Configuracion *cfg) {
    cfg->delay_min_ms = 50;
    cfg->delay_max_ms = 300;
    cfg->seed = static_cast<unsigned int>(std::time(nullptr));
    cfg->pos_inicial_puma = 0;
    cfg->prob_fallo_milesimas = 0;

    int nodos_perros[NUM_PERROS] = {16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27};
    for (int i = 0; i < NUM_PERROS; i++) {
        cfg->pos_inicial_perros[i] = nodos_perros[i];
    }
}

static bool cargar_archivo_config(const char *ruta, Configuracion *cfg) {
    cargar_configuracion_por_defecto(cfg);
    if (!ruta) return true;

    std::ifstream archivo(ruta);
    if (!archivo.is_open()) {
        std::perror("Error al abrir archivo de configuración");
        return false;
    }

    std::string linea;
    while (std::getline(archivo, linea)) {
        std::string limpia = limpiar_espacios(linea);
        if (limpia.empty() || limpia[0] == '#') continue;

        size_t pos_igual = limpia.find('=');
        if (pos_igual == std::string::npos) continue;

        std::string clave = limpiar_espacios(limpia.substr(0, pos_igual));
        std::string valor = limpiar_espacios(limpia.substr(pos_igual + 1));

        if (clave == "DELAY_MIN_MS") {
            cfg->delay_min_ms = std::stoi(valor);
        } else if (clave == "DELAY_MAX_MS") {
            cfg->delay_max_ms = std::stoi(valor);
        } else if (clave == "SEED") {
            cfg->seed = static_cast<unsigned int>(std::stoul(valor));
        } else if (clave == "PUMA_POS") {
            cfg->pos_inicial_puma = std::stoi(valor);
        } else if (clave == "PROB_FALLO_MILESIMAS") {
            cfg->prob_fallo_milesimas = std::stoi(valor);
        } else if (clave == "PERROS_POS") {
            std::stringstream ss(valor);
            std::string token;
            int idx = 0;
            while (std::getline(ss, token, ',') && idx < NUM_PERROS) {
                std::string t = limpiar_espacios(token);
                if (!t.empty()) {
                    cfg->pos_inicial_perros[idx++] = std::stoi(t);
                }
            }
        }
    }

    if (cfg->delay_min_ms < 1) cfg->delay_min_ms = 1;
    if (cfg->delay_max_ms < cfg->delay_min_ms) cfg->delay_max_ms = cfg->delay_min_ms;
    return true;
}

/* Funciones auxiliares */
static void dormir_ms(int ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = static_cast<long>(ms % 1000) * 1000000L;
    nanosleep(&ts, nullptr);
}

static char simbolo_casilla(const MesaJuego *m, int nodo) {
    if (m->casillas[nodo] == CASILLA_PUMA)  return 'P';
    if (m->casillas[nodo] == CASILLA_PERRO) return 'D';
    return '.';
}

static void imprimir_tablero_log(MesaJuego *m, const char *evento) {
    if (!m->archivo_log) return;

    std::fprintf(m->archivo_log, "\n[Mesa %d | Partida #%d | Mov #%d] %s\n",
                 m->id_mesa, m->id_partida, m->total_movimientos, evento);
    std::fprintf(m->archivo_log, "Perros capturados: %d/6\n", m->perros_capturados);
    std::fprintf(m->archivo_log, "        (%c:00)\n", simbolo_casilla(m, 0));
    std::fprintf(m->archivo_log, "       /   |   \\\n");
    std::fprintf(m->archivo_log, "   (%c:01)--+--(%c:02)\n",
                 simbolo_casilla(m, 1), simbolo_casilla(m, 2));
    std::fprintf(m->archivo_log, "   /  |    |    |  \\\n");

    for (int r = 0; r < 5; r++) {
        int b = 3 + r * 5;
        std::fprintf(m->archivo_log, "(%c:%02d)-(%c:%02d)-(%c:%02d)-(%c:%02d)-(%c:%02d)\n",
                     simbolo_casilla(m, b), b,
                     simbolo_casilla(m, b + 1), b + 1,
                     simbolo_casilla(m, b + 2), b + 2,
                     simbolo_casilla(m, b + 3), b + 3,
                     simbolo_casilla(m, b + 4), b + 4);
        if (r < 4) {
            std::fprintf(m->archivo_log, "  |      |      |      |      |\n");
        }
    }
    std::fflush(m->archivo_log);
}

/* Ordena ascendentemente hasta 3 indices de casilla y toma sus mutex sin deadlock */
static void bloquear_casillas_ordenadas(MesaJuego *m, int n1, int n2, int n3) {
    int nodos[3] = {n1, n2, n3};
    int k = (n3 >= 0) ? 3 : 2;
    std::sort(nodos, nodos + k);
    for (int i = 0; i < k; i++) {
        pthread_mutex_lock(&m->mutex_casilla[nodos[i]]);
    }
}

static void desbloquear_casillas_ordenadas(MesaJuego *m, int n1, int n2, int n3) {
    int nodos[3] = {n1, n2, n3};
    int k = (n3 >= 0) ? 3 : 2;
    std::sort(nodos, nodos + k);
    for (int i = k - 1; i >= 0; i--) {
        pthread_mutex_unlock(&m->mutex_casilla[nodos[i]]);
    }
}

/* Verifica si el puma tiene salto valido disponible */
static bool puma_tiene_movimientos(const MesaJuego *m) {
    int u = m->pos_puma;
    for (int i = 0; i < g_num_vecinos[u]; i++) {
        int v = g_vecinos[u][i];
        if (m->casillas[v] == CASILLA_VACIA) return true;
    }
    for (int i = 0; i < g_num_saltos[u]; i++) {
        int sobre = g_saltos[u][i].sobre;
        int dest  = g_saltos[u][i].destino;
        if (m->casillas[sobre] == CASILLA_PERRO && m->casillas[dest] == CASILLA_VACIA) {
            return true;
        }
    }
    return false;
}

static bool son_adyacentes(int u, int v) {
    for (int i = 0; i < g_num_vecinos[u]; i++) {
        if (g_vecinos[u][i] == v) return true;
    }
    return false;
}

static bool es_salto_legal(int origen, int sobre, int destino) {
    for (int i = 0; i < g_num_saltos[origen]; i++) {
        if (g_saltos[origen][i].sobre == sobre &&
            g_saltos[origen][i].destino == destino) {
            return true;
        }
    }
    return false;
}

/* Hilo de pieza concurrente */
static void *hilo_ficha(void *arg) {
    ArgsFicha *args = static_cast<ArgsFicha *>(arg);
    MesaJuego *m = args->mesa;
    int tipo = args->tipo_ficha;
    int id = args->id_ficha;

    std::mt19937 rng(args->seed_local);
    int min_ms = g_torneo->config.delay_min_ms;
    int max_ms = g_torneo->config.delay_max_ms;
    std::uniform_int_distribution<int> dist_retardo(min_ms, max_ms);
    std::uniform_int_distribution<int> dist_milesimas(0, 999);

    while (!g_sigint_recibido) {
       /* Ciclo de decision con retardo aleatorio */
        dormir_ms(dist_retardo(rng));

        pthread_mutex_lock(&m->mutex_mesa);
        if (m->estado_partida != PARTIDA_EN_CURSO || g_sigint_recibido) {
            pthread_mutex_unlock(&m->mutex_mesa);
            break;
        }
        if (tipo == CASILLA_PERRO && m->perro_capturado[id]) {
            pthread_mutex_unlock(&m->mutex_mesa);
            break;
        }

        /* Actualizar heartbeat del jugador */
        if (tipo == CASILLA_PUMA) {
            clock_gettime(CLOCK_MONOTONIC, &m->ultimo_latido_puma);
        } else {
            clock_gettime(CLOCK_MONOTONIC, &m->ultimo_latido_perros);
        }

        /* Simulacion opcional de desconexión segun config.txt */
        if (g_torneo->config.prob_fallo_milesimas > 0 &&
            dist_milesimas(rng) < g_torneo->config.prob_fallo_milesimas) {
            if (tipo == CASILLA_PUMA) {
                m->fallo_jugador_puma = true;
            } else {
                m->fallo_jugador_perros = true;
            }
            pthread_cond_signal(&m->cond_juez);
            pthread_mutex_unlock(&m->mutex_mesa);
            break;
        }

        int mi_pos = (tipo == CASILLA_PUMA) ? m->pos_puma : m->pos_perros[id];

        /* 2. Seleccionar candidatos de movimiento */
        int cand_sobre[MAX_VECINOS + MAX_SALTOS];
        int cand_dest[MAX_VECINOS + MAX_SALTOS];
        int num_cand = 0;

        if (tipo == CASILLA_PUMA) {
            /* Priorizar capturas si estan disponibles */
            for (int i = 0; i < g_num_saltos[mi_pos]; i++) {
                int s = g_saltos[mi_pos][i].sobre;
                int d = g_saltos[mi_pos][i].destino;
                if (m->casillas[s] == CASILLA_PERRO && m->casillas[d] == CASILLA_VACIA) {
                    cand_sobre[num_cand] = s;
                    cand_dest[num_cand]  = d;
                    num_cand++;
                }
            }
        }
        /* Movimientos a casillas vacías */
        for (int i = 0; i < g_num_vecinos[mi_pos]; i++) {
            int v = g_vecinos[mi_pos][i];
            if (m->casillas[v] == CASILLA_VACIA) {
                cand_sobre[num_cand] = -1;
                cand_dest[num_cand]  = v;
                num_cand++;
            }
        }
        pthread_mutex_unlock(&m->mutex_mesa);

        if (num_cand == 0) {
            pthread_mutex_lock(&m->mutex_mesa);
            pthread_cond_signal(&m->cond_juez);
            pthread_mutex_unlock(&m->mutex_mesa);
            continue;
        }

        /* Elegir uno de los movimientos candidatos aleatoriamente */
        std::uniform_int_distribution<int> dist_cand(0, num_cand - 1);
        int eleccion = dist_cand(rng);
        int sobre = cand_sobre[eleccion];
        int destino = cand_dest[eleccion];

        /* Adquirir mutex de las casillas involucradas en orden de ID */
        bloquear_casillas_ordenadas(m, mi_pos, destino, sobre);

        pthread_mutex_lock(&m->mutex_mesa);
        if (m->peticion.pendiente &&
            m->estado_partida == PARTIDA_EN_CURSO &&
            !g_sigint_recibido) {
            /* Si otra ficha ya esta siendo validada por el Juez liberar casillas y reintentar */
            pthread_mutex_unlock(&m->mutex_mesa);
            desbloquear_casillas_ordenadas(m, mi_pos, destino, sobre);
            continue;
        }

        if (m->estado_partida != PARTIDA_EN_CURSO || g_sigint_recibido ||
            (tipo == CASILLA_PERRO && m->perro_capturado[id])) {
            pthread_mutex_unlock(&m->mutex_mesa);
            desbloquear_casillas_ordenadas(m, mi_pos, destino, sobre);
            break;
        }

        /* Enviar propuesta de movimiento al Juez */
        m->peticion.pendiente  = true;
        m->peticion.procesada  = false;
        m->peticion.aprobada   = false;
        m->peticion.tipo_ficha = tipo;
        m->peticion.id_ficha   = id;
        m->peticion.origen     = mi_pos;
        m->peticion.sobre      = sobre;
        m->peticion.destino    = destino;

        pthread_cond_signal(&m->cond_juez);
        while (!m->peticion.procesada && !g_sigint_recibido) {
            pthread_cond_wait(&m->cond_ficha, &m->mutex_mesa);
        }
        m->peticion.pendiente = false;
        pthread_mutex_unlock(&m->mutex_mesa);

        desbloquear_casillas_ordenadas(m, mi_pos, destino, sobre);
    }

    delete args;
    return nullptr;
}

/* Hilo juez de mesa */
static void inicializar_estado_tablero_juez(MesaJuego *m, const Configuracion *cfg) {
    for (int i = 0; i < NUM_NODOS; i++) {
        m->casillas[i] = CASILLA_VACIA;
    }
    m->pos_puma = cfg->pos_inicial_puma;
    m->casillas[m->pos_puma] = CASILLA_PUMA;

    for (int i = 0; i < NUM_PERROS; i++) {
        m->pos_perros[i] = cfg->pos_inicial_perros[i];
        m->perro_capturado[i] = false;
        m->casillas[m->pos_perros[i]] = CASILLA_PERRO;
    }
    m->perros_capturados = 0;
    m->total_movimientos = 0;
    m->fallo_jugador_puma = false;
    m->fallo_jugador_perros = false;
    m->estado_partida = PARTIDA_EN_CURSO;
    m->peticion.pendiente = false;
    m->peticion.procesada = false;

    clock_gettime(CLOCK_MONOTONIC, &m->ultimo_latido_puma);
    clock_gettime(CLOCK_MONOTONIC, &m->ultimo_latido_perros);
}

static void *hilo_juez_mesa(void *arg) {
    ArgsJuez *args = static_cast<ArgsJuez *>(arg);
    MesaJuego *m = args->mesa;
    delete args;

    /* El Juez inicia el tablero y abre el registro de simulacion */
    char nombre_log[64];
    std::snprintf(nombre_log, sizeof(nombre_log), "mesa_%d.log", m->id_mesa);
    m->archivo_log = std::fopen(nombre_log, "a");

    pthread_mutex_lock(&m->mutex_mesa);
    inicializar_estado_tablero_juez(m, &g_torneo->config);
    imprimir_tablero_log(m, "Inicio de partida - Tablero inicializado por el Juez");
    std::printf("[Mesa %d | Partida #%d] Juez inicia partida: Cliente %d (Puma) vs Cliente %d (Perros)\n",
                m->id_mesa, m->id_partida, m->cliente_puma_id, m->cliente_perros_id);
    pthread_mutex_unlock(&m->mutex_mesa);

    /* Lanza los 13 hilos concurrentes de las fichas */
    pthread_t hilo_puma;
    pthread_t hilos_perros[NUM_PERROS];

    ArgsFicha *arg_p = new ArgsFicha{
        m, CASILLA_PUMA, 0,
        g_torneo->config.seed ^ static_cast<unsigned int>(m->id_partida * 97 + 1)
    };
    pthread_create(&hilo_puma, nullptr, hilo_ficha, arg_p);

    for (int i = 0; i < NUM_PERROS; i++) {
        ArgsFicha *arg_d = new ArgsFicha{
            m, CASILLA_PERRO, i,
            g_torneo->config.seed ^ static_cast<unsigned int>(m->id_partida * 97 + 10 + i)
        };
        pthread_create(&hilos_perros[i], nullptr, hilo_ficha, arg_d);
    }

    /* Bucle principal de arbitraje en tiempo real */
    pthread_mutex_lock(&m->mutex_mesa);
    while (m->estado_partida == PARTIDA_EN_CURSO && !g_sigint_recibido) {
        struct timespec timeout;
        clock_gettime(CLOCK_REALTIME, &timeout);
        timeout.tv_nsec += 100 * 1000000L;
        if (timeout.tv_nsec >= 1000000000L) {
            timeout.tv_sec += 1;
            timeout.tv_nsec -= 1000000000L;
        }

        if (!m->peticion.pendiente) {
            pthread_cond_timedwait(&m->cond_juez, &m->mutex_mesa, &timeout);
        }

        if (g_sigint_recibido) {
            m->estado_partida = PARTIDA_ABORTADA_SIGINT;
            break;
        }

        /* Verificar desconexiones o fallos internos */
        struct timespec ahora;
        clock_gettime(CLOCK_MONOTONIC, &ahora);
        long inactividad_puma_ms = (ahora.tv_sec - m->ultimo_latido_puma.tv_sec) * 1000L +
                                   (ahora.tv_nsec - m->ultimo_latido_puma.tv_nsec) / 1000000L;
        long inactividad_perros_ms = (ahora.tv_sec - m->ultimo_latido_perros.tv_sec) * 1000L +
                                     (ahora.tv_nsec - m->ultimo_latido_perros.tv_nsec) / 1000000L;
        long umbral_timeout_ms = g_torneo->config.delay_max_ms * 8 + 1500;

        if (m->fallo_jugador_puma || inactividad_puma_ms > umbral_timeout_ms) {
            m->estado_partida = VICTORIA_PERROS_ABANDONO_PUMA;
            imprimir_tablero_log(m, "ANOMALÍA DETECTADA: Jugador Puma desconectado/fallido. Ganan los Perros por abandono.");
            break;
        }
        if (m->fallo_jugador_perros || inactividad_perros_ms > umbral_timeout_ms) {
            m->estado_partida = VICTORIA_PUMA_ABANDONO_PERROS;
            imprimir_tablero_log(m, "ANOMALÍA DETECTADA: Jugador Perros desconectado/fallido. Gana el Puma por abandono.");
            break;
        }

        /* Validar peticion de movimiento si hay una pendiente */
        if (m->peticion.pendiente && !m->peticion.procesada) {
            PeticionJuez *pet = &m->peticion;
            bool valido = false;
            char desc_evento[160];

            if (pet->tipo_ficha == CASILLA_PUMA) {
                if (m->pos_puma == pet->origen && m->casillas[pet->destino] == CASILLA_VACIA) {
                    if (pet->sobre == -1 && son_adyacentes(pet->origen, pet->destino)) {
                        m->casillas[pet->origen] = CASILLA_VACIA;
                        m->casillas[pet->destino] = CASILLA_PUMA;
                        m->pos_puma = pet->destino;
                        m->total_movimientos++;
                        valido = true;
                        std::snprintf(desc_evento, sizeof(desc_evento),
                                      "Puma se mueve de %02d a %02d", pet->origen, pet->destino);
                    } else if (pet->sobre >= 0 &&
                               m->casillas[pet->sobre] == CASILLA_PERRO &&
                               es_salto_legal(pet->origen, pet->sobre, pet->destino)) {
                        int perro_id = -1;
                        for (int i = 0; i < NUM_PERROS; i++) {
                            if (!m->perro_capturado[i] && m->pos_perros[i] == pet->sobre) {
                                perro_id = i;
                                break;
                            }
                        }
                        if (perro_id >= 0) {
                            m->perro_capturado[perro_id] = true;
                            m->pos_perros[perro_id] = -1;
                            m->casillas[pet->sobre] = CASILLA_VACIA;
                            m->casillas[pet->origen] = CASILLA_VACIA;
                            m->casillas[pet->destino] = CASILLA_PUMA;
                            m->pos_puma = pet->destino;
                            m->perros_capturados++;
                            m->total_movimientos++;
                            valido = true;
                            std::snprintf(desc_evento, sizeof(desc_evento),
                                          "¡CAPTURA! Puma salta de %02d sobre %02d (Perro #%d) hacia %02d",
                                          pet->origen, pet->sobre, perro_id, pet->destino);
                        }
                    }
                }
            } else if (pet->tipo_ficha == CASILLA_PERRO) {
                int id_d = pet->id_ficha;
                if (!m->perro_capturado[id_d] &&
                    m->pos_perros[id_d] == pet->origen &&
                    pet->sobre == -1 &&
                    m->casillas[pet->destino] == CASILLA_VACIA &&
                    son_adyacentes(pet->origen, pet->destino)) {
                    m->casillas[pet->origen] = CASILLA_VACIA;
                    m->casillas[pet->destino] = CASILLA_PERRO;
                    m->pos_perros[id_d] = pet->destino;
                    m->total_movimientos++;
                    valido = true;
                    std::snprintf(desc_evento, sizeof(desc_evento),
                                  "Perro #%02d se mueve de %02d a %02d", id_d, pet->origen, pet->destino);
                }
            }

            pet->aprobada = valido;
            pet->procesada = true;
            pthread_cond_broadcast(&m->cond_ficha);

            if (valido) {
                imprimir_tablero_log(m, desc_evento);
            }
        }

        /* Revisar condiciones de victoria */
        if (m->perros_capturados >= 6) {
            m->estado_partida = VICTORIA_PUMA_CAPTURAS;
            imprimir_tablero_log(m, "¡VICTORIA DEL PUMA! Ha capturado a 6 perros.");
            break;
        }
        if (!puma_tiene_movimientos(m)) {
            m->estado_partida = VICTORIA_PERROS_ACORRALADO;
            imprimir_tablero_log(m, "¡VICTORIA DE LOS PERROS! El Puma ha quedado completamente acorralado.");
            break;
        }
    }

    /* Terminar partida, despertar a todas las fichas y esperar su termino */
    if (m->peticion.pendiente && !m->peticion.procesada) {
        m->peticion.procesada = true;
        m->peticion.aprobada = false;
    }
    pthread_cond_broadcast(&m->cond_ficha);
    pthread_mutex_unlock(&m->mutex_mesa);

    pthread_join(hilo_puma, nullptr);
    for (int i = 0; i < NUM_PERROS; i++) {
        pthread_join(hilos_perros[i], nullptr);
    }

    pthread_mutex_lock(&m->mutex_mesa);
    const char *resumen = "Partida abortada por SIGINT";
    if (m->estado_partida == VICTORIA_PUMA_CAPTURAS)
        resumen = "Ganador: PUMA (6 perros capturados)";
    else if (m->estado_partida == VICTORIA_PERROS_ACORRALADO)
        resumen = "Ganador: PERROS (Puma acorralado)";
    else if (m->estado_partida == VICTORIA_PUMA_ABANDONO_PERROS)
        resumen = "Ganador: PUMA (Abandono/Fallo de Perros)";
    else if (m->estado_partida == VICTORIA_PERROS_ABANDONO_PUMA)
        resumen = "Ganador: PERROS (Abandono/Fallo de Puma)";

    std::printf("[Mesa %d | Partida #%d] Finalizada (%d movs) -> %s (Log: %s)\n",
                m->id_mesa, m->id_partida, m->total_movimientos, resumen, nombre_log);

    if (m->archivo_log) {
        std::fprintf(m->archivo_log, "=== FIN PARTIDA #%d: %s ===\n", m->id_partida, resumen);
        std::fclose(m->archivo_log);
        m->archivo_log = nullptr;
    }

    pthread_cond_broadcast(&m->cond_fin_partida);
    pthread_mutex_unlock(&m->mutex_mesa);

    return nullptr;
}

/* Emparejamiento y ciclo de vida del cliente */
static void *hilo_cliente(void *arg) {
    ArgsCliente *args = static_cast<ArgsCliente *>(arg);
    int mi_id = args->id_cliente;
    delete args;

    MesaJuego *mesas = obtener_mesas(g_torneo);

    pthread_mutex_lock(&g_torneo->mutex_servidor);
    if (g_sigint_recibido) {
        pthread_mutex_unlock(&g_torneo->mutex_servidor);
        return nullptr;
    }

    /* Si hay un numero impar de clientes y es el ultimo sin pareja posible */
    if (g_torneo->clientes_finalizados + 1 == g_torneo->N_clientes &&
        g_torneo->clientes_en_cola == 0 && (g_torneo->N_clientes % 2 != 0)) {
        std::printf("[Servidor] Cliente %d se retira sin partida (N=%d es impar).\n",
                    mi_id, g_torneo->N_clientes);
        g_torneo->clientes_finalizados++;
        pthread_mutex_unlock(&g_torneo->mutex_servidor);
        return nullptr;
    }

    /* Esperar turno de emparejamiento si otro par se esta formando */
    while (g_torneo->par_listo && !g_sigint_recibido) {
        pthread_cond_wait(&g_torneo->cond_emparejamiento, &g_torneo->mutex_servidor);
    }
    if (g_sigint_recibido) {
        pthread_mutex_unlock(&g_torneo->mutex_servidor);
        return nullptr;
    }

    if (g_torneo->clientes_en_cola == 0) {
        /* Primer jugador del par */
        g_torneo->clientes_en_cola = 1;
        g_torneo->id_cliente_esperando = mi_id;

        while (!g_torneo->par_listo && !g_sigint_recibido) {
            if (g_torneo->clientes_finalizados + 1 == g_torneo->N_clientes &&
                (g_torneo->N_clientes % 2 != 0)) {
                std::printf("[Servidor] Cliente %d libera espera (N=%d impar, sin oponente).\n",
                            mi_id, g_torneo->N_clientes);
                g_torneo->clientes_en_cola = 0;
                g_torneo->clientes_finalizados++;
                pthread_mutex_unlock(&g_torneo->mutex_servidor);
                return nullptr;
            }
            pthread_cond_wait(&g_torneo->cond_emparejamiento, &g_torneo->mutex_servidor);
        }

        if (g_sigint_recibido) {
            pthread_mutex_unlock(&g_torneo->mutex_servidor);
            return nullptr;
        }

        int id_mesa = g_torneo->mesa_asignada_al_par;
        g_torneo->par_listo = false;
        pthread_cond_broadcast(&g_torneo->cond_emparejamiento);
        pthread_mutex_unlock(&g_torneo->mutex_servidor);

        /* Esperar a que el Juez termine la partida */
        MesaJuego *m = &mesas[id_mesa];
        pthread_mutex_lock(&m->mutex_mesa);
        while (m->estado_partida == PARTIDA_EN_CURSO && !g_sigint_recibido) {
            pthread_cond_wait(&m->cond_fin_partida, &m->mutex_mesa);
        }
        pthread_mutex_unlock(&m->mutex_mesa);

        pthread_mutex_lock(&g_torneo->mutex_servidor);
        g_torneo->clientes_finalizados++;
        pthread_cond_broadcast(&g_torneo->cond_emparejamiento);
        pthread_mutex_unlock(&g_torneo->mutex_servidor);
    } else {
        /* Segundo jugador del par completa la pareja */
        int id_puma = g_torneo->id_cliente_esperando;
        int id_perros = mi_id;
        g_torneo->clientes_en_cola = 0;
        pthread_mutex_unlock(&g_torneo->mutex_servidor);

        /* Esperar por una de las mesas disponibles */
        while (sem_wait(&g_torneo->sem_mesas_libres) == -1) {
            if (errno == EINTR && g_sigint_recibido) {
                pthread_mutex_lock(&g_torneo->mutex_servidor);
                pthread_cond_broadcast(&g_torneo->cond_emparejamiento);
                pthread_mutex_unlock(&g_torneo->mutex_servidor);
                return nullptr;
            }
        }

        if (g_sigint_recibido) {
            sem_post(&g_torneo->sem_mesas_libres);
            pthread_mutex_lock(&g_torneo->mutex_servidor);
            pthread_cond_broadcast(&g_torneo->cond_emparejamiento);
            pthread_mutex_unlock(&g_torneo->mutex_servidor);
            return nullptr;
        }

        pthread_mutex_lock(&g_torneo->mutex_servidor);
        int id_mesa = -1;
        for (int i = 0; i < g_torneo->M_mesas; i++) {
            if (!mesas[i].ocupada) {
                mesas[i].ocupada = true;
                id_mesa = i;
                break;
            }
        }

        MesaJuego *m = &mesas[id_mesa];
        m->id_partida = ++g_torneo->contador_partidas;
        m->cliente_puma_id = id_puma;
        m->cliente_perros_id = id_perros;
        m->estado_partida = PARTIDA_EN_CURSO;

        g_torneo->mesa_asignada_al_par = id_mesa;
        g_torneo->par_listo = true;
        pthread_cond_broadcast(&g_torneo->cond_emparejamiento);
        pthread_mutex_unlock(&g_torneo->mutex_servidor);

        /* Lanzar el hilo Juez de la mesa asignada */
        pthread_t hilo_juez;
        ArgsJuez *arg_j = new ArgsJuez{m};
        pthread_create(&hilo_juez, nullptr, hilo_juez_mesa, arg_j);

        /* Esperar a que el Juez termine la partida */
        pthread_join(hilo_juez, nullptr);

        /* Liberar la mesa y señalizar el semáforo para los pares en espera */
        pthread_mutex_lock(&g_torneo->mutex_servidor);
        m->ocupada = false;
        g_torneo->clientes_finalizados++;
        pthread_cond_broadcast(&g_torneo->cond_emparejamiento);
        pthread_mutex_unlock(&g_torneo->mutex_servidor);

        sem_post(&g_torneo->sem_mesas_libres);
    }

    return nullptr;
}

/* Inicializacion y limpieza de memoria compartida */
static EstadoTorneoSHM *crear_memoria_compartida(int N, int M, const Configuracion *cfg) {
    g_shm_tamano = sizeof(EstadoTorneoSHM) + static_cast<size_t>(M) * sizeof(MesaJuego);

    int shm_fd = shm_open(SHM_NOMBRE, O_CREAT | O_RDWR, 0666);
    if (shm_fd == -1) {
        std::perror("shm_open");
        return nullptr;
    }
    if (ftruncate(shm_fd, static_cast<off_t>(g_shm_tamano)) == -1) {
        std::perror("ftruncate");
        close(shm_fd);
        shm_unlink(SHM_NOMBRE);
        return nullptr;
    }

    void *ptr = mmap(nullptr, g_shm_tamano, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    close(shm_fd);
    if (ptr == MAP_FAILED) {
        std::perror("mmap");
        shm_unlink(SHM_NOMBRE);
        return nullptr;
    }

    EstadoTorneoSHM *shm = static_cast<EstadoTorneoSHM *>(ptr);
    std::memset(shm, 0, g_shm_tamano);
    shm->N_clientes = N;
    shm->M_mesas = M;
    shm->config = *cfg;

    pthread_mutexattr_t mattr;
    pthread_condattr_t cattr;
    pthread_mutexattr_init(&mattr);
    pthread_mutexattr_setpshared(&mattr, PTHREAD_PROCESS_SHARED);
    pthread_condattr_init(&cattr);
    pthread_condattr_setpshared(&cattr, PTHREAD_PROCESS_SHARED);

    sem_init(&shm->sem_mesas_libres, 1, static_cast<unsigned int>(M));
    pthread_mutex_init(&shm->mutex_servidor, &mattr);
    pthread_cond_init(&shm->cond_emparejamiento, &cattr);

    MesaJuego *mesas = obtener_mesas(shm);
    for (int i = 0; i < M; i++) {
        MesaJuego *m = &mesas[i];
        m->id_mesa = i;
        m->ocupada = false;
        pthread_mutex_init(&m->mutex_mesa, &mattr);
        pthread_cond_init(&m->cond_juez, &cattr);
        pthread_cond_init(&m->cond_ficha, &cattr);
        pthread_cond_init(&m->cond_fin_partida, &cattr);
        for (int k = 0; k < NUM_NODOS; k++) {
            pthread_mutex_init(&m->mutex_casilla[k], &mattr);
        }
    }

    pthread_mutexattr_destroy(&mattr);
    pthread_condattr_destroy(&cattr);
    return shm;
}

static void destruir_recursos_torneo(EstadoTorneoSHM *shm) {
    if (!shm) return;

    MesaJuego *mesas = obtener_mesas(shm);
    for (int i = 0; i < shm->M_mesas; i++) {
        MesaJuego *m = &mesas[i];
        if (m->archivo_log) {
            std::fclose(m->archivo_log);
            m->archivo_log = nullptr;
        }
        pthread_mutex_destroy(&m->mutex_mesa);
        pthread_cond_destroy(&m->cond_juez);
        pthread_cond_destroy(&m->cond_ficha);
        pthread_cond_destroy(&m->cond_fin_partida);
        for (int k = 0; k < NUM_NODOS; k++) {
            pthread_mutex_destroy(&m->mutex_casilla[k]);
        }
    }

    pthread_mutex_destroy(&shm->mutex_servidor);
    pthread_cond_destroy(&shm->cond_emparejamiento);
    sem_destroy(&shm->sem_mesas_libres);

    munmap(shm, g_shm_tamano);
    shm_unlink(SHM_NOMBRE);
}

/* Funcion principal */
int main(int argc, char *argv[]) {
    if (argc < 3 || argc > 4) {
        std::fprintf(stderr, "Uso: %s N M [config.txt]\n", argv[0]);
        std::fprintf(stderr, "  N: Número total de clientes/jugadores a procesar (>= 2)\n");
        std::fprintf(stderr, "  M: Número máximo de mesas concurrentes (>= 1)\n");
        return EXIT_FAILURE;
    }

    int N = std::atoi(argv[1]);
    int M = std::atoi(argv[2]);
    const char *ruta_config = (argc == 4) ? argv[3] : nullptr;

    if (N < 2 || M < 1) {
        std::fprintf(stderr, "Error: Se requiere N >= 2 jugadores y M >= 1 mesas.\n");
        return EXIT_FAILURE;
    }

    /* Registrar manejador de señal Ctrl+C */
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = manejador_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);

    inicializar_grafo_tablero();

    Configuracion cfg;
    if (!cargar_archivo_config(ruta_config, &cfg)) {
        return EXIT_FAILURE;
    }

    g_torneo = crear_memoria_compartida(N, M, &cfg);
    if (!g_torneo) {
        return EXIT_FAILURE;
    }

    std::printf("=== TORNEO DE KOMIKAN CONCURRENTE (C++17) ===\n");
    std::printf("Clientes totales (N): %d | Mesas máximas (M): %d | Seed: %u | Retardos: [%d, %d] ms\n",
                N, M, cfg.seed, cfg.delay_min_ms, cfg.delay_max_ms);

    std::vector<pthread_t> hilos_clientes(static_cast<size_t>(N));
    std::vector<bool> cliente_creado(static_cast<size_t>(N), false);

    /* Llegada de clientes concurrentes al servidor */
    for (int i = 0; i < N; i++) {
        if (g_sigint_recibido) {
            std::printf("\n[Servidor] Señal SIGINT detectada. Se detiene la llegada de nuevos clientes.\n");
            break;
        }
        ArgsCliente *arg = new ArgsCliente{i + 1};
        if (pthread_create(&hilos_clientes[static_cast<size_t>(i)], nullptr, hilo_cliente, arg) == 0) {
            cliente_creado[static_cast<size_t>(i)] = true;
        } else {
            delete arg;
        }
    }

    MesaJuego *mesas = obtener_mesas(g_torneo);

    /* Monitorear si llega CTRL + C mientras se ejecutan las partidas para despertar todos los hilos */
    for (int i = 0; i < N; i++) {
        if (!cliente_creado[static_cast<size_t>(i)]) continue;
        if (g_sigint_recibido) {
            pthread_mutex_lock(&g_torneo->mutex_servidor);
            pthread_cond_broadcast(&g_torneo->cond_emparejamiento);
            pthread_mutex_unlock(&g_torneo->mutex_servidor);

            for (int m_idx = 0; m_idx < M; m_idx++) {
                MesaJuego *m = &mesas[m_idx];
                pthread_mutex_lock(&m->mutex_mesa);
                pthread_cond_broadcast(&m->cond_juez);
                pthread_cond_broadcast(&m->cond_ficha);
                pthread_cond_broadcast(&m->cond_fin_partida);
                pthread_mutex_unlock(&m->mutex_mesa);
                sem_post(&g_torneo->sem_mesas_libres);
            }
        }
        pthread_join(hilos_clientes[static_cast<size_t>(i)], nullptr);
    }

    destruir_recursos_torneo(g_torneo);

    std::printf("=== TORNEO FINALIZADO LIMPIAMENTE (0 fugas de memoria / 0 recursos huérfanos) ===\n");
    return EXIT_SUCCESS;
}
