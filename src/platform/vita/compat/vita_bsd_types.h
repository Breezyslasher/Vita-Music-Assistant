/**
 * BSD compatibility force-include for building usrsctp on PS Vita.
 *
 * usrsctp's userspace build assumes BSD-flavoured libc headers. vitasdk's
 * newlib provides most of it, but only when BSD visibility is enabled, and a
 * few derived types are missing entirely. This header is force-included
 * (-include) into every usrsctp translation unit.
 */

#ifndef VITA_BSD_TYPES_H
#define VITA_BSD_TYPES_H

/* Enable BSD-visible definitions (u_char, u_int, caddr_t, timercmp, ...) in
 * newlib headers regardless of the -std level used to compile usrsctp. */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif
#ifndef _BSD_SOURCE
#define _BSD_SOURCE 1
#endif
#ifndef __BSD_VISIBLE
#define __BSD_VISIBLE 1
#endif

#include <sys/types.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <errno.h>

/* Constants newlib doesn't provide. usrsctp only compares against ERESTART
 * (never a case label), so any distinct value works; 85 matches Linux. */
#ifndef UIO_MAXIOV
#define UIO_MAXIOV 1024
#endif
#ifndef ERESTART
#define ERESTART 85
#endif
#ifndef SOMAXCONN
#define SOMAXCONN 128
#endif

#endif /* VITA_BSD_TYPES_H */
