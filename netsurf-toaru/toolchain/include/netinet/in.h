#ifndef TOARU_SHIM_NETINET_IN_H
#define TOARU_SHIM_NETINET_IN_H
#include_next <netinet/in.h>
#ifndef AF_INET6
#define AF_INET6 10
#endif
#ifndef IPPROTO_IPV6
#define IPPROTO_IPV6 41
#endif
#ifndef in6_addr
struct in6_addr { unsigned char s6_addr[16]; };
#endif
#endif
