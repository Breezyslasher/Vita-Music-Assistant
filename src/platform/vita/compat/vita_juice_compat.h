/**
 * Force-included into every libjuice translation unit on PS Vita.
 *
 * - Marks the platform little-endian so picohash.h doesn't reach for the
 *   glibc-only <endian.h> (vitasdk newlib has none; the Vita CPU is ARM LE).
 * - Provides IPv6 socket option constants that vitasdk headers may lack.
 *   The Vita has no IPv6 support: AF_INET6 sockets fail at creation, so
 *   these setsockopt calls are never reached - the constants only need to
 *   exist for compilation.
 */

#ifndef VITA_JUICE_COMPAT_H
#define VITA_JUICE_COMPAT_H

#ifndef __LITTLE_ENDIAN__
#define __LITTLE_ENDIAN__ 1
#endif

#include <netinet/in.h>

#ifndef IPV6_V6ONLY
#define IPV6_V6ONLY 27
#endif
#ifndef IPV6_TCLASS
#define IPV6_TCLASS 67
#endif
#ifndef IPV6_UNICAST_HOPS
#define IPV6_UNICAST_HOPS 4
#endif

#endif /* VITA_JUICE_COMPAT_H */
