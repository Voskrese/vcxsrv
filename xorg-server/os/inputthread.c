/* inputthread.c -- Threaded generation of input events.
 *
 * Copyright © 2007-2008 Tiago Vignatti <vignatti at freedesktop org>
 * Copyright © 2010 Nokia
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * Authors: Fernando Carrijo <fcarrijo at freedesktop org>
 *          Tiago Vignatti <vignatti at freedesktop org>
 */

#ifdef HAVE_DIX_CONFIG_H
#include <dix-config.h>
#endif

#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>

#include <X11/Xpoll.h>
#include "inputstr.h"
#include "opaque.h"
#include "osdep.h"

#if INPUTTHREAD

Bool InputThreadEnable = TRUE;

/**
 * An input device as seen by the threaded input facility
 */
typedef struct _InputThreadDevice {
    struct xorg_list node;
    NotifyFdProcPtr readInputProc;
    void *readInputArgs;
    int fd;
} InputThreadDevice;

/**
 * The threaded input facility.
 *
 * For now, we have one instance for all input devices.
 */
typedef struct {
    pthread_t thread;
    struct xorg_list devs;
    fd_set fds;
    int readPipe;
    int writePipe;
} InputThreadInfo;

static InputThreadInfo *inputThreadInfo;

static int hotplugPipeRead = -1;
static int hotplugPipeWrite = -1;

static int input_mutex_count;

#ifdef PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP
static pthread_mutex_t input_mutex = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
#else
static pthread_mutex_t input_mutex;
static Bool input_mutex_initialized;
#endif

void
input_lock(void)
{
#ifndef PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP
    if (!input_mutex_initialized) {
        pthread_mutexattr_t mutex_attr;

        input_mutex_initialized = TRUE;
        pthread_mutexattr_init(&mutex_attr);
        pthread_mutexattr_settype(&mutex_attr, PTHREAD_MUTEX_RECURSIVE);
        pthread_mutex_init(&input_mutex, &mutex_attr);
    }
#endif
    pthread_mutex_lock(&input_mutex);
    ++input_mutex_count;
}

void
input_unlock(void)
{
    --input_mutex_count;
    pthread_mutex_unlock(&input_mutex);
}

void
input_force_unlock(void)
{
    if (pthread_mutex_trylock(&input_mutex) == 0) {
        input_mutex_count++;
        /* unlock +1 times for the trylock */
        while (input_mutex_count > 0)
            input_unlock();
    }
}

/**
 * Notify a thread about the availability of new asynchronously enqueued input
 * events.
 *
 * @see WaitForSomething()
 */
static void
InputThreadFillPipe(int writeHead)
{
    int ret;
    char byte = 0;
    fd_set writePipe;

    FD_ZERO(&writePipe);

    while (1) {
        ret = write(writeHead, &byte, 1);
        if (!ret)
            FatalError("input-thread: write() returned 0");
        if (ret > 0)
            break;

        if (errno != EAGAIN)
            FatalError("input-thread: filling pipe");

        DebugF("input-thread: pipe full\n");
        FD_SET(writeHead, &writePipe);
        Select(writeHead + 1, NULL, &writePipe, NULL, NULL);
    }
}

/**
 * Consume eventual notifications left by a thread.
 *
 * @see WaitForSomething()
 * @see InputThreadFillPipe()
 */
static int
InputThreadReadPipe(int readHead)
{
    int ret, array[10];

    ret = read(readHead, &array, sizeof(array));
    if (ret >= 0)
        return ret;

    if (errno != EAGAIN)
        FatalError("input-thread: draining pipe (%d)", errno);

    return 1;
}

/**
 * Register an input device in the threaded input facility
 *
 * @param fd File descriptor which identifies the input device
 * @param readInputProc Procedure used to read input from the device
 * @param readInputArgs Arguments to be consumed by the above procedure
 *
 * return 1 if success; 0 otherwise.
 */
int
InputThreadRegisterDev(int fd,
                       NotifyFdProcPtr readInputProc,
                       void *readInputArgs)
{
    InputThreadDevice *dev;

    if (!inputThreadInfo)
        return SetNotifyFd(fd, readInputProc, X_NOTIFY_READ, readInputArgs);

    dev = calloc(1, sizeof(InputThreadDevice));
    if (dev == NULL) {
        DebugF("input-thread: could not register device\n");
        return 0;
    }

    dev->fd = fd;
    dev->readInputProc = readInputProc;
    dev->readInputArgs = readInputArgs;

    input_lock();
    xorg_list_add(&dev->node, &inputThreadInfo->devs);

    FD_SET(fd, &inputThreadInfo->fds);

    InputThreadFillPipe(hotplugPipeWrite);
    DebugF("input-thread: registered device %d\n", fd);
    input_unlock();

    return 1;
}

/**
 * Unregister a device in the threaded input facility
 *
 * @param fd File descriptor which identifies the input device
 *
 * @return 1 if success; 0 otherwise.
 */
int
InputThreadUnregisterDev(int fd)
{
    InputThreadDevice *dev;
    Bool found_device = FALSE;

    /* return silently if input thread is already finished (e.g., at
     * DisableDevice time, evdev tries to call this function again through
     * xf86RemoveEnabledDevice) */
    if (!inputThreadInfo) {
        RemoveNotifyFd(fd);
        return 1;
    }

    input_lock();
    xorg_list_for_each_entry(dev, &inputThreadInfo->devs, node)
        if (dev->fd == fd) {
            found_device = TRUE;
            break;
        }

    /* fd didn't match any registered device. */
    if (!found_device) {
        input_unlock();
        return 0;
    }

    xorg_list_del(&dev->node);

    FD_CLR(fd, &inputThreadInfo->fds);

    input_unlock();

    free(dev);

    InputThreadFillPipe(hotplugPipeWrite);
    DebugF("input-thread: unregistered device: %d\n", fd);

    return 1;
}

/**
 * The workhorse of threaded input event generation.
 *
 * Or if you prefer: The WaitForSomething for input devices. :)
 *
 * Runs in parallel with the server main thread, listening to input devices in
 * an endless loop. Whenever new input data is made available, calls the
 * proper device driver's routines which are ultimately responsible for the
 * generation of input events.
 *
 * @see InputThreadPreInit()
 * @see InputThreadInit()
 */

static void*
InputThreadDoWork(void *arg)
{
    fd_set readyFds;
    InputThreadDevice *dev, *next;
    sigset_t set;

    /* Don't handle any signals on this thread */
    sigfillset(&set);
    pthread_sigmask(SIG_BLOCK, &set, NULL);

    FD_ZERO(&readyFds);

    while (1)
    {
        XFD_COPYSET(&inputThreadInfo->fds, &readyFds);
        FD_SET(hotplugPipeRead, &readyFds);

        DebugF("input-thread: %s waiting for devices\n", __func__);

        if (Select(MAXSELECT, &readyFds, NULL, NULL, NULL) < 0) {
            if (errno == EINVAL)
                FatalError("input-thread: %s (%s)", __func__, strerror(errno));
            else if (errno != EINTR)
                ErrorF("input-thread: %s (%s)\n", __func__, strerror(errno));
        }

        DebugF("input-thread: %s generating events\n", __func__);

        input_lock();
        /* Call the device drivers to generate input events for us */
        xorg_list_for_each_entry_safe(dev, next, &inputThreadInfo->devs, node) {
            if (FD_ISSET(dev->fd, &readyFds) && dev->readInputProc) {
                dev->readInputProc(dev->fd, X_NOTIFY_READ, dev->readInputArgs);
            }
        }
        input_unlock();

        /* Kick main thread to process the generated input events and drain
         * events from hotplug pipe */
        InputThreadFillPipe(inputThreadInfo->writePipe);

        /* Empty pending input, shut down if the pipe has been closed */
        if (FD_ISSET(hotplugPipeRead, &readyFds)) {
            if (InputThreadReadPipe(hotplugPipeRead) == 0)
                break;
        }
    }
    return NULL;
}

static void
InputThreadNotifyPipe(int fd, int mask, void *data)
{
    InputThreadReadPipe(fd);
}

/**
 * Pre-initialize the facility used for threaded generation of input events
 *
 */
void
InputThreadPreInit(void)
{
    int fds[2], hotplugPipe[2];

    if (!InputThreadEnable)
        return;

    if (pipe(fds) < 0)
        FatalError("input-thread: could not create pipe");

     if (pipe(hotplugPipe) < 0)
        FatalError("input-thread: could not create pipe");

    inputThreadInfo = malloc(sizeof(InputThreadInfo));
    if (!inputThreadInfo)
        FatalError("input-thread: could not allocate memory");

    inputThreadInfo->thread = 0;
    xorg_list_init(&inputThreadInfo->devs);
    FD_ZERO(&inputThreadInfo->fds);

    /* By making read head non-blocking, we ensure that while the main thread
     * is busy servicing client requests, the dedicated input thread can work
     * in parallel.
     */
    inputThreadInfo->readPipe = fds[0];
    fcntl(inputThreadInfo->readPipe, F_SETFL, O_NONBLOCK | O_CLOEXEC);
    SetNotifyFd(inputThreadInfo->readPipe, InputThreadNotifyPipe, X_NOTIFY_READ, NULL);

    inputThreadInfo->writePipe = fds[1];

    hotplugPipeRead = hotplugPipe[0];
    fcntl(hotplugPipeRead, F_SETFL, O_NONBLOCK | O_CLOEXEC);
    hotplugPipeWrite = hotplugPipe[1];
}

/**
 * Start the threaded generation of input events. This routine complements what
 * was previously done by InputThreadPreInit(), being only responsible for
 * creating the dedicated input thread.
 *
 */
void
InputThreadInit(void)
{
    pthread_attr_t attr;

    /* If the driver hasn't asked for input thread support by calling
     * InputThreadPreInit, then do nothing here
     */
    if (!inputThreadInfo)
        return;

    pthread_attr_init(&attr);

    /* For OSes that differentiate between processes and threads, the following
     * lines have sense. Linux uses the 1:1 thread model. The scheduler handles
     * every thread as a normal process. Therefore this probably has no meaning
     * if we are under Linux.
     */
    if (pthread_attr_setscope(&attr, PTHREAD_SCOPE_SYSTEM) != 0)
        ErrorF("input-thread: error setting thread scope\n");

    DebugF("input-thread: creating thread\n");
    pthread_create(&inputThreadInfo->thread, &attr,
                   &InputThreadDoWork, NULL);

    pthread_attr_destroy (&attr);
}

/**
 * Stop the threaded generation of input events
 *
 * This function is supposed to be called at server shutdown time only.
 */
void
InputThreadFini(void)
{
    InputThreadDevice *dev, *next;

    if (!inputThreadInfo)
        return;

    /* Close the pipe to get the input thread to shut down */
    close(hotplugPipeWrite);
    pthread_join(inputThreadInfo->thread, NULL);

    xorg_list_for_each_entry_safe(dev, next, &inputThreadInfo->devs, node) {
        FD_CLR(dev->fd, &inputThreadInfo->fds);
        free(dev);
    }
    xorg_list_init(&inputThreadInfo->devs);
    FD_ZERO(&inputThreadInfo->fds);

    RemoveNotifyFd(inputThreadInfo->readPipe);
    close(inputThreadInfo->readPipe);
    close(inputThreadInfo->writePipe);
    inputThreadInfo->readPipe = -1;
    inputThreadInfo->writePipe = -1;

    close(hotplugPipeRead);
    hotplugPipeRead = -1;
    hotplugPipeWrite = -1;

    free(inputThreadInfo);
    inputThreadInfo = NULL;
}

int xthread_sigmask(int how, const sigset_t *set, sigset_t *oldset)
{
    return pthread_sigmask(how, set, oldset);
}

#else /* INPUTTHREAD */

Bool InputThreadEnable = FALSE;

void input_lock(void) {}
void input_unlock(void) {}
void input_force_unlock(void) {}

void InputThreadPreInit(void) {}
void InputThreadInit(void) {}
void InputThreadFini(void) {}

int InputThreadRegisterDev(int fd,
                           NotifyFdProcPtr readInputProc,
                           void *readInputArgs)
{
    return SetNotifyFd(fd, readInputProc, X_NOTIFY_READ, readInputArgs);
}

extern int InputThreadUnregisterDev(int fd)
{
    RemoveNotifyFd(fd);
    return 1;
}

int xthread_sigmask(int how, const sigset_t *set, sigset_t *oldset)
{
    return sigprocmask(how, set, oldset);
}

#endif
