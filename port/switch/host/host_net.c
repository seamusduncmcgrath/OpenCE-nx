/*
HOST_NET.C

port/linux/src/posix.h's sockets, and its functions about the process and
the desktop, for the Switch: what port/linux/src/posix_net.c is to the other
ports, with libnx's BSD sockets (the system's network stack is FreeBSD's).

The guest calls these as it does on Android. Two things differ from the
Linux sockets the other ports have:

- an address begins with its length and its family in a byte each, where
  Winsock (which the game's addresses are laid out for) and Linux have the
  family in two bytes: each address is rewritten on its way in and out;
- a datagram is received through a buffer of the host's, to tell when it
  was larger than the game's (there is no recvmsg to say so).

There is no UPnP (posix_upnp.c) and no Discord here.
*/

#include "posix.h"
#include "host.h"

#include <switch.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

/* Winsock error codes (winsockx.h) */
#define WSAEINTR 10004
#define WSAEBADF 10009
#define WSAEACCES 10013
#define WSAEFAULT 10014
#define WSAEINVAL 10022
#define WSAEMFILE 10024
#define WSAEWOULDBLOCK 10035
#define WSAEINPROGRESS 10036
#define WSAEALREADY 10037
#define WSAENOTSOCK 10038
#define WSAEDESTADDRREQ 10039
#define WSAEMSGSIZE 10040
#define WSAEPROTOTYPE 10041
#define WSAENOPROTOOPT 10042
#define WSAEPROTONOSUPPORT 10043
#define WSAEOPNOTSUPP 10045
#define WSAEAFNOSUPPORT 10047
#define WSAEADDRINUSE 10048
#define WSAEADDRNOTAVAIL 10049
#define WSAENETDOWN 10050
#define WSAENETUNREACH 10051
#define WSAENETRESET 10052
#define WSAECONNABORTED 10053
#define WSAECONNRESET 10054
#define WSAENOBUFS 10055
#define WSAEISCONN 10056
#define WSAENOTCONN 10057
#define WSAESHUTDOWN 10058
#define WSAETIMEDOUT 10060
#define WSAECONNREFUSED 10061
#define WSAEHOSTDOWN 10064
#define WSAEHOSTUNREACH 10065

/* Winsock's SOL_SOCKET and its options (winsockx.h), which are BSD's own
numbers: only those the port knows are passed on */
#define WINSOCK_SOL_SOCKET 0xffff
#define WINSOCK_SO_REUSEADDR 0x0004
#define WINSOCK_SO_KEEPALIVE 0x0008
#define WINSOCK_SO_BROADCAST 0x0020
#define WINSOCK_SO_LINGER 0x0080
#define WINSOCK_SO_SNDBUF 0x1001
#define WINSOCK_SO_RCVBUF 0x1002
#define WINSOCK_SO_ERROR 0x1007
#define WINSOCK_SO_TYPE 0x1008

/* the largest datagram there is */
#define DATAGRAM_MAXIMUM 65536

static __thread int last_error;

static int fail(void)
{
	switch (errno)
	{
	case EINTR: last_error = WSAEINTR; break;
	case EBADF: last_error = WSAEBADF; break;
	case EACCES: case EPERM: last_error = WSAEACCES; break;
	case EFAULT: last_error = WSAEFAULT; break;
	case EMFILE: case ENFILE: last_error = WSAEMFILE; break;
	case EAGAIN: last_error = WSAEWOULDBLOCK; break;
	case EINPROGRESS: last_error = WSAEINPROGRESS; break;
	case EALREADY: last_error = WSAEALREADY; break;
	case ENOTSOCK: last_error = WSAENOTSOCK; break;
	case EDESTADDRREQ: last_error = WSAEDESTADDRREQ; break;
	case EMSGSIZE: last_error = WSAEMSGSIZE; break;
	case EPROTOTYPE: last_error = WSAEPROTOTYPE; break;
	case ENOPROTOOPT: last_error = WSAENOPROTOOPT; break;
	case EPROTONOSUPPORT: last_error = WSAEPROTONOSUPPORT; break;
	case EOPNOTSUPP: last_error = WSAEOPNOTSUPP; break;
	case EAFNOSUPPORT: last_error = WSAEAFNOSUPPORT; break;
	case EADDRINUSE: last_error = WSAEADDRINUSE; break;
	case EADDRNOTAVAIL: last_error = WSAEADDRNOTAVAIL; break;
	case ENETDOWN: last_error = WSAENETDOWN; break;
	case ENETUNREACH: last_error = WSAENETUNREACH; break;
	case ENETRESET: last_error = WSAENETRESET; break;
	case ECONNABORTED: last_error = WSAECONNABORTED; break;
	/* a send on a connection the other end reset: Winsock's WSAECONNRESET,
	which the game takes as the connection lost */
	case ECONNRESET: case EPIPE: last_error = WSAECONNRESET; break;
	case ENOBUFS: case ENOMEM: last_error = WSAENOBUFS; break;
	case EISCONN: last_error = WSAEISCONN; break;
	case ENOTCONN: last_error = WSAENOTCONN; break;
	case ETIMEDOUT: last_error = WSAETIMEDOUT; break;
	case ECONNREFUSED: last_error = WSAECONNREFUSED; break;
	case EHOSTUNREACH: last_error = WSAEHOSTUNREACH; break;
	default: last_error = WSAEINVAL; break;
	}
	return -1;
}

static int succeed(int result)
{
	if (result < 0)
		return fail();
	last_error = 0;
	return result;
}

/* ---------- addresses */

/* the game's sockaddr_in (Winsock's, and Linux's) */
struct guest_address
{
	uint16_t family;
	uint16_t port;
	uint32_t address;
	uint8_t zero[8];
};

/* the host's address for one of the game's; NULL if it is none (or not IPv4) */
static const struct sockaddr *address_in(const void *guest, int length, struct sockaddr_in *host)
{
	const struct guest_address *address = guest;

	if (!guest || length < (int)sizeof(*address) || address->family != AF_INET)
		return NULL;
	memset(host, 0, sizeof(*host));
	host->sin_len = sizeof(*host);
	host->sin_family = AF_INET;
	host->sin_port = address->port;
	host->sin_addr.s_addr = address->address;
	return (const struct sockaddr *)host;
}

static void address_out(const struct sockaddr_in *host, void *guest, int *length)
{
	struct guest_address address;

	if (!guest || !length)
		return;
	memset(&address, 0, sizeof(address));
	address.family = AF_INET;
	address.port = host->sin_port;
	address.address = host->sin_addr.s_addr;
	if (*length > (int)sizeof(address))
		*length = sizeof(address);
	if (*length > 0)
		memcpy(guest, &address, (size_t)*length);
}

/* ---------- sockets */

int posix_socket_last_error(void)
{
	return last_error;
}

/* a failure the game's log does not tell of, or tells of as another (a
socket that was not made is a bad descriptor to every call after it): in the
program's log, the first few of them */
static void note_failure(const char *what, int value)
{
	static int notes;
	int error = errno;

	if (notes < 32)
	{
		notes++;
		host_logf(HOST_LOG_WARN, "sockets: %s %d: error %d (%s)", what, value, error, strerror(error));
	}
	errno = error;
}

int posix_socket(int family, int type, int protocol)
{
	int result = socket(family, type, protocol);

	if (result < 0)
		note_failure("no socket of type", type);
	return succeed(result);
}

int posix_socket_close(int socket)
{
	return succeed(close(socket));
}

int posix_socket_bind(int socket, const void *address, int address_length)
{
	struct sockaddr_in host;
	const struct sockaddr *name = address_in(address, address_length, &host);

	if (!name)
	{
		last_error = WSAEAFNOSUPPORT;
		return -1;
	}
	return succeed(bind(socket, name, sizeof(host)));
}

int posix_socket_connect(int socket, const void *address, int address_length)
{
	/* A non-blocking connect that is under way is EINPROGRESS here but
	WSAEWOULDBLOCK in Winsock, which is what the game waits on before it
	selects for the socket becoming writeable (posix_net.c) */
	struct sockaddr_in host;
	const struct sockaddr *name = address_in(address, address_length, &host);
	int result;

	if (!name)
	{
		last_error = WSAEAFNOSUPPORT;
		return -1;
	}
	result = connect(socket, name, sizeof(host));
	if (result < 0 && errno == EINPROGRESS)
	{
		last_error = WSAEWOULDBLOCK;
		return -1;
	}
	return succeed(result);
}

int posix_socket_listen(int socket, int backlog)
{
	int result = listen(socket, backlog);

	if (result < 0)
		note_failure("no listening on socket", socket);
	return succeed(result);
}

int posix_socket_accept(int socket, void *address, int *address_length)
{
	struct sockaddr_in host;
	socklen_t length = sizeof(host);
	int result;

	memset(&host, 0, sizeof(host));
	result = accept(socket, (struct sockaddr *)&host, &length);
	if (result >= 0)
	{
		address_out(&host, address, address_length);
		/* a connection taken from a socket that does not block does not
		block either here (BSD's rule); on Linux, which the game's code and
		internet play's are written to, it blocks until it is told not to */
		fcntl(result, F_SETFL, 0);
	}
	return succeed(result);
}

int posix_socket_send(int socket, const void *buffer, int length, int flags)
{
	return succeed((int)send(socket, buffer, (size_t)length, flags));
}

int posix_socket_sendto(int socket, const void *buffer, int length, int flags,
	const void *address, int address_length)
{
	struct sockaddr_in host;
	const struct sockaddr *name = address_in(address, address_length, &host);
	int result = (int)sendto(socket, buffer, (size_t)length, flags, name, name ? sizeof(host) : 0);

	/* a datagram with an address, from a socket that is connected: Linux
	sends it there; BSD refuses it (EISCONN). To the address the socket is
	connected to, which is where the game's go (a client's to its host), it
	is the same datagram without the address. */
	if (result < 0 && errno == EISCONN && name)
	{
		struct sockaddr_in peer;
		socklen_t peer_length = sizeof(peer);

		memset(&peer, 0, sizeof(peer));
		if (getpeername(socket, (struct sockaddr *)&peer, &peer_length) == 0 &&
			peer.sin_addr.s_addr == host.sin_addr.s_addr && peer.sin_port == host.sin_port)
		{
			result = (int)send(socket, buffer, (size_t)length, flags);
		}
		else
		{
			errno = EISCONN;
		}
	}
	return succeed(result);
}

int posix_socket_recv(int socket, void *buffer, int length, int flags)
{
	return succeed((int)recv(socket, buffer, (size_t)length, flags));
}

int posix_socket_recvfrom(int socket, void *buffer, int length, int flags,
	void *address, int *address_length)
{
	/* (each thread's own: internet play's thread and the game's both receive) */
	static __thread unsigned char *datagram;
	struct sockaddr_in host;
	socklen_t host_length = sizeof(host);
	int result;

	if (length < 0)
	{
		last_error = WSAEINVAL;
		return -1;
	}
	if (!datagram)
		datagram = malloc(DATAGRAM_MAXIMUM);
	memset(&host, 0, sizeof(host));
	if (!datagram || length >= DATAGRAM_MAXIMUM)
	{
		result = (int)recvfrom(socket, buffer, (size_t)length, flags, (struct sockaddr *)&host, &host_length);
	}
	else
	{
		result = (int)recvfrom(socket, datagram, DATAGRAM_MAXIMUM, flags, (struct sockaddr *)&host, &host_length);
		if (result > 0)
			memcpy(buffer, datagram, (size_t)(result < length ? result : length));
	}
	if (result >= 0 && host_length >= sizeof(host) && host.sin_family == AF_INET)
		address_out(&host, address, address_length);
	else if (result >= 0 && address_length)
		*address_length = 0;
	/* a datagram larger than the buffer: both give its start, but Winsock
	with WSAEMSGSIZE, which the game takes as an error, not as the datagram */
	if (result > length)
	{
		last_error = WSAEMSGSIZE;
		return -1;
	}
	return succeed(result);
}

int posix_socket_shutdown(int socket, int how)
{
	return succeed(shutdown(socket, how));
}

int posix_socket_set_nonblocking(int socket, int nonblocking)
{
	int flags = fcntl(socket, F_GETFL, 0);

	if (flags < 0)
		return fail();
	flags = nonblocking ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
	return succeed(fcntl(socket, F_SETFL, flags));
}

int posix_socket_set_nodelay(int socket)
{
	int value = 1;

	return succeed(setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, &value, sizeof(value)));
}

int posix_socket_bytes_available(int socket, posix_ulong *count)
{
	int available = 0;
	int result = ioctl(socket, FIONREAD, &available);

	if (result < 0)
	{
		/* (without the request: what a look at the data finds) */
		static __thread unsigned char *scratch;

		if (!scratch)
			scratch = malloc(DATAGRAM_MAXIMUM);
		result = scratch ? (int)recv(socket, scratch, DATAGRAM_MAXIMUM, MSG_PEEK | MSG_DONTWAIT) : -1;
		if (result < 0 && errno == EAGAIN)
			result = 0;
		available = result;
	}
	if (result >= 0)
		*count = (posix_ulong)available;
	return succeed(result < 0 ? -1 : 0);
}

static int known_option(int level, int name)
{
	if (level != WINSOCK_SOL_SOCKET)
		return 1;
	switch (name)
	{
	case WINSOCK_SO_REUSEADDR:
	case WINSOCK_SO_KEEPALIVE:
	case WINSOCK_SO_BROADCAST:
	case WINSOCK_SO_LINGER:
	case WINSOCK_SO_SNDBUF:
	case WINSOCK_SO_RCVBUF:
	case WINSOCK_SO_ERROR:
	case WINSOCK_SO_TYPE:
		return 1;
	default:
		return 0;
	}
}

/* The game asks for buffers of 1 MB each way for each of its datagram
sockets (transport_endpoint_winsock.c), and internet play for its tunnel's
(p2p.c). Linux gives such a request what its limit allows (about 200 KB)
and says nothing. Here all the sockets' buffers come out of one pool
(host_main.c), so a request gets no more than this, and if the pool has not
that, what it has; the socket keeps the buffer it had at the least. */
#define BUFFER_MAXIMUM (256 * 1024)

static int set_buffer_size(int socket, int name, const void *value, int length)
{
	int wanted, size, current = 0;
	socklen_t current_length = sizeof(current);
	int result;

	if (!value || length < (int)sizeof(wanted))
	{
		errno = EINVAL;
		return -1;
	}
	memcpy(&wanted, value, sizeof(wanted));
	size = wanted > BUFFER_MAXIMUM ? BUFFER_MAXIMUM : wanted;
	if (getsockopt(socket, SOL_SOCKET, name, &current, &current_length) < 0)
		return -1;
	result = setsockopt(socket, SOL_SOCKET, name, &size, sizeof(size));
	while (result < 0 && errno == ENOBUFS && size / 2 > current)
	{
		size /= 2;
		result = setsockopt(socket, SOL_SOCKET, name, &size, sizeof(size));
	}
	if (result < 0 && errno == ENOBUFS)
	{
		note_failure("the pool has no larger buffer for socket", socket);
		result = 0;
	}
	return result;
}

int posix_socket_setsockopt(int socket, int level, int name, const void *value, int length)
{
	if (!known_option(level, name))
	{
		/* Xbox-only options such as SO_ENCRYPT have nothing to do here */
		last_error = 0;
		return 0;
	}
	if (level == WINSOCK_SOL_SOCKET && (name == WINSOCK_SO_SNDBUF || name == WINSOCK_SO_RCVBUF))
		return succeed(set_buffer_size(socket, name, value, length));
	return succeed(setsockopt(socket, level, name, value, (socklen_t)length));
}

int posix_socket_getsockopt(int socket, int level, int name, void *value, int *length)
{
	socklen_t socket_length = (socklen_t)*length;
	int result;

	if (!known_option(level, name))
	{
		last_error = WSAENOPROTOOPT;
		return -1;
	}
	result = getsockopt(socket, level, name, value, &socket_length);
	*length = (int)socket_length;
	return succeed(result);
}

int posix_socket_getsockname(int socket, void *address, int *address_length)
{
	struct sockaddr_in host;
	socklen_t length = sizeof(host);
	int result;

	memset(&host, 0, sizeof(host));
	result = getsockname(socket, (struct sockaddr *)&host, &length);
	if (result >= 0)
		address_out(&host, address, address_length);
	return succeed(result);
}

int posix_socket_getpeername(int socket, void *address, int *address_length)
{
	struct sockaddr_in host;
	socklen_t length = sizeof(host);
	int result;

	memset(&host, 0, sizeof(host));
	result = getpeername(socket, (struct sockaddr *)&host, &length);
	if (result >= 0)
		address_out(&host, address, address_length);
	return succeed(result);
}

int posix_socket_select(int *read, int *read_count, int *write, int *write_count,
	int *error, int *error_count, posix_long timeout_seconds, posix_long timeout_microseconds, int infinite)
{
	/* poll, with select's readiness (posix_net.c) */
	static const short events[3] = { POLLIN, POLLOUT, POLLPRI };
	static const short ready[3] = { POLLIN | POLLHUP | POLLERR, POLLOUT | POLLERR, POLLPRI | POLLERR };
	static __thread struct pollfd *buffer;
	static __thread int buffer_size;
	int *lists[3] = { read, write, error };
	int *counts[3] = { read_count, write_count, error_count };
	struct pollfd stack[64];
	struct pollfd *descriptors = stack;
	long long milliseconds = (long long)timeout_seconds * 1000 + ((long long)timeout_microseconds + 999) / 1000;
	int total = 0;
	int list, index;
	int result;

	for (list = 0; list < 3; list++)
	{
		if (!lists[list] || !counts[list])
			lists[list] = NULL;
		else
			total += *counts[list];
	}
	if (total > (int)(sizeof(stack) / sizeof(*stack)))
	{
		if (total > buffer_size)
		{
			struct pollfd *larger = realloc(buffer, sizeof(*buffer) * (size_t)total);

			if (!larger)
			{
				errno = ENOMEM;
				return fail();
			}
			buffer = larger;
			buffer_size = total;
		}
		descriptors = buffer;
	}
	total = 0;
	for (list = 0; list < 3; list++)
	{
		for (index = 0; lists[list] && index < *counts[list]; index++, total++)
		{
			descriptors[total].fd = lists[list][index];
			descriptors[total].events = events[list];
			descriptors[total].revents = 0;
		}
	}
	if (!total)
	{
		/* (nothing to wait for but the time) */
		if (!infinite && milliseconds > 0)
			svcSleepThread(milliseconds * 1000000LL);
		return 0;
	}
	result = poll(descriptors, (nfds_t)total, infinite ? -1 :
		(int)(milliseconds < 0 ? 0 : milliseconds > INT_MAX ? INT_MAX : milliseconds));
	for (index = 0; index < total && result > 0; index++)
	{
		/* (as select fails on a descriptor that is not open) */
		if (descriptors[index].revents & POLLNVAL)
		{
			errno = EBADF;
			result = -1;
		}
	}
	if (result < 0)
		return fail();
	result = 0;
	total = 0;
	for (list = 0; list < 3; list++)
	{
		int kept = 0;

		for (index = 0; lists[list] && index < *counts[list]; index++, total++)
		{
			int descriptor = lists[list][index];
			int pending = 0;
			socklen_t length = sizeof(pending);

			if (!(descriptors[total].revents & ready[list]))
				continue;
			/* Winsock reports a socket writeable once its connect has
			succeeded; one whose connect failed is not (posix_net.c) */
			if (list == 1 && getsockopt(descriptor, SOL_SOCKET, SO_ERROR, &pending, &length) == 0 && pending)
			{
				errno = pending;
				fail();
				continue;
			}
			lists[list][kept++] = descriptor;
		}
		if (lists[list])
			*counts[list] = kept;
		result += kept;
	}
	/* like Winsock, a select with nothing ready leaves the last error as it
	was */
	if (result > 0)
		last_error = 0;
	return result;
}

posix_ulong posix_local_ipv4_address(void)
{
	/* the address of the console's connection, from the system; asked again
	after a moment only (each time is a call to another process) */
	static Mutex lock;
	static int initialized;
	static u64 asked;
	static u32 address;
	u64 now = armGetSystemTick();
	posix_ulong result;

	mutexLock(&lock);
	if (!initialized)
		initialized = R_SUCCEEDED(nifmInitialize(NifmServiceType_User)) ? 1 : -1;
	if (initialized > 0 && (!asked || armTicksToNs(now - asked) > 2000000000ULL))
	{
		u32 current = 0;

		address = R_SUCCEEDED(nifmGetCurrentIpAddress(&current)) ? current : 0;
		asked = now ? now : 1;
	}
	result = (ntohl(address) >> 24) == 127 ? 0 : (posix_ulong)address;
	mutexUnlock(&lock);
	return result;
}

void posix_random_bytes(void *buffer, posix_ulong size)
{
	/* (libnx's generator, seeded by the kernel) */
	randomGet(buffer, size);
}

posix_ulong posix_resolve_ipv4(const char *host)
{
	struct addrinfo hints, *results = NULL;
	posix_ulong address = 0;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_DGRAM;
	if (getaddrinfo(host, NULL, &hints, &results) != 0)
		return 0;
	if (results && results->ai_addr && results->ai_addr->sa_family == AF_INET)
		address = ((struct sockaddr_in *)results->ai_addr)->sin_addr.s_addr;
	if (results)
		freeaddrinfo(results);
	return address;
}

/* ---------- UPnP: not on the Switch (internet play makes its connections
without it, as where a router has none) */

int posix_upnp_forward_udp(unsigned short port, unsigned short preferred_port, posix_ulong *external_address,
	unsigned short *external_port, char *error, int error_size)
{
	(void)port;
	(void)preferred_port;
	(void)external_address;
	(void)external_port;
	if (error && error_size > 0)
		snprintf(error, (size_t)error_size, "%s", "UPnP is not available on the Switch");
	return 0;
}

void posix_upnp_stop_forwarding_udp(unsigned short external_port)
{
	(void)external_port;
}

/* ---------- the process and the desktop: as on Android, there is none */

int posix_command_line_argument(int index, char *buffer, posix_ulong size)
{
	(void)index;
	(void)buffer;
	(void)size;
	return 0;
}

posix_ulong posix_process_id(void)
{
	u64 id = 0;

	svcGetProcessId(&id, CUR_PROCESS_HANDLE);
	return (posix_ulong)id;
}

int posix_register_url_scheme(const char *scheme, const char *description)
{
	(void)scheme;
	(void)description;
	return 0;
}

int posix_user_secret(unsigned char *secret, int size)
{
	(void)secret;
	(void)size;
	return 0;
}

int posix_discord_connect(void)
{
	return -1;
}

int posix_discord_write(int handle, const void *buffer, int length)
{
	(void)handle;
	(void)buffer;
	(void)length;
	return -1;
}

int posix_discord_read(int handle, void *buffer, int length)
{
	(void)handle;
	(void)buffer;
	(void)length;
	return -1;
}

void posix_discord_close(int handle)
{
	(void)handle;
}
