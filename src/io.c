#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE _POSIX_C_SOURCE
#endif

#ifndef __BSD_VISIBLE
#define __BSD_VISIBLE 1
#endif

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>

#include "config.h"
#include "src/io.h"
#include "utils/utils.h"

int
sendf(char *a, struct server *b, const char *c, ...)
{
	(void)a;
	(void)b;
	(void)c;
	fatal("Not implemented", 0);
	return 0;
}

void
server_disconnect(struct server *a, int b, int c, char *d)
{
	(void)a;
	(void)b;
	(void)c;
	(void)d;
	fatal("Not implemented", 0);
}

/* RFC 2812, section 2.3 */
#define IO_MESG_LEN 512

#ifndef IO_PING_MIN
	#define IO_PING_MIN 150
#elif (IO_PING_MIN < 0 || IO_PING_MIN > 86400)
	#error "IO_PING_MIN: [0, 86400]"
#endif

#ifndef IO_PING_REFRESH
	#define IO_PING_REFRESH 5
#elif (IO_PING_REFRESH < 0 || IO_PING_REFRESH > 86400)
	#error "IO_PING_REFRESH: [0, 86400]"
#endif

#ifndef IO_PING_MAX
	#define IO_PING_MAX 300
#elif (IO_PING_MAX < 0 || IO_PING_MAX > 86400)
	#error "IO_PING_MAX: [0, 86400]"
#endif

#ifndef IO_RECONNECT_BACKOFF_BASE
	#define IO_RECONNECT_BACKOFF_BASE 4
#elif (IO_RECONNECT_BACKOFF_BASE < 1 || IO_RECONNECT_BACKOFF_BASE > 86400)
	#error "IO_RECONNECT_BACKOFF_BASE: [1, 32]"
#endif

#ifndef IO_RECONNECT_BACKOFF_FACTOR
	#define IO_RECONNECT_BACKOFF_FACTOR 2
#elif (IO_RECONNECT_BACKOFF_FACTOR < 1 || IO_RECONNECT_BACKOFF_FACTOR > 32)
	#error "IO_RECONNECT_BACKOFF_FACTOR: [1, 32]"
#endif

#ifndef IO_RECONNECT_BACKOFF_MAX
	#define IO_RECONNECT_BACKOFF_MAX 86400
#elif (IO_RECONNECT_BACKOFF_MAX < 1 || IO_RECONNECT_BACKOFF_MAX > 86400)
	#error "IO_RECONNECT_BACKOFF_MAX: [0, 86400]"
#endif

#define PT_CF(X) do { int ret; if ((ret = (X)) != 0) fatal((#X), ret); } while (0)
#define PT_LK(X) PT_CF(pthread_mutex_lock((X)))
#define PT_UL(X) PT_CF(pthread_mutex_unlock((X)))
#define PT_CB(X) do { PT_LK(&cb_mutex); (X); PT_UL(&cb_mutex); } while (0)

enum io_err_t
{
	IO_ERR_NONE,
	IO_ERR_TRUNC,
	IO_ERR_DXED,
	IO_ERR_CXNG,
	IO_ERR_CXED
};

struct io_lock
{
	pthread_cond_t  cnd;
	pthread_mutex_t mtx;
	volatile int predicate;
};

struct connection
{
	const void *obj;
	const char *host;
	const char *port;
	enum io_state_t {
		IO_ST_INVALID,
		IO_ST_INIT, /* Initial thread state */
		IO_ST_DXED, /* Socket disconnected, passive */
		IO_ST_RXNG, /* Socket disconnected, pending reconnect */
		IO_ST_CXNG, /* Socket connection in progress */
		IO_ST_CXED, /* Socket connected */
		IO_ST_PING, /* Socket connected, network state in question */
		IO_ST_TERM, /* Terminal thread state */
		IO_ST_SIZE
	} state;
	int soc;

	// FIXME: remove
	pthread_mutex_t pt_state_mutex;

	pthread_t       pt_tid;
	struct {
		size_t i;
		char buf[IO_MESG_LEN];
	} read;
	struct io_lock lock;
	unsigned rx_backoff;
};

static char* io_strerror(int, char*, size_t);
static void io_close(struct connection*);
static void io_init(void);
static void io_init_sig(void);
static void io_init_tty(void);
static void io_term(void);
static void io_term_tty(void);
static void* io_thread(void*);

static void io_recv(struct connection*, char*, size_t);

static enum io_state_t io_state_init(struct connection*);
static enum io_state_t io_state_dxed(struct connection*);
static enum io_state_t io_state_rxng(struct connection*);
static enum io_state_t io_state_cxng(struct connection*);
static enum io_state_t io_state_cxed(struct connection*);
static enum io_state_t io_state_ping(struct connection*);
static enum io_state_t io_state_term(struct connection*);

static struct io_lock init_lock;

// TODO
static pthread_mutex_t cb_mutex;
static pthread_once_t init_once = PTHREAD_ONCE_INIT;
static struct termios term;
static volatile sig_atomic_t flag_sigwinch;

static void io_lock_init(struct io_lock*);
static void io_lock_term(struct io_lock*);
static void io_lock_wake(struct io_lock*);
static void io_lock_wait(struct io_lock*, struct timespec*);

static void
io_close(struct connection *c)
{
	/* Lock thread state to prevent ambiguous handling of EINTR on close() */
	if (c->soc >= 0) {
		PT_LK(&(c->pt_state_mutex));
		if (close(c->soc) < 0)
			fatal("close", errno);
		PT_UL(&(c->pt_state_mutex));
		c->soc = -1;
	}
}

static char*
io_strerror(int errnum, char *buf, size_t buflen)
{
	PT_CF(strerror_r(errnum, buf, buflen));
	return buf;
}

static void
io_init(void)
{
	// TODO: this is just io_lock_init, no? and io_lock_term?
	pthread_mutexattr_t m_attr;

	PT_CF(pthread_mutexattr_init(&m_attr));
	PT_CF(pthread_mutexattr_settype(&m_attr, PTHREAD_MUTEX_ERRORCHECK));

	PT_CF(pthread_cond_init(&init_lock.cnd, NULL));
	PT_CF(pthread_mutex_init(&cb_mutex, &m_attr));
	PT_CF(pthread_mutex_init(&init_lock.mtx, &m_attr));

	PT_CF(pthread_mutexattr_destroy(&m_attr));

	if (atexit(io_term) != 0)
		fatal("atexit", 0);
}

static void
io_term(void)
{
	int ret;

	if ((ret = pthread_mutex_trylock(&cb_mutex)) < 0 && ret != EBUSY)
		fatal("pthread_mutex_trylock", ret);

	PT_UL(&cb_mutex);
	PT_CF(pthread_cond_destroy(&init_lock.cnd));
	PT_CF(pthread_mutex_destroy(&cb_mutex));
	PT_CF(pthread_mutex_destroy(&init_lock.mtx));
}

static void
io_lock_init(struct io_lock *lock)
{
	pthread_mutexattr_t m_attr;

	PT_CF(pthread_mutexattr_init(&m_attr));
	PT_CF(pthread_mutexattr_settype(&m_attr, PTHREAD_MUTEX_ERRORCHECK));

	PT_CF(pthread_cond_init(&(lock->cnd), NULL));
	PT_CF(pthread_mutex_init(&(lock->mtx), &m_attr));

	PT_CF(pthread_mutexattr_destroy(&m_attr));

	lock->predicate = 0;
}

static void
io_lock_term(struct io_lock *lock)
{
	PT_CF(pthread_cond_destroy(&(lock->cnd)));
	PT_CF(pthread_mutex_destroy(&(lock->mtx)));
}

static void
io_lock_wait(struct io_lock *lock, struct timespec *timeout)
{
	PT_LK(&(lock->mtx));

	int ret = 0;

	while (!lock->predicate && ret == 0) {
		if (timeout) {
			ret = pthread_cond_timedwait(&(lock->cnd), &(lock->mtx), timeout);
		} else {
			ret = pthread_cond_wait(&(lock->cnd), &(lock->mtx));
		}
	}

	if (ret && (!timeout || ret != ETIMEDOUT))
		fatal("io_lock_wait", ret);

	lock->predicate = 0;

	PT_UL(&(lock->mtx));
}

static void
io_lock_wake(struct io_lock *lock)
{
	lock->predicate = 1;
	PT_CF(pthread_cond_signal(&(lock->cnd)));
}

struct connection*
connection(const void *obj, const char *host, const char *port)
{
	struct connection *c;

	if ((c = calloc(1U, sizeof(*c))) == NULL)
		fatal("malloc", errno);

	c->obj   = obj;
	c->host  = strdup(host);
	c->port  = strdup(port);
	c->state = IO_ST_INIT;
	io_lock_init(&(c->lock));

	PT_CF(pthread_once(&init_once, io_init));

	pthread_attr_t t_attr;

	PT_CF(pthread_attr_init(&t_attr));
	PT_CF(pthread_attr_setdetachstate(&t_attr, PTHREAD_CREATE_DETACHED));
	PT_CF(pthread_create(&(c->pt_tid), &t_attr, io_thread, c));
	PT_CF(pthread_attr_destroy(&t_attr));

	io_lock_wait(&init_lock, NULL);

	return c;
}

int
io_sendf(struct connection *c, const char *fmt, ...)
{
	char sendbuf[512];

	va_list ap;
	int ret;
	size_t len;

	if (c->state != IO_ST_CXED && c->state != IO_ST_PING)
		return IO_ERR_DXED;

	va_start(ap, fmt);
	ret = vsnprintf(sendbuf, sizeof(sendbuf) - 2, fmt, ap);
	va_end(ap);

	if (ret <= 0) {
		return IO_ERR_NONE; /* TODO handle error */
	}

	len = ret;

	if (len >= sizeof(sendbuf) - 2)
		return IO_ERR_TRUNC;

	sendbuf[len++] = '\r';
	sendbuf[len++] = '\n';

	if (send(c->soc, sendbuf, len, 0) < 0) {
		return IO_ERR_NONE; /* TODO: handle error */
	}

	return IO_ERR_NONE;
}

int
io_cx(struct connection *c)
{
	/* Force a socket thread into IO_ST_CXNG state
	 *
	 * Valid only for states blocked on:
	 *   - IO_ST_INIT: pthread_cond_wait()
	 *   - IO_ST_DXED: pthread_cond_wait()
	 *   - IO_ST_RXNG: pthread_cond_timedwait()
	 */

	enum io_err_t ret = IO_ERR_NONE;

	PT_LK(&(c->pt_state_mutex));

	switch (c->state) {
		case IO_ST_INIT:
		case IO_ST_DXED:
		case IO_ST_RXNG:
			io_lock_wake(&c->lock);
			break;
		case IO_ST_CXNG:
			ret = IO_ERR_CXNG;
			break;
		case IO_ST_CXED:
		case IO_ST_PING:
			ret = IO_ERR_CXED;
			break;
		default:
			fatal("Unknown net state", 0);
	}

	PT_UL(&(c->pt_state_mutex));

	return ret;
}

int
io_dx(struct connection *c)
{
	/* Force a socket thread into IO_ST_DXED state
	 *
	 * Valid only for states blocked on:
	 *   - IO_ST_RXNG: pthread_cond_timedwait()
	 *   - IO_ST_CXNG: connect()
	 *   - IO_ST_CXED: recv()
	 *   - IO_ST_PING: recv()
	 */

	enum io_err_t ret = IO_ERR_NONE;

	PT_LK(&(c->pt_state_mutex));

	switch (c->state) {
		case IO_ST_INIT:
		case IO_ST_DXED:
			ret = IO_ERR_DXED;
			break;
		case IO_ST_RXNG:
			io_lock_wake(&(c->lock));
			break;
		case IO_ST_CXNG:
		case IO_ST_CXED:
		case IO_ST_PING:
			do {
				/* Signal and yield the cpu until the target thread
				 * is flagged as having reached an EINTR handler */
				PT_CF(pthread_kill(c->pt_tid, SIGUSR1));
				if (sched_yield() < 0)
					fatal("sched_yield", errno);
			} while (0 /* TODO */);
			break;
		default:
			fatal("Unknown net state", 0);
	}

	PT_UL(&(c->pt_state_mutex));

	return ret;
}

static enum io_state_t
io_state_init(struct connection *c)
{
	io_lock_wake(&init_lock);
	io_lock_wait(&c->lock, NULL);
	return IO_ST_CXNG;
}

static enum io_state_t
io_state_dxed(struct connection *c)
{
	io_lock_wait(&c->lock, NULL);
	return IO_ST_CXNG;
}

static enum io_state_t
io_state_rxng(struct connection *c)
{
	struct timespec ts;

	if (c->rx_backoff == 0) {
		c->rx_backoff = IO_RECONNECT_BACKOFF_BASE;
	} else {
		c->rx_backoff = MIN(
			IO_RECONNECT_BACKOFF_FACTOR * c->rx_backoff,
			IO_RECONNECT_BACKOFF_MAX
		);
	}

	if (clock_gettime(CLOCK_REALTIME, &ts) < 0)
		fatal("clock_gettime", errno);
	ts.tv_sec += c->rx_backoff;

	PT_CB(io_cb(IO_CB_INFO, c->obj, "Attemping reconnect in: %us", c->rx_backoff));

	io_lock_wait(&c->lock, &ts);
	return IO_ST_CXNG;
}

static enum io_state_t
io_state_cxng(struct connection *c)
{
	int ret;

	char errbuf[1024];
	char ipbuf[INET6_ADDRSTRLEN];

	PT_CB(io_cb(IO_CB_INFO, c->obj, "Connecting to %s:%s ...", c->host, c->port));

	struct addrinfo *p, *res, hints = {
		.ai_family   = AF_UNSPEC,
		.ai_flags    = AI_PASSIVE,
		.ai_socktype = SOCK_STREAM
	};

	if ((ret = getaddrinfo(c->host, c->port, &hints, &res))) {
		PT_CB(io_cb(IO_CB_ERROR, c->obj, "Connection error: %s", gai_strerror(ret)));
		return IO_ST_RXNG;
	}

	for (p = res; p != NULL; p = p->ai_next) {

		if ((c->soc = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) < 0)
			continue;

		if (connect(c->soc, p->ai_addr, p->ai_addrlen) == 0)
			break;

		if ((ret = errno) == EINTR) {
			/* Connection was interrupted  by signal, canceled */
			;
		} else {
			/* Connection failed for other reasons */
			io_close(c);
		}
	}

	if (p == NULL) {
		PT_CB(io_cb(IO_CB_ERROR, c->obj, "Connection failure [%s]", io_strerror(ret, errbuf, sizeof(errbuf))));
		freeaddrinfo(res);
		return IO_ST_RXNG;
	}

	if ((ret = getnameinfo(p->ai_addr, p->ai_addrlen, ipbuf, INET6_ADDRSTRLEN, NULL, 0, NI_NUMERICHOST))) {
		io_cb(IO_CB_ERROR, c->obj, "Connected to %s [IP lookup failure: %s]", c->host, gai_strerror(ret));
	} else {
		io_cb(IO_CB_INFO, c->obj, "Connected to %s [%s]", c->host, ipbuf);
	}

	freeaddrinfo(res);

	return IO_ST_CXED;
}

static enum io_state_t
io_state_cxed(struct connection *c)
{
	PT_CB(io_cb(IO_CB_CXED, c->obj, "Connected to %s [TODO: ip]", c->host));

	struct timeval tv = {
		.tv_sec = 3
	};

	if (setsockopt(c->soc, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0)
		{ fatal("setsockopt", errno); }

	for (;;) {
		char recvbuf[IO_MESG_LEN];
		ssize_t ret;

		if ((ret = recv(c->soc, recvbuf, sizeof(recvbuf), 0)) <= 0) {

			if (ret == 0) {
				PT_CB(io_cb(IO_CB_DXED, c->obj, "Connection closed"));
				return IO_ST_CXNG;
			}

			if (errno == ECONNRESET) {
				PT_CB(io_cb(IO_CB_DXED, c->obj, "Connection forcibly reset by peer"));
				return IO_ST_CXNG;
			}

			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				return IO_ST_PING;
			}

			if (errno == EINTR) {
				return IO_ST_DXED;
			}

			fatal("recv", errno);
		} else {
			io_recv(c, recvbuf, (size_t) ret);
		}
	}
}

static enum io_state_t
io_state_ping(struct connection *c)
{
	struct timeval tv = {
		// FIXME: test this
		.tv_sec = 10
	};

	if (setsockopt(c->soc, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0)
		{ fatal("setsockopt", errno); }


	for (;;) {
		char recvbuf[IO_MESG_LEN];
		ssize_t ret;
		unsigned ping = 0;

		if ((ret = recv(c->soc, recvbuf, sizeof(recvbuf), 0)) <= 0) {

			if (ret == 0) {
				PT_CB(io_cb(IO_CB_DXED, c->obj, "Connection closed"));
				return IO_ST_CXNG;
			}

			if (errno == ECONNRESET) {
				PT_CB(io_cb(IO_CB_DXED, c->obj, "Connection forcibly reset by peer"));
				return IO_ST_CXNG;
			}

			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				if ((ping += 2) >= 10) {
					// TODO: close the socket, etc
					PT_CB(io_cb(IO_CB_DXED, c->obj, "Connection lost: ping timeout (%u)", IO_PING_MAX));
					io_close(c);
					return IO_ST_CXNG;
				} else {
					PT_CB(io_cb(IO_CB_PING_N, c->obj, ping));
					continue;
				}
			}

			if (errno == EINTR) {
				return IO_ST_DXED;
			}

			fatal("recv", errno);
		} else {
			io_recv(c, recvbuf, (size_t) ret);
		}

		PT_CB(io_cb(IO_CB_PING_0, c->obj));
		return IO_ST_CXED;
	}
}

static enum io_state_t
io_state_term(struct connection *c)
{
	io_lock_term(&(c->lock));

	free((void*)c->host);
	free((void*)c->port);
	free(c);

	pthread_exit(EXIT_SUCCESS);

	/* not reached */
	return IO_ST_INVALID;
}

static void*
io_thread(void *arg)
{
	struct connection *c = arg;

	for (;;) {

		enum io_state_t n_state;

		switch (c->state) {
			case IO_ST_INIT:
				n_state = io_state_init(c);
				break;
			case IO_ST_DXED:
				n_state = io_state_dxed(c);
				break;
			case IO_ST_CXNG:
				n_state = io_state_cxng(c);
				break;
			case IO_ST_RXNG:
				n_state = io_state_rxng(c);
				break;
			case IO_ST_CXED:
				n_state = io_state_cxed(c);
				break;
			case IO_ST_PING:
				n_state = io_state_ping(c);
				break;
			case IO_ST_TERM:
				io_state_term(c);
				break;
			default:
				fatal("Unknown net state", 0);
		}
		c->state = n_state;
	}

	return NULL;
}

static void
io_recv(struct connection *c, char *buf, size_t n)
{
	for (size_t i = 0; i < n; i++) {

		if (buf[i] == '\n' && c->read.i && c->read.buf[c->read.i - 1] == '\r') {
			c->read.buf[--c->read.i] = 0;

			if (c->read.i) {
				PT_CB(io_cb_read_soc(c->read.buf, c->read.i - 1, c->obj));
				c->read.i = 0;
			}
		} else {
			c->read.buf[c->read.i++] = buf[i];
		}
	}
}

void
io_free(struct connection *c)
{
	/* TODO  fatal if not disconnected */
	(void)c;
}

static void
sigaction_sigwinch(int sig)
{
	UNUSED(sig);
	flag_sigwinch = 1;
}

static void
io_init_sig(void)
{
	struct sigaction sa;

	sa.sa_handler = sigaction_sigwinch;
	sa.sa_flags = 0;
	sigemptyset(&sa.sa_mask);

	if (sigaction(SIGWINCH, &sa, NULL) < 0)
		fatal("sigaction - SIGWINCH", errno);

	// TODO add sigusr here to block, otherwise created threads are going
	// to wake on sigwinch
}

static void
io_init_tty(void)
{
	struct termios nterm;

	if (isatty(STDIN_FILENO) == 0)
		fatal("isatty", errno);

	if (tcgetattr(STDIN_FILENO, &term) < 0)
		fatal("tcgetattr", errno);

	nterm = term;
	nterm.c_lflag &= ~(ECHO | ICANON | ISIG);
	nterm.c_cc[VMIN]  = 1;
	nterm.c_cc[VTIME] = 0;

	if (tcsetattr(STDIN_FILENO, TCSANOW, &nterm) < 0)
		fatal("tcsetattr", errno);

	if (atexit(io_term_tty) < 0)
		fatal("atexit", 0);
}

static void
io_term_tty(void)
{
	if (tcsetattr(STDIN_FILENO, TCSADRAIN, &term) < 0)
		fatal("tcsetattr", errno);
}

void
io_loop(void (*io_loop_cb)(void))
{
	PT_CF(pthread_once(&init_once, io_init));

	io_init_sig();
	io_init_tty();

	for (;;) {
		char buf[512];
		ssize_t ret = read(STDIN_FILENO, buf, sizeof(buf));

		if (ret > 0)
			PT_CB(io_cb_read_inp(buf, ret));

		if (ret <= 0) {
			if (errno == EINTR) {
				if (flag_sigwinch) {
					flag_sigwinch = 0;
					PT_CB(io_cb(IO_CB_SIGNAL, NULL, IO_SIGWINCH));
				}
			} else {
				fatal("read", ret ? errno : 0);
			}
		}

		if (io_loop_cb)
			io_loop_cb();
	}
}

const char*
io_err(int err)
{
	const char *err_strs[] = {
		[IO_ERR_NONE]  = "success",
		[IO_ERR_TRUNC] = "data truncated",
		[IO_ERR_DXED]  = "socket not connected",
		[IO_ERR_CXNG]  = "socket connection in progress",
		[IO_ERR_CXED]  = "socket connected"
	};

	const char *err_str = NULL;

	if (err >= 0 && (unsigned) err < ELEMS(err_strs))
		err_str = err_strs[err];

	return err_str ? err_str : "unknown error";
}

unsigned
io_tty_cols(void)
{
	/* TODO */
	return 0;
}

unsigned
io_tty_rows(void)
{
	/* TODO */
	return 0;
}
