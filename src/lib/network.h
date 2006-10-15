#ifndef __NETWORK_H
#define __NETWORK_H

#ifndef WIN32
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <netdb.h>
#  include <arpa/inet.h>
#endif

#ifdef HAVE_SOCKS_H
#include <socks.h>
#endif

#ifndef AF_INET6
#  ifdef PF_INET6
#    define AF_INET6 PF_INET6
#  else
#    define AF_INET6 10
#  endif
#endif

struct ip_addr {
	unsigned short family;
#ifdef HAVE_IPV6
	struct in6_addr ip;
#else
	struct in_addr ip;
#endif
};

/* maxmimum string length of IP address */
#ifdef HAVE_IPV6
#  define MAX_IP_LEN INET6_ADDRSTRLEN
#else
#  define MAX_IP_LEN 20
#endif

#define IPADDR_IS_V4(ip) ((ip)->family == AF_INET)
#define IPADDR_IS_V6(ip) ((ip)->family == AF_INET6)

/* returns 1 if IPADDRs are the same */
bool net_ip_compare(const struct ip_addr *ip1, const struct ip_addr *ip2);

/* Connect to socket with ip address */
int net_connect_ip(const struct ip_addr *ip, unsigned int port,
		   const struct ip_addr *my_ip);
/* Connect to named UNIX socket */
int net_connect_unix(const char *path);
/* Disconnect socket */
void net_disconnect(int fd);

/* Set socket blocking/nonblocking */
void net_set_nonblock(int fd, bool nonblock);
/* Set TCP_CORK if supported, ie. don't send out partial frames.
   Returns 0 if ok, -1 if failed. */
int net_set_cork(int fd, bool cork);

/* Set IP to contain INADDR_ANY for IPv4 or IPv6. The IPv6 any address may
   include IPv4 depending on the system (Linux yes, BSD no). */
void net_get_ip_any4(struct ip_addr *ip);
void net_get_ip_any6(struct ip_addr *ip);

/* Listen for connections on a socket */
int net_listen(const struct ip_addr *my_ip, unsigned int *port, int backlog);
/* Listen for connections on an UNIX socket */
int net_listen_unix(const char *path, int backlog);
/* Accept a connection on a socket. Returns -1 for temporary failure,
   -2 for fatal failure */
int net_accept(int fd, struct ip_addr *addr, unsigned int *port);

/* Read data from socket, return number of bytes read,
   -1 = error, -2 = disconnected */
ssize_t net_receive(int fd, void *buf, size_t len);
/* Transmit data, return number of bytes sent, -1 = error, -2 = disconnected */
ssize_t net_transmit(int fd, const void *data, size_t len);

/* Get IP addresses for host. ips contains ips_count of IPs, they don't need
   to be free'd. Returns 0 = ok, others = error code for net_gethosterror() */
int net_gethostbyname(const char *addr, struct ip_addr **ips, int *ips_count);
/* get error of net_gethostname() */
const char *net_gethosterror(int error);
/* return TRUE if host lookup failed because it didn't exist (ie. not
   some error with name server) */
int net_hosterror_notfound(int error);

/* Get socket local address/port */
int net_getsockname(int fd, struct ip_addr *addr, unsigned int *port);
/* Get socket remote address/port */
int net_getpeername(int fd, struct ip_addr *addr, unsigned int *port);

/* Returns ip_addr as string, or NULL if ip is invalid. */
const char *net_ip2addr(const struct ip_addr *ip);
/* char* -> struct ip_addr translation. */
int net_addr2ip(const char *addr, struct ip_addr *ip);
/* Convert IPv6 mapped IPv4 address to an actual IPv4 address. Returns 0 if
   successful, -1 if the source address isn't IPv6 mapped IPv4 address. */
int net_ipv6_mapped_ipv4_convert(const struct ip_addr *src,
				 struct ip_addr *dest);

/* Get socket error */
int net_geterror(int fd);

/* Get name of TCP service */
const char *net_getservbyport(unsigned short port);

bool is_ipv4_address(const char *addr);
bool is_ipv6_address(const char *addr);

#endif
