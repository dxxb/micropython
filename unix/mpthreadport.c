/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2016 Damien P. George on behalf of Pycom Ltd
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

#include "py/mpstate.h"
#include "py/mpthread.h"
#include "py/gc.h"

#if MICROPY_PY_THREAD

#include <signal.h>
#include <sched.h>

// this structure forms a linked list, one node per active thread
typedef struct _thread_t {
    pthread_t id;           // system id of thread
    int ready;              // whether the thread is ready and running
    void *arg;              // thread Python args, a GC root pointer
    struct _thread_t *next;
} thread_t;

STATIC pthread_key_t tls_key;

// the mutex controls access to the linked list
STATIC pthread_mutex_t thread_mutex = PTHREAD_MUTEX_INITIALIZER;
STATIC thread_t *thread;

// this is used to synchronise the signal handler of the thread
// it's needed because we can't use any pthread calls in a signal handler
STATIC volatile int thread_signal_done;

// this signal handler is used to scan the regs and stack of a thread
STATIC void mp_thread_gc(int signo) {
    if (signo == SIGUSR1) {
        void gc_collect_regs_and_stack(void);
        gc_collect_regs_and_stack();
        thread_signal_done = 1;
    }
}

void mp_thread_init(void) {
    pthread_key_create(&tls_key, NULL);
    pthread_setspecific(tls_key, &mp_state_ctx.thread);

    // create first entry in linked list of all threads
    thread = malloc(sizeof(thread_t));
    thread->id = pthread_self();
    thread->ready = 1;
    thread->arg = NULL;
    thread->next = NULL;

    // enable signal handler for garbage collection
    struct sigaction sa;
    sa.sa_flags = 0;
    sa.sa_handler = mp_thread_gc;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, NULL);
}

// This function scans all pointers that are external to the current thread.
// It does this by signalling all other threads and getting them to scan their
// own registers and stack.  Note that there may still be some edge cases left
// with race conditions and root-pointer scanning: a given thread may manipulate
// the global root pointers (in mp_state_ctx) while another thread is doing a
// garbage collection and tracing these pointers.
void mp_thread_gc_others(void) {
    pthread_mutex_lock(&thread_mutex);
    for (thread_t *th = thread; th != NULL; th = th->next) {
        gc_collect_root(&th->arg, 1);
        if (th->id == pthread_self()) {
            continue;
        }
        if (!th->ready) {
            continue;
        }
        thread_signal_done = 0;
        pthread_kill(th->id, SIGUSR1);
        while (thread_signal_done == 0) {
            sched_yield();
        }
    }
    pthread_mutex_unlock(&thread_mutex);
}

mp_state_thread_t *mp_thread_get_state(void) {
    return (mp_state_thread_t*)pthread_getspecific(tls_key);
}

void mp_thread_set_state(void *state) {
    pthread_setspecific(tls_key, state);
}

void mp_thread_start(void) {
    pthread_mutex_lock(&thread_mutex);
    for (thread_t *th = thread; th != NULL; th = th->next) {
        if (th->id == pthread_self()) {
            th->ready = 1;
            break;
        }
    }
    pthread_mutex_unlock(&thread_mutex);
}

void mp_thread_create(void *(*entry)(void*), void *arg, size_t stack_size) {
    // default stack size is 8k machine-words
    if (stack_size == 0) {
        stack_size = 8192 * BYTES_PER_WORD;
    }

    // set thread attributes
    pthread_attr_t attr;
    int ret = pthread_attr_init(&attr);
    if (ret != 0) {
        goto er;
    }
    ret = pthread_attr_setstacksize(&attr, stack_size);
    if (ret != 0) {
        goto er;
    }

    pthread_mutex_lock(&thread_mutex);

    // create thread
    pthread_t id;
    ret = pthread_create(&id, &attr, entry, arg);
    if (ret != 0) {
        pthread_mutex_unlock(&thread_mutex);
        goto er;
    }

    // add thread to linked list of all threads
    thread_t *th = malloc(sizeof(thread_t));
    th->id = id;
    th->ready = 0;
    th->arg = arg;
    th->next = thread;
    thread = th;

    pthread_mutex_unlock(&thread_mutex);

    return;

er:
    nlr_raise(mp_obj_new_exception_arg1(&mp_type_OSError, MP_OBJ_NEW_SMALL_INT(ret)));
}

void mp_thread_finish(void) {
    pthread_mutex_lock(&thread_mutex);
    // TODO unlink from list
    for (thread_t *th = thread; th != NULL; th = th->next) {
        if (th->id == pthread_self()) {
            th->ready = 0;
            break;
        }
    }
    pthread_mutex_unlock(&thread_mutex);
}

void mp_thread_mutex_init(mp_thread_mutex_t *mutex) {
    pthread_mutex_init(mutex, NULL);
}

int mp_thread_mutex_lock(mp_thread_mutex_t *mutex, int wait) {
    int ret;
    if (wait) {
        ret = pthread_mutex_lock(mutex);
        if (ret == 0) {
            return 1;
        }
    } else {
        ret = pthread_mutex_trylock(mutex);
        if (ret == 0) {
            return 1;
        } else if (ret == EBUSY) {
            return 0;
        }
    }
    return -ret;
}

void mp_thread_mutex_unlock(mp_thread_mutex_t *mutex) {
    pthread_mutex_unlock(mutex);
    // TODO check return value
}

#endif // MICROPY_PY_THREAD
