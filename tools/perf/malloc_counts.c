/* Linux/glibc diagnostic interposer. Never use its elapsed time as a benchmark. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

extern void *__libc_malloc(size_t);
extern void *__libc_calloc(size_t, size_t);
extern void *__libc_realloc(void *, size_t);
static _Atomic unsigned long long calls;

void *malloc(size_t size)
{
    atomic_fetch_add_explicit(&calls, 1, memory_order_relaxed);
    return __libc_malloc(size);
}
void *calloc(size_t count, size_t size)
{
    atomic_fetch_add_explicit(&calls, 1, memory_order_relaxed);
    return __libc_calloc(count, size);
}
void *realloc(void *pointer, size_t size)
{
    atomic_fetch_add_explicit(&calls, 1, memory_order_relaxed);
    return __libc_realloc(pointer, size);
}
__attribute__((destructor)) static void report_counts(void)
{
    const unsigned long long value = atomic_load_explicit(&calls, memory_order_relaxed);
    const char *path = getenv("CHAMPSIM_MALLOC_COUNTS");
    if (path == NULL)
        return;
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd >= 0) {
        char buffer[100];
        int size = snprintf(buffer, sizeof buffer, "{\"allocation_calls\": %llu}\n", value);
        if (size > 0 && (size_t)size < sizeof buffer && write(fd, buffer, (size_t)size) != size)
            (void)unlink(path);
        close(fd);
    }
}
