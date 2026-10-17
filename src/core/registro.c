/**
 * WIISP - core/registro.c
 * El hilo que escribe wiisp.log (ver registro.h).
 *
 * Una cola circular de bytes con un productor (quien llama a hle_log) y un
 * consumidor (el escritor). Todo lo compartido va con el cerrojo; el
 * escritor copia un lote a su búfer y lo escribe sin el cerrojo, así que
 * mientras espera a la tarjeta se puede seguir poniendo texto.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
**/
#include <stdlib.h>
#include <string.h>
#include "core/registro.h"
#include "core/prof.h"

#if defined(_WIN32)
#define REG_SIN_HILOS 1
#else
#include <unistd.h>
#if defined(GEKKO)
#include <ogc/lwp.h>
#include <ogc/mutex.h>
#include <ogc/cond.h>
/* Por encima del hilo principal (64): en cuanto hay texto, el escritor lo
   pasa a libfat (poco trabajo) y se queda esperando a IOS, y entonces
   vuelve la emulación */
#define REG_PRIORIDAD 100
#define REG_PILA (32 * 1024)
typedef mutex_t reg_mutex;
typedef cond_t reg_cond;
static lwp_t hilo;
static int crear(void *(*fn)(void *)){ return LWP_CreateThread(&hilo, fn, NULL, NULL, REG_PILA, REG_PRIORIDAD) == 0; }
static int mutex_init(reg_mutex *m){ return LWP_MutexInit(m, false) == 0; }
static int cond_init(reg_cond *c){ return LWP_CondInit(c) == 0; }
#define LOCK(m)          LWP_MutexLock(m)
#define UNLOCK(m)        LWP_MutexUnlock(m)
#define WAIT(c, m)       LWP_CondWait(c, m)
#define BROADCAST(c)     LWP_CondBroadcast(c)
#else
#include <pthread.h>
typedef pthread_mutex_t reg_mutex;
typedef pthread_cond_t reg_cond;
static pthread_t hilo;
static int crear(void *(*fn)(void *)){
	if(pthread_create(&hilo, NULL, fn, NULL)) return 0;
	pthread_detach(hilo);
	return 1;
}
static int mutex_init(reg_mutex *m){ return pthread_mutex_init(m, NULL) == 0; }
static int cond_init(reg_cond *c){ return pthread_cond_init(c, NULL) == 0; }
#define LOCK(m)          pthread_mutex_lock(&(m))
#define UNLOCK(m)        pthread_mutex_unlock(&(m))
#define WAIT(c, m)       pthread_cond_wait(&(c), &(m))
#define BROADCAST(c)     pthread_cond_broadcast(&(c))
#endif
#endif

#if !defined(REG_SIN_HILOS)
static reg_mutex m;
static reg_cond hay;        /* para el escritor: texto o fsync pendiente */
static reg_cond hecho;      /* para la emulación: sitio en la cola o todo escrito */
static int estado;          /* 0 sin probar, 1 en marcha, -1 no se pudo */
static char cola[REG_COLA];
static u32 cap = REG_COLA;
static u32 ini, len;        /* lo pendiente: len bytes desde cola[ini] */
static FILE *archivo;       /* de lo que hay en la cola */
static int pide_fsync, ocupado;
static RegStats st;
/* El reloj del perfilador, copiado con el cerrojo: prof_clock puede
   cambiar desde la emulación mientras el escritor mide */
static u64 (*reloj)(void);

static u64 ahora_con(u64 (*r)(void)){ return r ? r() : 0; }
#define ahora() ahora_con(reloj)

static void *escritor(void *arg){
	static char lote[16 * 1024];
	(void)arg;
	LOCK(m);
	for(;;){
		u32 n, a;
		int fs;
		FILE *f;
		u64 t0, t1, t2, (*r)(void);
		while(!len && !pide_fsync) WAIT(hay, m);
		/* Un lote: lo que quepa del principio de la cola (en dos trozos si
		   da la vuelta) */
		n = len < sizeof(lote) ? len : (u32)sizeof(lote);
		a = cap - ini < n ? cap - ini : n;
		memcpy(lote, cola + ini, a);
		memcpy(lote + a, cola, n - a);
		ini = (ini + n) % cap;
		len -= n;
		/* El fsync, cuando ya está escrito todo lo anterior */
		fs = pide_fsync && !len;
		if(fs) pide_fsync = 0;
		f = archivo;
		r = reloj;
		ocupado = 1;
		BROADCAST(hecho);
		UNLOCK(m);
		t0 = ahora_con(r);
		if(f && (n || fs)){
			if(n) fwrite(lote, 1, n, f);
			fflush(f);   /* también lo que se escribió sin la cola */
		}
		t1 = ahora_con(r);
		if(f && fs) fsync(fileno(f));
		t2 = ahora_con(r);
		LOCK(m);
		ocupado = 0;
		st.bytes += n;
		st.lotes += n > 0;
		st.t_escribir += t1 - t0;
		if(f && fs){
			st.fsyncs++;
			st.t_fsync += t2 - t1;
			if(t2 - t1 > st.t_fsync_max) st.t_fsync_max = t2 - t1;
		}
		BROADCAST(hecho);
	}
	return NULL;
}

/* Al salir del programa: que no se quede nada en la cola */
static void al_salir(void){ reg_esperar(0); }

int reg_disponible(void){
	if(!estado){
		estado = mutex_init(&m) && cond_init(&hay) && cond_init(&hecho) && crear(escritor) ? 1 : -1;
		if(estado > 0) atexit(al_salir);
	}
	return estado > 0;
}

int reg_pendiente(void){
	int r;
	if(estado <= 0) return 0;
	LOCK(m);
	r = len || ocupado || pide_fsync;
	UNLOCK(m);
	return r;
}

/* Con el cerrojo: espera a que se cumpla cond, contando la espera */
#define ESPERAR_MIENTRAS(cond) do { \
	if(cond){ \
		u64 t0_ = ahora(); \
		st.esperas++; \
		while(cond) WAIT(hecho, m); \
		st.t_esperas += ahora() - t0_; \
	} \
} while(0)

void reg_poner(FILE *f, const char *texto, u32 n, int sync){
	if(!reg_disponible()) return;
	LOCK(m);
	reloj = prof_clock;
	if(f != archivo){
		ESPERAR_MIENTRAS(len || ocupado || pide_fsync);
		archivo = f;
	}
	while(n){
		u32 sitio, k, fin, a;
		ESPERAR_MIENTRAS(len == cap);
		sitio = cap - len;
		k = n < sitio ? n : sitio;
		fin = (ini + len) % cap;
		a = cap - fin < k ? cap - fin : k;
		memcpy(cola + fin, texto, a);
		memcpy(cola, texto + a, k - a);
		len += k;
		if(len > st.max_cola) st.max_cola = len;
		texto += k;
		n -= k;
		BROADCAST(hay);
	}
	if(sync){
		pide_fsync = 1;
		BROADCAST(hay);
	}
	UNLOCK(m);
}

void reg_archivo(FILE *f){
	if(estado <= 0) return;
	LOCK(m);
	ESPERAR_MIENTRAS(len || ocupado || pide_fsync);
	archivo = f;
	UNLOCK(m);
}

void reg_pedir_fsync(void){
	if(estado <= 0) return;
	LOCK(m);
	pide_fsync = 1;
	BROADCAST(hay);
	UNLOCK(m);
}

void reg_esperar(int fsync_tambien){
	if(estado <= 0) return;
	LOCK(m);
	if(fsync_tambien && archivo){
		pide_fsync = 1;
		BROADCAST(hay);
	}
	ESPERAR_MIENTRAS(len || ocupado || pide_fsync);
	UNLOCK(m);
}

void reg_stats(RegStats *out){
	if(estado <= 0){ memset(out, 0, sizeof(*out)); return; }
	LOCK(m);
	*out = st;
	UNLOCK(m);
}

void reg_test_capacidad(u32 c){
	reg_esperar(0);
	if(estado > 0) LOCK(m);
	cap = c >= 1 && c <= REG_COLA ? c : REG_COLA;
	ini = len = 0;
	if(estado > 0) UNLOCK(m);
}

#else

int reg_disponible(void){ return 0; }
int reg_pendiente(void){ return 0; }
void reg_poner(FILE *f, const char *texto, u32 n, int sync){ (void)f; (void)texto; (void)n; (void)sync; }
void reg_archivo(FILE *f){ (void)f; }
void reg_pedir_fsync(void){}
void reg_esperar(int fsync_tambien){ (void)fsync_tambien; }
void reg_stats(RegStats *out){ memset(out, 0, sizeof(*out)); }
void reg_test_capacidad(u32 c){ (void)c; }

#endif
