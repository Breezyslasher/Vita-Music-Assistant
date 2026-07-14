/**
 * Minimal <sys/uio.h> for PS Vita (vitasdk newlib has none).
 * usrsctp only needs struct iovec (scatter/gather descriptors for its
 * userspace API); it never calls readv()/writev().
 */

#ifndef VITA_COMPAT_SYS_UIO_H
#define VITA_COMPAT_SYS_UIO_H

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

struct iovec {
    void  *iov_base;
    size_t iov_len;
};

ssize_t readv(int fd, const struct iovec *iov, int iovcnt);
ssize_t writev(int fd, const struct iovec *iov, int iovcnt);

#ifdef __cplusplus
}
#endif

#endif /* VITA_COMPAT_SYS_UIO_H */
