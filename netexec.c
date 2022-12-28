/*
 * Copyright (c) 2022 Ali Farzanrad <ali_farzanrad@riseup.net>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
 * WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS.  IN NO EVENT SHALL THE
 * AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
 * DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR
 * PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>

#include <netinet/in.h>
#include <arpa/inet.h>

#include <err.h>
#include <errno.h>
#include <netdb.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

enum {
	Server = STDIN_FILENO,
	Client = STDOUT_FILENO
};

void		 on_signal(int);
void		 listen_unix(const char *);
void		 listen_tcp(const char *, const char *);
void		 dumpservername(void);
const char	*getsocknamestr(int, char *, size_t);

int
main(int argc, char **argv)
{
	const char	*host = "127.0.0.1", *port = NULL;

	close(Server);
	close(Client);
	if (signal(SIGCHLD, on_signal) == SIG_ERR ||
	    signal(SIGINT, on_signal) == SIG_ERR ||
	    signal(SIGTERM, on_signal) == SIG_ERR)
		err(1, "signal");
	argc -= 1;
	argv += 1;
	if (argc > 0 && strcmp(argv[0], "--") != 0) {
		host = argv[0];
		--argc;
		++argv;
	}
	if (argc > 0 && strcmp(argv[0], "--") != 0) {
		port = argv[0];
		if (strcmp(port, "auto") == 0)
			port = NULL;
		--argc;
		++argv;
	}
	if (argc > 0 && strcmp(argv[0], "--") == 0) {
		--argc;
		++argv;
	}
	if (argc < 1 || (port && host[0] == '/'))
		errx(1, "usage: netexec [host [port]] -- cmd args ...");
	if (host[0] == '/')
		listen_unix(host);
	else
		listen_tcp(host, port);
	dumpservername();
	for (;;) {
		struct sockaddr	 addr;
		socklen_t	 len = sizeof(addr);
		const int	 s = accept(Server, &addr, &len);
		if (s != Client) {
			if (s != -1) {
				warnx("accept: bad file descriptor");
				shutdown(s, SHUT_RDWR);
				close(s);
			} else if (errno == EBADF)
				return 0;
			else
				warn("accept");
		}
		switch (fork()) {
		case -1:
			warn("fork");
			shutdown(Client, SHUT_RDWR);
			close(Client);
			break;
		case 0:
			if (dup2(Client, Server) == -1) {
				warn("dup2");
				shutdown(Client, SHUT_RDWR);
				return 1;
			}
			if (execvp(argv[0], argv) == -1)
				warn("execvp");
			else
				warnx("execvp: returned!");
			shutdown(Client, SHUT_RDWR);
			return 1;
		default:
			if (close(Client))
				warn("close");
		}
	}
	return 0;
}

void
on_signal(int sigraised)
{
	int		 status;

	switch (sigraised) {
	case SIGCHLD:
		if (wait(&status) == -1)
			warn("wait");
		if (status)
			warnx("child exit code: %d", status);
		break;
	case SIGINT:
	case SIGTERM:
		shutdown(Server, SHUT_RDWR);
		close(Server);
		break;
	}
}

void
listen_unix(const char *path)
{
	struct sockaddr_un addr;
	int		 s;
	const size_t	 path_size = sizeof(addr.sun_path);

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	if (strlcpy(addr.sun_path, path, path_size) >= path_size)
		errx(1, "path too long");
	if (unlink(path) == -1 && errno != ENOENT)
		errx(1, "invalid unix socket path");
	if ((s = socket(AF_UNIX, SOCK_STREAM, 0)) != Server) {
		if (s == -1)
			err(1, "socket");
		errx(1, "socket: FATAL");
	}
	if (bind(s, (struct sockaddr *)&addr, sizeof(addr)))
		err(1, "bind");
	if (listen(s, 5))
		err(1, "listen");
}

void
listen_tcp(const char *host, const char *port)
{
	struct addrinfo	 hints, *res;
	int		 s, error;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE;
	error = getaddrinfo(host, port, &hints, &res);
	if (error)
		errx(1, "getaddrinfo: %s", gai_strerror(error));
	if (res == NULL)
		errx(1, "no address to listen");
	if (res->ai_next != NULL)
		warnx("many address to listen");
	s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
	if (s != Server) {
		if (s == -1)
			err(1, "socket");
		errx(1, "socket: FATAL");
	}
	for (int i = 0; bind(s, res->ai_addr, res->ai_addrlen); ) {
		if (errno != EADDRINUSE || ++i >= 60)
			err(1, "bind");
		sleep(1);
	}
	freeaddrinfo(res);
	if (listen(s, 5))
		err(1, "listen");
}

void
dumpservername(void)
{
	char		 buf[256];
	if (getsocknamestr(Server, buf, sizeof(buf))) {
		fprintf(stderr, "listening on %s\n", buf);
		fflush(stderr);
	}
}

const char *
getsocknamestr(int s, char *res, size_t size)
{
	union {
		struct sockaddr		 sa;
		struct sockaddr_in	 sin;
		struct sockaddr_in6	 sin6;
		struct sockaddr_un	 sun;
	} a;
	socklen_t		 len = sizeof(a);

	if (getsockname(s, &a.sa, &len)) {
		warn("getsockname");
		return NULL;
	}
	switch (a.sa.sa_family) {
	case AF_UNIX:
		len = strnlen(a.sun.sun_path, sizeof(a.sun.sun_path));
		if (len >= size)
			len = size - 1;
		memcpy(res, a.sun.sun_path, len);
		res[len] = 0;
		return res;
	case AF_INET:
		if (!inet_ntop(AF_INET, &a.sin.sin_addr, res, size)) {
			warn("inet_ntop");
			return NULL;
		}
		len = strlen(res);
		snprintf(res + len, size - len, ":%u",
		    (unsigned)ntohs(a.sin.sin_port));
		return res;
	case AF_INET6:
		if (!inet_ntop(AF_INET6, &a.sin6.sin6_addr, res, size)) {
			warn("inet_ntop");
			return NULL;
		}
		len = strlen(res);
		snprintf(res + len, size - len, ":%u",
		    (unsigned)ntohs(a.sin6.sin6_port));
		return res;
	default:
		warnx("socknamestr: unknown address family");
		return NULL;
	}
}
