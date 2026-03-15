/*
 * Vita-specific stubs and wrappers for missing/incompatible functions.
 * Based on VitaPlex stubs, stripped of video-related code.
 */

#ifdef __vita__

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/*
 * Wrapper for pthread_create that enforces a minimum stack size of 512KB.
 * This prevents stack overflow crashes in FFmpeg's audio decoder threads.
 * Linked via -Wl,--wrap=pthread_create
 */
int __real_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                          void *(*start_routine)(void *), void *arg);

int __wrap_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                          void *(*start_routine)(void *), void *arg) {
    pthread_attr_t newAttr;
    const size_t MIN_STACK = 512 * 1024; /* 512KB minimum */

    if (attr == NULL) {
        pthread_attr_init(&newAttr);
        pthread_attr_setstacksize(&newAttr, MIN_STACK);
        int ret = __real_pthread_create(thread, &newAttr, start_routine, arg);
        pthread_attr_destroy(&newAttr);
        return ret;
    }

    size_t stackSize = 0;
    pthread_attr_getstacksize(attr, &stackSize);
    if (stackSize < MIN_STACK) {
        memcpy(&newAttr, attr, sizeof(pthread_attr_t));
        pthread_attr_setstacksize(&newAttr, MIN_STACK);
        return __real_pthread_create(thread, &newAttr, start_routine, arg);
    }

    return __real_pthread_create(thread, attr, start_routine, arg);
}

/* Stubs for functions that may be missing on Vita newlib */
void flockfile(FILE *f) { (void)f; }
void funlockfile(FILE *f) { (void)f; }

/* SDL_OpenURL stub (not available on Vita) */
int SDL_OpenURL(const char *url) {
    (void)url;
    return -1;
}

/*
 * Minimal getnameinfo for FFmpeg compatibility.
 * Vita's network stack may not provide this.
 */
#include <psp2/net/net.h>

int getnameinfo(const void *sa, unsigned int salen,
                char *host, unsigned int hostlen,
                char *serv, unsigned int servlen,
                int flags) {
    (void)flags;
    (void)salen;

    const SceNetSockaddrIn *sin = (const SceNetSockaddrIn *)sa;

    if (host && hostlen > 0) {
        sceNetInetNtop(SCE_NET_AF_INET, &sin->sin_addr, host, hostlen);
    }
    if (serv && servlen > 0) {
        snprintf(serv, servlen, "%d", sceNetNtohs(sin->sin_port));
    }

    return 0;
}

#endif /* __vita__ */
