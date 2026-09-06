/* Minimal SDL_thread.h shim for Android NDK builds of mupen64plus-core.
   Replaces SDL2 thread/mutex/cond with pthreads. */
#pragma once
#include <pthread.h>
#include <stdlib.h>

typedef unsigned int Uint32;

/* ── Types ── */
typedef struct { pthread_mutex_t m; } SDL_mutex;
typedef struct { pthread_cond_t  c; } SDL_cond;
typedef struct { pthread_t t;       } SDL_Thread;

/* ── Mutex ── */
static inline SDL_mutex* SDL_CreateMutex(void) {
    SDL_mutex* mtx = (SDL_mutex*)malloc(sizeof(SDL_mutex));
    if (mtx) pthread_mutex_init(&mtx->m, NULL);
    return mtx;
}
static inline void SDL_DestroyMutex(SDL_mutex* mtx) {
    if (mtx) { pthread_mutex_destroy(&mtx->m); free(mtx); }
}
static inline int SDL_LockMutex(SDL_mutex* mtx)   { return pthread_mutex_lock(&mtx->m); }
static inline int SDL_UnlockMutex(SDL_mutex* mtx) { return pthread_mutex_unlock(&mtx->m); }

/* ── Condition variable ── */
static inline SDL_cond* SDL_CreateCond(void) {
    SDL_cond* c = (SDL_cond*)malloc(sizeof(SDL_cond));
    if (c) pthread_cond_init(&c->c, NULL);
    return c;
}
static inline void SDL_DestroyCond(SDL_cond* c) {
    if (c) { pthread_cond_destroy(&c->c); free(c); }
}
static inline int SDL_CondWait(SDL_cond* c, SDL_mutex* mtx) {
    return pthread_cond_wait(&c->c, &mtx->m);
}
static inline int SDL_CondSignal(SDL_cond* c) {
    return pthread_cond_signal(&c->c);
}

/* ── Threads ── */
typedef struct { int (*fn)(void*); void* data; } _sdl_thd_args;
static inline void* _sdl_thd_wrapper(void* arg) {
    _sdl_thd_args* a = ((_sdl_thd_args*)arg);
    int (*fn)(void*) = a->fn;
    void* data = a->data;
    free(a);
    fn(data);
    return NULL;
}
static inline SDL_Thread* SDL_CreateThread(int (*fn)(void*), const char* name, void* data) {
    (void)name;
    SDL_Thread* t = (SDL_Thread*)malloc(sizeof(SDL_Thread));
    if (!t) return NULL;
    _sdl_thd_args* a = (_sdl_thd_args*)malloc(sizeof(_sdl_thd_args));
    if (!a) { free(t); return NULL; }
    a->fn = fn; a->data = data;
    if (pthread_create(&t->t, NULL, _sdl_thd_wrapper, a) != 0) {
        free(a); free(t); return NULL;
    }
    return t;
}
static inline void SDL_WaitThread(SDL_Thread* t, int* status) {
    if (!t) return;
    pthread_join(t->t, NULL);
    if (status) *status = 0;
    free(t);
}
