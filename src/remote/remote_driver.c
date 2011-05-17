/*
 * remote_internal.c: driver to provide access to libvirtd running
 *   on a remote machine
 *
 * Copyright (C) 2007-2011 Red Hat, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307  USA
 *
 * Author: Richard Jones <rjones@redhat.com>
 */

#include <config.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <assert.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <sys/wait.h>

/* Windows socket compatibility functions. */
#include <errno.h>
#include <sys/socket.h>

#ifndef HAVE_WINSOCK2_H /* Unix & Cygwin. */
# include <sys/un.h>
# include <net/if.h>
# include <netinet/in.h>
# include <netinet/tcp.h>
#endif

#ifdef HAVE_PWD_H
# include <pwd.h>
#endif

#ifdef HAVE_PATHS_H
# include <paths.h>
#endif

#include <rpc/types.h>
#include <rpc/xdr.h>
#include <gnutls/gnutls.h>
#include <gnutls/x509.h>
#include "gnutls_1_0_compat.h"
#if HAVE_SASL
# include <sasl/sasl.h>
#endif
#include <libxml/uri.h>

#include <netdb.h>

#include <poll.h>

#include "virterror_internal.h"
#include "logging.h"
#include "datatypes.h"
#include "domain_event.h"
#include "driver.h"
#include "buf.h"
#include "qparams.h"
#include "remote_driver.h"
#include "remote_protocol.h"
#include "qemu_protocol.h"
#include "memory.h"
#include "util.h"
#include "event.h"
#include "ignore-value.h"
#include "files.h"
#include "command.h"

#define VIR_FROM_THIS VIR_FROM_REMOTE

static int inside_daemon = 0;

struct remote_thread_call;


enum {
    REMOTE_MODE_WAIT_TX,
    REMOTE_MODE_WAIT_RX,
    REMOTE_MODE_COMPLETE,
    REMOTE_MODE_ERROR,
};

struct remote_thread_call {
    int mode;

    /* Buffer for outgoing data packet
     * 4 byte length, followed by RPC message header+body */
    char buffer[4 + REMOTE_MESSAGE_MAX];
    unsigned int bufferLength;
    unsigned int bufferOffset;

    unsigned int serial;
    unsigned int proc_nr;

    virCond cond;

    int want_reply;
    xdrproc_t ret_filter;
    char *ret;

    remote_error err;

    struct remote_thread_call *next;
};

struct private_stream_data {
    unsigned int has_error : 1;
    remote_error err;

    unsigned int serial;
    unsigned int proc_nr;

    virStreamEventCallback cb;
    void *cbOpaque;
    virFreeCallback cbFree;
    int cbEvents;
    int cbTimer;
    int cbDispatch;

    /* XXX this is potentially unbounded if the client
     * app has domain events registered, since packets
     * may be read off wire, while app isn't ready to
     * recv them. Figure out how to address this some
     * time....
     */
    char *incoming;
    unsigned int incomingOffset;
    unsigned int incomingLength;

    struct private_stream_data *next;
};

struct private_data {
    virMutex lock;

    int sock;                   /* Socket. */
    int errfd;                /* File handle connected to remote stderr */
    int watch;                  /* File handle watch */
    pid_t pid;                  /* PID of tunnel process */
    int uses_tls;               /* TLS enabled on socket? */
    int is_secure;              /* Secure if TLS or SASL or UNIX sockets */
    gnutls_session_t session;   /* GnuTLS session (if uses_tls != 0). */
    char *type;                 /* Cached return from remoteType. */
    int counter;                /* Generates serial numbers for RPC. */
    int localUses;              /* Ref count for private data */
    char *hostname;             /* Original hostname */
    FILE *debugLog;             /* Debug remote protocol */

#if HAVE_SASL
    sasl_conn_t *saslconn;      /* SASL context */

    const char *saslDecoded;
    unsigned int saslDecodedLength;
    unsigned int saslDecodedOffset;

    const char *saslEncoded;
    unsigned int saslEncodedLength;
    unsigned int saslEncodedOffset;

    char saslTemporary[8192]; /* temorary holds data to be decoded */
#endif

    /* Buffer for incoming data packets
     * 4 byte length, followed by RPC message header+body */
    char buffer[4 + REMOTE_MESSAGE_MAX];
    unsigned int bufferLength;
    unsigned int bufferOffset;

    virDomainEventStatePtr domainEventState;

    /* Self-pipe to wakeup threads waiting in poll() */
    int wakeupSendFD;
    int wakeupReadFD;

    /* List of threads currently waiting for dispatch */
    struct remote_thread_call *waitDispatch;

    struct private_stream_data *streams;
};

enum {
    REMOTE_CALL_IN_OPEN           = (1 << 0),
    REMOTE_CALL_QUIET_MISSING_RPC = (1 << 1),
    REMOTE_CALL_QEMU              = (1 << 2),
    REMOTE_CALL_NONBLOCK          = (1 << 3),
};


static void remoteDriverLock(struct private_data *driver)
{
    virMutexLock(&driver->lock);
}

static void remoteDriverUnlock(struct private_data *driver)
{
    virMutexUnlock(&driver->lock);
}

static int remoteIO(virConnectPtr conn,
                    struct private_data *priv,
                    int flags,
                    struct remote_thread_call *thiscall);
static int call (virConnectPtr conn, struct private_data *priv,
                 int flags, int proc_nr,
                 xdrproc_t args_filter, char *args,
                 xdrproc_t ret_filter, char *ret);
static int remoteAuthenticate (virConnectPtr conn, struct private_data *priv, int in_open,
                               virConnectAuthPtr auth, const char *authtype);
#if HAVE_SASL
static int remoteAuthSASL (virConnectPtr conn, struct private_data *priv, int in_open,
                           virConnectAuthPtr auth, const char *mech);
#endif
#if HAVE_POLKIT
static int remoteAuthPolkit (virConnectPtr conn, struct private_data *priv, int in_open,
                             virConnectAuthPtr auth);
#endif /* HAVE_POLKIT */

#define remoteError(code, ...)                                    \
    virReportErrorHelper(VIR_FROM_REMOTE, code, __FILE__,         \
                         __FUNCTION__, __LINE__, __VA_ARGS__)

static virDomainPtr get_nonnull_domain (virConnectPtr conn, remote_nonnull_domain domain);
static virNetworkPtr get_nonnull_network (virConnectPtr conn, remote_nonnull_network network);
static virNWFilterPtr get_nonnull_nwfilter (virConnectPtr conn, remote_nonnull_nwfilter nwfilter);
static virInterfacePtr get_nonnull_interface (virConnectPtr conn, remote_nonnull_interface iface);
static virStoragePoolPtr get_nonnull_storage_pool (virConnectPtr conn, remote_nonnull_storage_pool pool);
static virStorageVolPtr get_nonnull_storage_vol (virConnectPtr conn, remote_nonnull_storage_vol vol);
static virNodeDevicePtr get_nonnull_node_device (virConnectPtr conn, remote_nonnull_node_device dev);
static virSecretPtr get_nonnull_secret (virConnectPtr conn, remote_nonnull_secret secret);
static virDomainSnapshotPtr get_nonnull_domain_snapshot (virDomainPtr domain, remote_nonnull_domain_snapshot snapshot);
static void make_nonnull_domain (remote_nonnull_domain *dom_dst, virDomainPtr dom_src);
static void make_nonnull_network (remote_nonnull_network *net_dst, virNetworkPtr net_src);
static void make_nonnull_interface (remote_nonnull_interface *interface_dst, virInterfacePtr interface_src);
static void make_nonnull_storage_pool (remote_nonnull_storage_pool *pool_dst, virStoragePoolPtr vol_src);
static void make_nonnull_storage_vol (remote_nonnull_storage_vol *vol_dst, virStorageVolPtr vol_src);
static void make_nonnull_secret (remote_nonnull_secret *secret_dst, virSecretPtr secret_src);
static void make_nonnull_nwfilter (remote_nonnull_nwfilter *nwfilter_dst, virNWFilterPtr nwfilter_src);
static void make_nonnull_domain_snapshot (remote_nonnull_domain_snapshot *snapshot_dst, virDomainSnapshotPtr snapshot_src);
void remoteDomainEventFired(int watch, int fd, int event, void *data);
void remoteDomainEventQueueFlush(int timer, void *opaque);
void remoteDomainEventQueue(struct private_data *priv, virDomainEventPtr event);
/*----------------------------------------------------------------------*/

/* Helper functions for remoteOpen. */
static char *get_transport_from_scheme (char *scheme);

/* GnuTLS functions used by remoteOpen. */
static int initialize_gnutls(char *pkipath, int flags);
static gnutls_session_t negotiate_gnutls_on_connection (virConnectPtr conn, struct private_data *priv, int no_verify);

#ifdef WITH_LIBVIRTD
static int
remoteStartup(int privileged ATTRIBUTE_UNUSED)
{
    /* Mark that we're inside the daemon so we can avoid
     * re-entering ourselves
     */
    inside_daemon = 1;
    return 0;
}
#endif

#ifndef WIN32
/**
 * remoteFindServerPath:
 *
 * Tries to find the path to the libvirtd binary.
 *
 * Returns path on success or NULL in case of error.
 */
static const char *
remoteFindDaemonPath(void)
{
    static const char *serverPaths[] = {
        SBINDIR "/libvirtd",
        SBINDIR "/libvirtd_dbg",
        NULL
    };
    int i;
    const char *customDaemon = getenv("LIBVIRTD_PATH");

    if (customDaemon)
        return(customDaemon);

    for (i = 0; serverPaths[i]; i++) {
        if (virFileIsExecutable(serverPaths[i])) {
            return serverPaths[i];
        }
    }
    return NULL;
}

/**
 * qemuForkDaemon:
 *
 * Forks and try to launch the libvirtd daemon
 *
 * Returns 0 in case of success or -1 in case of detected error.
 */
static int
remoteForkDaemon(void)
{
    const char *daemonPath = remoteFindDaemonPath();
    virCommandPtr cmd = NULL;
    int ret;

    if (!daemonPath) {
        remoteError(VIR_ERR_INTERNAL_ERROR, "%s",
                    _("failed to find libvirtd binary"));
        return -1;
    }

    cmd = virCommandNewArgList(daemonPath, "--timeout", "30", NULL);
    virCommandClearCaps(cmd);
    virCommandDaemonize(cmd);

    ret = virCommandRun(cmd, NULL);
    virCommandFree(cmd);

    return ret;
}
#endif

enum virDrvOpenRemoteFlags {
    VIR_DRV_OPEN_REMOTE_RO = (1 << 0),
    VIR_DRV_OPEN_REMOTE_USER      = (1 << 1), /* Use the per-user socket path */
    VIR_DRV_OPEN_REMOTE_AUTOSTART = (1 << 2), /* Autostart a per-user daemon */
};


/*
 * URIs that this driver needs to handle:
 *
 * The easy answer:
 *   - Everything that no one else has yet claimed, but nothing if
 *     we're inside the libvirtd daemon
 *
 * The hard answer:
 *   - Plain paths (///var/lib/xen/xend-socket)  -> UNIX domain socket
 *   - xxx://servername/      -> TLS connection
 *   - xxx+tls://servername/  -> TLS connection
 *   - xxx+tls:///            -> TLS connection to localhost
 *   - xxx+tcp://servername/  -> TCP connection
 *   - xxx+tcp:///            -> TCP connection to localhost
 *   - xxx+unix:///           -> UNIX domain socket
 *   - xxx:///                -> UNIX domain socket
 */
static int
doRemoteOpen (virConnectPtr conn,
              struct private_data *priv,
              virConnectAuthPtr auth ATTRIBUTE_UNUSED,
              int flags)
{
    struct qparam_set *vars = NULL;
    int wakeupFD[2] = { -1, -1 };
    char *transport_str = NULL;
    enum {
        trans_tls,
        trans_unix,
        trans_ssh,
        trans_ext,
        trans_tcp,
    } transport;

    /* We handle *ALL*  URIs here. The caller has rejected any
     * URIs we don't care about */

    if (conn->uri) {
        if (!conn->uri->scheme) {
            /* This is the ///var/lib/xen/xend-socket local path style */
            transport = trans_unix;
        } else {
            transport_str = get_transport_from_scheme (conn->uri->scheme);

            if (!transport_str) {
                if (conn->uri->server)
                    transport = trans_tls;
                else
                    transport = trans_unix;
            } else {
                if (STRCASEEQ (transport_str, "tls"))
                    transport = trans_tls;
                else if (STRCASEEQ (transport_str, "unix"))
                    transport = trans_unix;
                else if (STRCASEEQ (transport_str, "ssh"))
                    transport = trans_ssh;
                else if (STRCASEEQ (transport_str, "ext"))
                    transport = trans_ext;
                else if (STRCASEEQ (transport_str, "tcp"))
                    transport = trans_tcp;
                else {
                    remoteError(VIR_ERR_INVALID_ARG, "%s",
                                _("remote_open: transport in URL not recognised "
                                  "(should be tls|unix|ssh|ext|tcp)"));
                    return VIR_DRV_OPEN_ERROR;
                }
            }
        }
    } else {
        /* No URI, then must be probing so use UNIX socket */
        transport = trans_unix;
    }

    /* Local variables which we will initialize. These can
     * get freed in the failed: path.
     */
    char *name = NULL, *command = NULL, *sockname = NULL, *netcat = NULL;
    char *port = NULL, *authtype = NULL, *username = NULL;
    int no_verify = 0, no_tty = 0;
    char *pkipath = NULL;
    virCommandPtr cmd = NULL;

    /* Return code from this function, and the private data. */
    int retcode = VIR_DRV_OPEN_ERROR;

    /* Remote server defaults to "localhost" if not specified. */
    if (conn->uri && conn->uri->port != 0) {
        if (virAsprintf(&port, "%d", conn->uri->port) == -1) goto out_of_memory;
    } else if (transport == trans_tls) {
        port = strdup (LIBVIRTD_TLS_PORT);
        if (!port) goto out_of_memory;
    } else if (transport == trans_tcp) {
        port = strdup (LIBVIRTD_TCP_PORT);
        if (!port) goto out_of_memory;
    } else
        port = NULL; /* Port not used for unix, ext., default for ssh */


    priv->hostname = strdup (conn->uri && conn->uri->server ?
                             conn->uri->server : "localhost");
    if (!priv->hostname)
        goto out_of_memory;
    if (conn->uri && conn->uri->user) {
        username = strdup (conn->uri->user);
        if (!username)
            goto out_of_memory;
    }

    /* Get the variables from the query string.
     * Then we need to reconstruct the query string (because
     * feasibly it might contain variables needed by the real driver,
     * although that won't be the case for now).
     */
    struct qparam *var;
    int i;
    char *query;

    if (conn->uri) {
#ifdef HAVE_XMLURI_QUERY_RAW
        query = conn->uri->query_raw;
#else
        query = conn->uri->query;
#endif
        vars = qparam_query_parse (query);
        if (vars == NULL) goto failed;

        for (i = 0; i < vars->n; i++) {
            var = &vars->p[i];
            if (STRCASEEQ (var->name, "name")) {
                VIR_FREE(name);
                name = strdup (var->value);
                if (!name) goto out_of_memory;
                var->ignore = 1;
            } else if (STRCASEEQ (var->name, "command")) {
                VIR_FREE(command);
                command = strdup (var->value);
                if (!command) goto out_of_memory;
                var->ignore = 1;
            } else if (STRCASEEQ (var->name, "socket")) {
                VIR_FREE(sockname);
                sockname = strdup (var->value);
                if (!sockname) goto out_of_memory;
                var->ignore = 1;
            } else if (STRCASEEQ (var->name, "auth")) {
                VIR_FREE(authtype);
                authtype = strdup (var->value);
                if (!authtype) goto out_of_memory;
                var->ignore = 1;
            } else if (STRCASEEQ (var->name, "netcat")) {
                VIR_FREE(netcat);
                netcat = strdup (var->value);
                if (!netcat) goto out_of_memory;
                var->ignore = 1;
            } else if (STRCASEEQ (var->name, "no_verify")) {
                no_verify = atoi (var->value);
                var->ignore = 1;
            } else if (STRCASEEQ (var->name, "no_tty")) {
                no_tty = atoi (var->value);
                var->ignore = 1;
            } else if (STRCASEEQ (var->name, "debug")) {
                if (var->value &&
                    STRCASEEQ (var->value, "stdout"))
                    priv->debugLog = stdout;
                else
                    priv->debugLog = stderr;
            } else if (STRCASEEQ(var->name, "pkipath")) {
                VIR_FREE(pkipath);
                pkipath = strdup(var->value);
                if (!pkipath) goto out_of_memory;
                var->ignore = 1;
            } else {
                VIR_DEBUG("passing through variable '%s' ('%s') to remote end",
                      var->name, var->value);
            }
        }

        /* Construct the original name. */
        if (!name) {
            if (conn->uri->scheme &&
                (STREQ(conn->uri->scheme, "remote") ||
                 STRPREFIX(conn->uri->scheme, "remote+"))) {
                /* Allow remote serve to probe */
                name = strdup("");
            } else {
                xmlURI tmpuri = {
                    .scheme = conn->uri->scheme,
#ifdef HAVE_XMLURI_QUERY_RAW
                    .query_raw = qparam_get_query (vars),
#else
                    .query = qparam_get_query (vars),
#endif
                    .path = conn->uri->path,
                    .fragment = conn->uri->fragment,
                };

                /* Evil, blank out transport scheme temporarily */
                if (transport_str) {
                    assert (transport_str[-1] == '+');
                    transport_str[-1] = '\0';
                }

                name = (char *) xmlSaveUri (&tmpuri);

#ifdef HAVE_XMLURI_QUERY_RAW
                VIR_FREE(tmpuri.query_raw);
#else
                VIR_FREE(tmpuri.query);
#endif

                /* Restore transport scheme */
                if (transport_str)
                    transport_str[-1] = '+';
            }
        }

        free_qparam_set (vars);
        vars = NULL;
    } else {
        /* Probe URI server side */
        name = strdup("");
    }

    if (!name) {
        virReportOOMError();
        goto failed;
    }

    VIR_DEBUG("proceeding with name = %s", name);

    /* For ext transport, command is required. */
    if (transport == trans_ext && !command) {
        remoteError(VIR_ERR_INVALID_ARG, "%s",
                    _("remote_open: for 'ext' transport, command is required"));
        goto failed;
    }

    /* Connect to the remote service. */
    switch (transport) {
    case trans_tls:
        if (initialize_gnutls(pkipath, flags) == -1) goto failed;
        priv->uses_tls = 1;
        priv->is_secure = 1;

        /*FALLTHROUGH*/
    case trans_tcp: {
        /* http://people.redhat.com/drepper/userapi-ipv6.html */
        struct addrinfo *res, *r;
        struct addrinfo hints;
        int saved_errno = EINVAL;
        memset (&hints, 0, sizeof hints);
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_flags = AI_ADDRCONFIG;
        int e = getaddrinfo (priv->hostname, port, &hints, &res);
        if (e != 0) {
            remoteError(VIR_ERR_SYSTEM_ERROR,
                        _("unable to resolve hostname '%s': %s"),
                        priv->hostname, gai_strerror (e));
            goto failed;
        }

        /* Try to connect to each returned address in turn. */
        /* XXX This loop contains a subtle problem.  In the case
         * where a host is accessible over IPv4 and IPv6, it will
         * try the IPv4 and IPv6 addresses in turn.  However it
         * should be able to present different client certificates
         * (because the commonName field in a client cert contains
         * the client IP address, which is different for IPv4 and
         * IPv6).  At the moment we only have a single client
         * certificate, and no way to specify what address family
         * that certificate belongs to.
         */
        for (r = res; r; r = r->ai_next) {
            int no_slow_start = 1;

            priv->sock = socket (r->ai_family, SOCK_STREAM, 0);
            if (priv->sock == -1) {
                saved_errno = errno;
                continue;
            }

            /* Disable Nagle - Dan Berrange. */
            setsockopt (priv->sock,
                        IPPROTO_TCP, TCP_NODELAY, (void *)&no_slow_start,
                        sizeof no_slow_start);

            if (connect (priv->sock, r->ai_addr, r->ai_addrlen) == -1) {
                saved_errno = errno;
                VIR_FORCE_CLOSE(priv->sock);
                continue;
            }

            if (priv->uses_tls) {
                priv->session =
                    negotiate_gnutls_on_connection
                      (conn, priv, no_verify);
                if (!priv->session) {
                    VIR_FORCE_CLOSE(priv->sock);
                    goto failed;
                }
            }
            goto tcp_connected;
        }

        freeaddrinfo (res);
        virReportSystemError(saved_errno,
                             _("unable to connect to libvirtd at '%s'"),
                             priv->hostname);
        goto failed;

       tcp_connected:
        freeaddrinfo (res);

        /* NB. All versioning is done by the RPC headers, so we don't
         * need to worry (at this point anyway) about versioning. */
        break;
    }

#ifndef WIN32
    case trans_unix: {
        if (!sockname) {
            if (flags & VIR_DRV_OPEN_REMOTE_USER) {
                char *userdir = virGetUserDirectory(getuid());

                if (!userdir)
                    goto failed;

                if (virAsprintf(&sockname, "@%s" LIBVIRTD_USER_UNIX_SOCKET, userdir) < 0) {
                    VIR_FREE(userdir);
                    goto out_of_memory;
                }
                VIR_FREE(userdir);
            } else {
                if (flags & VIR_DRV_OPEN_REMOTE_RO)
                    sockname = strdup (LIBVIRTD_PRIV_UNIX_SOCKET_RO);
                else
                    sockname = strdup (LIBVIRTD_PRIV_UNIX_SOCKET);
                if (sockname == NULL)
                    goto out_of_memory;
            }
        }

# ifndef UNIX_PATH_MAX
#  define UNIX_PATH_MAX(addr) (sizeof (addr).sun_path)
# endif
        struct sockaddr_un addr;
        int trials = 0;

        memset (&addr, 0, sizeof addr);
        addr.sun_family = AF_UNIX;
        if (virStrcpyStatic(addr.sun_path, sockname) == NULL) {
            remoteError(VIR_ERR_INTERNAL_ERROR,
                        _("Socket %s too big for destination"), sockname);
            goto failed;
        }
        if (addr.sun_path[0] == '@')
            addr.sun_path[0] = '\0';

      autostart_retry:
        priv->is_secure = 1;
        priv->sock = socket (AF_UNIX, SOCK_STREAM, 0);
        if (priv->sock == -1) {
            virReportSystemError(errno, "%s",
                                 _("unable to create socket"));
            goto failed;
        }
        if (connect (priv->sock, (struct sockaddr *) &addr, sizeof addr) == -1) {
            /* We might have to autostart the daemon in some cases....
             * It takes a short while for the daemon to startup, hence we
             * have a number of retries, with a small sleep. This will
             * sometimes cause multiple daemons to be started - this is
             * ok because the duplicates will fail to bind to the socket
             * and immediately exit, leaving just one daemon.
             */
            if (errno == ECONNREFUSED &&
                flags & VIR_DRV_OPEN_REMOTE_AUTOSTART &&
                trials < 20) {
                VIR_FORCE_CLOSE(priv->sock);
                if (trials > 0 ||
                    remoteForkDaemon() == 0) {
                    trials++;
                    usleep(1000 * 100 * trials);
                    goto autostart_retry;
                }
            }
            virReportSystemError(errno,
              _("unable to connect to '%s', libvirtd may need to be started"),
              sockname);
            goto failed;
        }

        break;
    }

    case trans_ssh: {
        cmd = virCommandNew(command ? command : "ssh");

        /* Generate the final command argv[] array.
         *   ssh [-p $port] [-l $username] $hostname $netcat -U $sockname */

        if (port) {
            virCommandAddArgList(cmd, "-p", port, NULL);
        }
        if (username) {
            virCommandAddArgList(cmd, "-l", username, NULL);
        }
        if (no_tty) {
            virCommandAddArgList(cmd, "-T", "-o", "BatchMode=yes", "-e",
                                 "none", NULL);
        }
        virCommandAddArgList(cmd, priv->hostname, netcat ? netcat : "nc",
                             "-U", (sockname ? sockname :
                                    (flags & VIR_CONNECT_RO
                                     ? LIBVIRTD_PRIV_UNIX_SOCKET_RO
                                     : LIBVIRTD_PRIV_UNIX_SOCKET)), NULL);

        priv->is_secure = 1;
    }

        /*FALLTHROUGH*/
    case trans_ext: {
        pid_t pid;
        int sv[2];
        int errfd[2];

        /* Fork off the external process.  Use socketpair to create a private
         * (unnamed) Unix domain socket to the child process so we don't have
         * to faff around with two file descriptors (a la 'pipe(2)').
         */
        if (socketpair (PF_UNIX, SOCK_STREAM, 0, sv) == -1) {
            virReportSystemError(errno, "%s",
                                 _("unable to create socket pair"));
            goto failed;
        }

        if (pipe(errfd) == -1) {
            virReportSystemError(errno, "%s",
                                 _("unable to create socket pair"));
            goto failed;
        }

        virCommandSetInputFD(cmd, sv[1]);
        virCommandSetOutputFD(cmd, &(sv[1]));
        virCommandSetErrorFD(cmd, &(errfd[1]));
        virCommandClearCaps(cmd);
        if (virCommandRunAsync(cmd, &pid) < 0)
            goto failed;

        /* Parent continues here. */
        VIR_FORCE_CLOSE(sv[1]);
        VIR_FORCE_CLOSE(errfd[1]);
        priv->sock = sv[0];
        priv->errfd = errfd[0];
        priv->pid = pid;

        /* Do not set 'is_secure' flag since we can't guarentee
         * an external program is secure, and this flag must be
         * pessimistic */
    }
#else /* WIN32 */

    case trans_unix:
    case trans_ssh:
    case trans_ext:
        remoteError(VIR_ERR_INVALID_ARG, "%s",
                    _("transport methods unix, ssh and ext are not supported "
                      "under Windows"));
        goto failed;

#endif /* WIN32 */

    } /* switch (transport) */

    if (virSetNonBlock(priv->sock) < 0) {
        virReportSystemError(errno, "%s",
                             _("unable to make socket non-blocking"));
        goto failed;
    }

    if ((priv->errfd != -1) && virSetNonBlock(priv->errfd) < 0) {
        virReportSystemError(errno, "%s",
                             _("unable to make socket non-blocking"));
        goto failed;
    }

    if (pipe(wakeupFD) < 0) {
        virReportSystemError(errno, "%s",
                             _("unable to make pipe"));
        goto failed;
    }
    priv->wakeupReadFD = wakeupFD[0];
    priv->wakeupSendFD = wakeupFD[1];

    /* Try and authenticate with server */
    if (remoteAuthenticate(conn, priv, 1, auth, authtype) == -1)
        goto failed;

    /* Finally we can call the remote side's open function. */
    {
        remote_open_args args = { &name, flags };

        if (call (conn, priv, REMOTE_CALL_IN_OPEN, REMOTE_PROC_OPEN,
                  (xdrproc_t) xdr_remote_open_args, (char *) &args,
                  (xdrproc_t) xdr_void, (char *) NULL) == -1)
            goto failed;
    }

    /* Now try and find out what URI the daemon used */
    if (conn->uri == NULL) {
        remote_get_uri_ret uriret;
        int urierr;

        memset (&uriret, 0, sizeof uriret);
        urierr = call (conn, priv,
                       REMOTE_CALL_IN_OPEN | REMOTE_CALL_QUIET_MISSING_RPC,
                       REMOTE_PROC_GET_URI,
                       (xdrproc_t) xdr_void, (char *) NULL,
                       (xdrproc_t) xdr_remote_get_uri_ret, (char *) &uriret);
        if (urierr == -2) {
            /* Should not really happen, since we only probe local libvirtd's,
               & the library should always match the daemon. Only case is post
               RPM upgrade where an old daemon instance is still running with
               new client. Too bad. It is not worth the hassle to fix this */
            remoteError(VIR_ERR_INTERNAL_ERROR, "%s",
                        _("unable to auto-detect URI"));
            goto failed;
        }
        if (urierr == -1) {
            goto failed;
        }

        VIR_DEBUG("Auto-probed URI is %s", uriret.uri);
        conn->uri = xmlParseURI(uriret.uri);
        VIR_FREE(uriret.uri);
        if (!conn->uri) {
            virReportOOMError();
            goto failed;
        }
    }

    /* Set up a callback to listen on the socket data */
    if ((priv->watch = virEventAddHandle(priv->sock,
                                         VIR_EVENT_HANDLE_READABLE,
                                         remoteDomainEventFired,
                                         conn, NULL)) < 0) {
        VIR_DEBUG("virEventAddHandle failed: No addHandleImpl defined."
               " continuing without events.");
        priv->watch = -1;
    }

    priv->domainEventState = virDomainEventStateNew(remoteDomainEventQueueFlush,
                                                    conn,
                                                    NULL,
                                                    false);
    if (!priv->domainEventState) {
        goto failed;
    }
    if (priv->domainEventState->timer < 0 && priv->watch != -1) {
        virEventRemoveHandle(priv->watch);
        priv->watch = -1;
    }

    /* Successful. */
    retcode = VIR_DRV_OPEN_SUCCESS;

 cleanup:
    /* Free up the URL and strings. */
    VIR_FREE(name);
    VIR_FREE(command);
    VIR_FREE(sockname);
    VIR_FREE(authtype);
    VIR_FREE(netcat);
    VIR_FREE(username);
    VIR_FREE(port);
    virCommandFree(cmd);
    VIR_FREE(pkipath);

    return retcode;

 out_of_memory:
    virReportOOMError();
    if (vars)
        free_qparam_set (vars);

 failed:
    /* Close the socket if we failed. */
    VIR_FORCE_CLOSE(priv->errfd);

    if (priv->sock >= 0) {
        if (priv->uses_tls && priv->session) {
            gnutls_bye (priv->session, GNUTLS_SHUT_RDWR);
            gnutls_deinit (priv->session);
        }
        VIR_FORCE_CLOSE(priv->sock);
#ifndef WIN32
        if (priv->pid > 0) {
            pid_t reap;
            do {
retry:
                reap = waitpid(priv->pid, NULL, 0);
                if (reap == -1 && errno == EINTR)
                    goto retry;
            } while (reap != -1 && reap != priv->pid);
        }
#endif
    }

    VIR_FORCE_CLOSE(wakeupFD[0]);
    VIR_FORCE_CLOSE(wakeupFD[1]);

    VIR_FREE(priv->hostname);
    goto cleanup;
}

static struct private_data *
remoteAllocPrivateData(void)
{
    struct private_data *priv;
    if (VIR_ALLOC(priv) < 0) {
        virReportOOMError();
        return NULL;
    }

    if (virMutexInit(&priv->lock) < 0) {
        remoteError(VIR_ERR_INTERNAL_ERROR, "%s",
                    _("cannot initialize mutex"));
        VIR_FREE(priv);
        return NULL;
    }
    remoteDriverLock(priv);
    priv->localUses = 1;
    priv->watch = -1;
    priv->sock = -1;
    priv->errfd = -1;

    return priv;
}

static int
remoteOpenSecondaryDriver(virConnectPtr conn,
                          virConnectAuthPtr auth,
                          int flags,
                          struct private_data **priv)
{
    int ret;
    int rflags = 0;

    if (!((*priv) = remoteAllocPrivateData()))
        return VIR_DRV_OPEN_ERROR;

    if (flags & VIR_CONNECT_RO)
        rflags |= VIR_DRV_OPEN_REMOTE_RO;

    ret = doRemoteOpen(conn, *priv, auth, rflags);
    if (ret != VIR_DRV_OPEN_SUCCESS) {
        remoteDriverUnlock(*priv);
        VIR_FREE(*priv);
    } else {
        (*priv)->localUses = 1;
        remoteDriverUnlock(*priv);
    }

    return ret;
}

static virDrvOpenStatus
remoteOpen (virConnectPtr conn,
            virConnectAuthPtr auth,
            int flags)
{
    struct private_data *priv;
    int ret, rflags = 0;
    const char *autostart = getenv("LIBVIRT_AUTOSTART");

    if (inside_daemon && (!conn->uri || (conn->uri && !conn->uri->server)))
        return VIR_DRV_OPEN_DECLINED;

    if (!(priv = remoteAllocPrivateData()))
        return VIR_DRV_OPEN_ERROR;

    if (flags & VIR_CONNECT_RO)
        rflags |= VIR_DRV_OPEN_REMOTE_RO;

    /*
     * If no servername is given, and no +XXX
     * transport is listed, or transport is unix,
     * and path is /session, and uid is unprivileged
     * then auto-spawn a daemon.
     */
    if (conn->uri &&
        !conn->uri->server &&
        conn->uri->path &&
        conn->uri->scheme &&
        ((strchr(conn->uri->scheme, '+') == 0)||
         (strstr(conn->uri->scheme, "+unix") != NULL)) &&
        (STREQ(conn->uri->path, "/session") ||
         STRPREFIX(conn->uri->scheme, "test+")) &&
        getuid() > 0) {
        VIR_DEBUG("Auto-spawn user daemon instance");
        rflags |= VIR_DRV_OPEN_REMOTE_USER;
        if (!autostart ||
            STRNEQ(autostart, "0"))
            rflags |= VIR_DRV_OPEN_REMOTE_AUTOSTART;
    }

    /*
     * If URI is NULL, then do a UNIX connection possibly auto-spawning
     * unprivileged server and probe remote server for URI. On Solaris,
     * this isn't supported, but we may be privileged enough to connect
     * to the UNIX socket anyway.
     */
    if (!conn->uri) {
        VIR_DEBUG("Auto-probe remote URI");
#ifndef __sun
        if (getuid() > 0) {
            VIR_DEBUG("Auto-spawn user daemon instance");
            rflags |= VIR_DRV_OPEN_REMOTE_USER;
            if (!autostart ||
                STRNEQ(autostart, "0"))
                rflags |= VIR_DRV_OPEN_REMOTE_AUTOSTART;
        }
#endif
    }

    ret = doRemoteOpen(conn, priv, auth, rflags);
    if (ret != VIR_DRV_OPEN_SUCCESS) {
        conn->privateData = NULL;
        remoteDriverUnlock(priv);
        VIR_FREE(priv);
    } else {
        conn->privateData = priv;
        remoteDriverUnlock(priv);
    }
    return ret;
}


/* In a string "driver+transport" return a pointer to "transport". */
static char *
get_transport_from_scheme (char *scheme)
{
    char *p = strchr (scheme, '+');
    return p ? p+1 : 0;
}

/* GnuTLS functions used by remoteOpen. */
static gnutls_certificate_credentials_t x509_cred;


static int
check_cert_file(const char *type, const char *file)
{
    if (access(file, R_OK)) {
        virReportSystemError(errno,
                             _("Cannot access %s '%s'"),
                             type, file);
        return -1;
    }
    return 0;
}


static void remote_debug_gnutls_log(int level, const char* str) {
    VIR_DEBUG("%d %s", level, str);
}

static int
initialize_gnutls(char *pkipath, int flags)
{
    static int initialized = 0;
    int err;
    char *gnutlsdebug;
    char *libvirt_cacert = NULL;
    char *libvirt_clientkey = NULL;
    char *libvirt_clientcert = NULL;
    int ret = -1;
    char *userdir = NULL;
    char *user_pki_path = NULL;

    if (initialized) return 0;

    gnutls_global_init ();

    if ((gnutlsdebug = getenv("LIBVIRT_GNUTLS_DEBUG")) != NULL) {
        int val;
        if (virStrToLong_i(gnutlsdebug, NULL, 10, &val) < 0)
            val = 10;
        gnutls_global_set_log_level(val);
        gnutls_global_set_log_function(remote_debug_gnutls_log);
    }

    /* X509 stuff */
    err = gnutls_certificate_allocate_credentials (&x509_cred);
    if (err) {
        remoteError(VIR_ERR_GNUTLS_ERROR,
                    _("unable to allocate TLS credentials: %s"),
                    gnutls_strerror (err));
        return -1;
    }

    if (pkipath) {
        if ((virAsprintf(&libvirt_cacert, "%s/%s", pkipath,
                        "cacert.pem")) < 0)
            goto out_of_memory;

        if ((virAsprintf(&libvirt_clientkey, "%s/%s", pkipath,
                        "clientkey.pem")) < 0)
            goto out_of_memory;

        if ((virAsprintf(&libvirt_clientcert, "%s/%s", pkipath,
                        "clientcert.pem")) < 0)
             goto out_of_memory;
    } else if (flags & VIR_DRV_OPEN_REMOTE_USER || getuid() > 0) {
        userdir = virGetUserDirectory(getuid());

        if (!userdir)
            goto out_of_memory;

        if (virAsprintf(&user_pki_path, "%s/.pki/libvirt", userdir) < 0)
            goto out_of_memory;

        if ((virAsprintf(&libvirt_cacert, "%s/%s", user_pki_path,
                        "cacert.pem")) < 0)
            goto out_of_memory;

        if ((virAsprintf(&libvirt_clientkey, "%s/%s", user_pki_path,
                        "clientkey.pem")) < 0)
            goto out_of_memory;

        if ((virAsprintf(&libvirt_clientcert, "%s/%s", user_pki_path,
                        "clientcert.pem")) < 0)
            goto out_of_memory;

        /* Use the default location of the CA certificate if it
         * cannot be found in $HOME/.pki/libvirt
         */
        if (!virFileExists(libvirt_cacert)) {
            VIR_FREE(libvirt_cacert);

            libvirt_cacert = strdup(LIBVIRT_CACERT);
            if (!libvirt_cacert) goto out_of_memory;
        }

        /* Use default location as long as one of
         * client key, and client certificate cannot be found in
         * $HOME/.pki/libvirt, we don't want to make user confused
         * with one file is here, the other is there.
         */
        if (!virFileExists(libvirt_clientkey) ||
            !virFileExists(libvirt_clientcert)) {
            VIR_FREE(libvirt_clientkey);
            VIR_FREE(libvirt_clientcert);

            libvirt_clientkey = strdup(LIBVIRT_CLIENTKEY);
            if (!libvirt_clientkey) goto out_of_memory;

            libvirt_clientcert = strdup(LIBVIRT_CLIENTCERT);
            if (!libvirt_clientcert) goto out_of_memory;
        }
    } else {
        libvirt_cacert = strdup(LIBVIRT_CACERT);
        if (!libvirt_cacert) goto out_of_memory;

        libvirt_clientkey = strdup(LIBVIRT_CLIENTKEY);
        if (!libvirt_clientkey) goto out_of_memory;

        libvirt_clientcert = strdup(LIBVIRT_CLIENTCERT);
        if (!libvirt_clientcert) goto out_of_memory;
    }

    if (check_cert_file("CA certificate", libvirt_cacert) < 0)
        goto error;
    if (check_cert_file("client key", libvirt_clientkey) < 0)
        goto error;
    if (check_cert_file("client certificate", libvirt_clientcert) < 0)
        goto error;

    /* Set the trusted CA cert. */
    VIR_DEBUG("loading CA file %s", libvirt_cacert);
    err =
        gnutls_certificate_set_x509_trust_file (x509_cred, libvirt_cacert,
                                                GNUTLS_X509_FMT_PEM);
    if (err < 0) {
        remoteError(VIR_ERR_GNUTLS_ERROR,
                    _("unable to load CA certificate '%s': %s"),
                    libvirt_cacert, gnutls_strerror (err));
        goto error;
    }

    /* Set the client certificate and private key. */
    VIR_DEBUG("loading client cert and key from files %s and %s",
          libvirt_clientcert, libvirt_clientkey);
    err =
        gnutls_certificate_set_x509_key_file (x509_cred,
                                              libvirt_clientcert,
                                              libvirt_clientkey,
                                              GNUTLS_X509_FMT_PEM);
    if (err < 0) {
        remoteError(VIR_ERR_GNUTLS_ERROR,
                    _("unable to load private key '%s' and/or "
                    "certificate '%s': %s"), libvirt_clientkey,
                    libvirt_clientcert, gnutls_strerror (err));
        goto error;
    }

    initialized = 1;
    ret = 0;

cleanup:
    VIR_FREE(libvirt_cacert);
    VIR_FREE(libvirt_clientkey);
    VIR_FREE(libvirt_clientcert);
    VIR_FREE(userdir);
    VIR_FREE(user_pki_path);
    return ret;

error:
    ret = -1;
    goto cleanup;

out_of_memory:
    ret = -1;
    virReportOOMError();
    goto cleanup;
}

static int verify_certificate (virConnectPtr conn, struct private_data *priv, gnutls_session_t session);

#if HAVE_WINSOCK2_H
static ssize_t
custom_gnutls_push(void *s, const void *buf, size_t len)
{
    return send((size_t)s, buf, len, 0);
}

static ssize_t
custom_gnutls_pull(void *s, void *buf, size_t len)
{
    return recv((size_t)s, buf, len, 0);
}
#endif

static gnutls_session_t
negotiate_gnutls_on_connection (virConnectPtr conn,
                                struct private_data *priv,
                                int no_verify)
{
    const int cert_type_priority[3] = {
        GNUTLS_CRT_X509,
        GNUTLS_CRT_OPENPGP,
        0
    };
    bool success = false;
    int err;
    gnutls_session_t session;

    /* Initialize TLS session
     */
    err = gnutls_init (&session, GNUTLS_CLIENT);
    if (err) {
        remoteError(VIR_ERR_GNUTLS_ERROR,
                    _("unable to initialize TLS client: %s"),
                    gnutls_strerror (err));
        return NULL;
    }

    /* Use default priorities */
    err = gnutls_set_default_priority (session);
    if (err) {
        remoteError(VIR_ERR_GNUTLS_ERROR,
                    _("unable to set TLS algorithm priority: %s"),
                    gnutls_strerror (err));
        goto cleanup;
    }
    err =
        gnutls_certificate_type_set_priority (session,
                                              cert_type_priority);
    if (err) {
        remoteError(VIR_ERR_GNUTLS_ERROR,
                    _("unable to set certificate priority: %s"),
                    gnutls_strerror (err));
        goto cleanup;
    }

    /* put the x509 credentials to the current session
     */
    err = gnutls_credentials_set (session, GNUTLS_CRD_CERTIFICATE, x509_cred);
    if (err) {
        remoteError(VIR_ERR_GNUTLS_ERROR,
                    _("unable to set session credentials: %s"),
                    gnutls_strerror (err));
        goto cleanup;
    }

    gnutls_transport_set_ptr (session,
                              (gnutls_transport_ptr_t) (long) priv->sock);

#if HAVE_WINSOCK2_H
    /* Make sure GnuTLS uses gnulib's replacment functions for send() and
     * recv() on Windows */
    gnutls_transport_set_push_function(session, custom_gnutls_push);
    gnutls_transport_set_pull_function(session, custom_gnutls_pull);
#endif

    /* Perform the TLS handshake. */
 again:
    err = gnutls_handshake (session);
    if (err < 0) {
        if (err == GNUTLS_E_AGAIN || err == GNUTLS_E_INTERRUPTED)
            goto again;
        remoteError(VIR_ERR_GNUTLS_ERROR,
                    _("unable to complete TLS handshake: %s"),
                    gnutls_strerror (err));
        goto cleanup;
    }

    /* Verify certificate. */
    if (verify_certificate (conn, priv, session) == -1) {
        VIR_DEBUG("failed to verify peer's certificate");
        if (!no_verify)
            goto cleanup;
    }

    /* At this point, the server is verifying _our_ certificate, IP address,
     * etc.  If we make the grade, it will send us a '\1' byte.
     */
    char buf[1];
    int len;
 again_2:
    len = gnutls_record_recv (session, buf, 1);
    if (len < 0 && len != GNUTLS_E_UNEXPECTED_PACKET_LENGTH) {
        if (len == GNUTLS_E_AGAIN || len == GNUTLS_E_INTERRUPTED)
            goto again_2;
        remoteError(VIR_ERR_GNUTLS_ERROR,
                    _("unable to complete TLS initialization: %s"),
                    gnutls_strerror (len));
        goto cleanup;
    }
    if (len != 1 || buf[0] != '\1') {
        remoteError(VIR_ERR_RPC, "%s",
                    _("server verification (of our certificate or IP "
                      "address) failed"));
        goto cleanup;
    }

#if 0
    /* Print session info. */
    print_info (session);
#endif

    success = true;

cleanup:
    if (!success) {
        gnutls_deinit(session);
        session = NULL;
    }

    return session;
}

static int
verify_certificate (virConnectPtr conn ATTRIBUTE_UNUSED,
                    struct private_data *priv,
                    gnutls_session_t session)
{
    int ret;
    unsigned int status;
    const gnutls_datum_t *certs;
    unsigned int nCerts, i;
    time_t now;

    if ((ret = gnutls_certificate_verify_peers2 (session, &status)) < 0) {
        remoteError(VIR_ERR_GNUTLS_ERROR,
                    _("unable to verify server certificate: %s"),
                    gnutls_strerror (ret));
        return -1;
    }

    if ((now = time(NULL)) == ((time_t)-1)) {
        virReportSystemError(errno, "%s",
                             _("cannot get current time"));
        return -1;
    }

    if (status != 0) {
        const char *reason = _("Invalid certificate");

        if (status & GNUTLS_CERT_INVALID)
            reason = _("The certificate is not trusted.");

        if (status & GNUTLS_CERT_SIGNER_NOT_FOUND)
            reason = _("The certificate hasn't got a known issuer.");

        if (status & GNUTLS_CERT_REVOKED)
            reason = _("The certificate has been revoked.");

#ifndef GNUTLS_1_0_COMPAT
        if (status & GNUTLS_CERT_INSECURE_ALGORITHM)
            reason = _("The certificate uses an insecure algorithm");
#endif

        remoteError(VIR_ERR_RPC,
                    _("server certificate failed validation: %s"),
                    reason);
        return -1;
    }

    if (gnutls_certificate_type_get(session) != GNUTLS_CRT_X509) {
        remoteError(VIR_ERR_RPC,  "%s",_("Certificate type is not X.509"));
        return -1;
    }

    if (!(certs = gnutls_certificate_get_peers(session, &nCerts))) {
        remoteError(VIR_ERR_RPC,  "%s",_("gnutls_certificate_get_peers failed"));
        return -1;
    }

    for (i = 0 ; i < nCerts ; i++) {
        gnutls_x509_crt_t cert;

        ret = gnutls_x509_crt_init (&cert);
        if (ret < 0) {
            remoteError(VIR_ERR_GNUTLS_ERROR,
                        _("unable to initialize certificate: %s"),
                        gnutls_strerror (ret));
            return -1;
        }

        ret = gnutls_x509_crt_import (cert, &certs[i], GNUTLS_X509_FMT_DER);
        if (ret < 0) {
            remoteError(VIR_ERR_GNUTLS_ERROR,
                        _("unable to import certificate: %s"),
                        gnutls_strerror (ret));
            gnutls_x509_crt_deinit (cert);
            return -1;
        }

        if (gnutls_x509_crt_get_expiration_time (cert) < now) {
            remoteError(VIR_ERR_RPC, "%s", _("The certificate has expired"));
            gnutls_x509_crt_deinit (cert);
            return -1;
        }

        if (gnutls_x509_crt_get_activation_time (cert) > now) {
            remoteError(VIR_ERR_RPC, "%s",
                        _("The certificate is not yet activated"));
            gnutls_x509_crt_deinit (cert);
            return -1;
        }

        if (i == 0) {
            if (!gnutls_x509_crt_check_hostname (cert, priv->hostname)) {
                remoteError(VIR_ERR_RPC,
                            _("Certificate's owner does not match the hostname (%s)"),
                            priv->hostname);
                gnutls_x509_crt_deinit (cert);
                return -1;
            }
        }
    }

    return 0;
}

/*----------------------------------------------------------------------*/


static int
doRemoteClose (virConnectPtr conn, struct private_data *priv)
{
    /* Remove timer before closing the connection, to avoid possible
     * remoteDomainEventFired with a free'd connection */
    if (priv->domainEventState->timer >= 0) {
        virEventRemoveTimeout(priv->domainEventState->timer);
        virEventRemoveHandle(priv->watch);
        priv->watch = -1;
        priv->domainEventState->timer = -1;
    }

    if (call (conn, priv, 0, REMOTE_PROC_CLOSE,
              (xdrproc_t) xdr_void, (char *) NULL,
              (xdrproc_t) xdr_void, (char *) NULL) == -1)
        return -1;

    /* Close socket. */
    if (priv->uses_tls && priv->session) {
        gnutls_bye (priv->session, GNUTLS_SHUT_RDWR);
        gnutls_deinit (priv->session);
    }
#if HAVE_SASL
    if (priv->saslconn)
        sasl_dispose (&priv->saslconn);
#endif
    VIR_FORCE_CLOSE(priv->sock);
    VIR_FORCE_CLOSE(priv->errfd);

#ifndef WIN32
    if (priv->pid > 0) {
        pid_t reap;
        do {
retry:
            reap = waitpid(priv->pid, NULL, 0);
            if (reap == -1 && errno == EINTR)
                goto retry;
        } while (reap != -1 && reap != priv->pid);
    }
#endif
    VIR_FORCE_CLOSE(priv->wakeupReadFD);
    VIR_FORCE_CLOSE(priv->wakeupSendFD);


    /* Free hostname copy */
    VIR_FREE(priv->hostname);

    /* See comment for remoteType. */
    VIR_FREE(priv->type);

    virDomainEventStateFree(priv->domainEventState);

    return 0;
}

static int
remoteClose (virConnectPtr conn)
{
    int ret = 0;
    struct private_data *priv = conn->privateData;

    remoteDriverLock(priv);
    priv->localUses--;
    if (!priv->localUses) {
        ret = doRemoteClose(conn, priv);
        conn->privateData = NULL;
        remoteDriverUnlock(priv);
        virMutexDestroy(&priv->lock);
        VIR_FREE (priv);
    }
    if (priv)
        remoteDriverUnlock(priv);

    return ret;
}

/* Unfortunately this function is defined to return a static string.
 * Since the remote end always answers with the same type (for a
 * single connection anyway) we cache the type in the connection's
 * private data, and free it when we close the connection.
 *
 * See also:
 * http://www.redhat.com/archives/libvir-list/2007-February/msg00096.html
 */
static const char *
remoteType (virConnectPtr conn)
{
    char *rv = NULL;
    remote_get_type_ret ret;
    struct private_data *priv = conn->privateData;

    remoteDriverLock(priv);

    /* Cached? */
    if (priv->type) {
        rv = priv->type;
        goto done;
    }

    memset (&ret, 0, sizeof ret);
    if (call (conn, priv, 0, REMOTE_PROC_GET_TYPE,
              (xdrproc_t) xdr_void, (char *) NULL,
              (xdrproc_t) xdr_remote_get_type_ret, (char *) &ret) == -1)
        goto done;

    /* Stash. */
    rv = priv->type = ret.type;

done:
    remoteDriverUnlock(priv);
    return rv;
}

static int remoteIsSecure(virConnectPtr conn)
{
    int rv = -1;
    struct private_data *priv = conn->privateData;
    remote_is_secure_ret ret;
    remoteDriverLock(priv);

    memset (&ret, 0, sizeof ret);
    if (call (conn, priv, 0, REMOTE_PROC_IS_SECURE,
              (xdrproc_t) xdr_void, (char *) NULL,
              (xdrproc_t) xdr_remote_is_secure_ret, (char *) &ret) == -1)
        goto done;

    /* We claim to be secure, if the remote driver
     * transport itself is secure, and the remote
     * HV connection is secure
     *
     * ie, we don't want to claim to be secure if the
     * remote driver is used to connect to a XenD
     * driver using unencrypted HTTP:/// access
     */
    rv = priv->is_secure && ret.secure ? 1 : 0;

done:
    remoteDriverUnlock(priv);
    return rv;
}

static int remoteIsEncrypted(virConnectPtr conn)
{
    int rv = -1;
    int encrypted = 0;
    struct private_data *priv = conn->privateData;
    remote_is_secure_ret ret;
    remoteDriverLock(priv);

    memset (&ret, 0, sizeof ret);
    if (call (conn, priv, 0, REMOTE_PROC_IS_SECURE,
              (xdrproc_t) xdr_void, (char *) NULL,
              (xdrproc_t) xdr_remote_is_secure_ret, (char *) &ret) == -1)
        goto done;

    if (priv->uses_tls)
        encrypted = 1;
#if HAVE_SASL
    else if (priv->saslconn)
        encrypted = 1;
#endif


    /* We claim to be encrypted, if the remote driver
     * transport itself is encrypted, and the remote
     * HV connection is secure.
     *
     * Yes, we really don't check the remote 'encrypted'
     * option, since it will almost always be false,
     * even if secure (eg UNIX sockets).
     */
    rv = encrypted && ret.secure ? 1 : 0;

done:
    remoteDriverUnlock(priv);
    return rv;
}

static int
remoteNodeGetCellsFreeMemory(virConnectPtr conn,
                            unsigned long long *freeMems,
                            int startCell,
                            int maxCells)
{
    int rv = -1;
    remote_node_get_cells_free_memory_args args;
    remote_node_get_cells_free_memory_ret ret;
    int i;
    struct private_data *priv = conn->privateData;

    remoteDriverLock(priv);

    if (maxCells > REMOTE_NODE_MAX_CELLS) {
        remoteError(VIR_ERR_RPC,
                    _("too many NUMA cells: %d > %d"),
                    maxCells, REMOTE_NODE_MAX_CELLS);
        goto done;
    }

    args.startCell = startCell;
    args.maxcells = maxCells;

    memset (&ret, 0, sizeof ret);
    if (call (conn, priv, 0, REMOTE_PROC_NODE_GET_CELLS_FREE_MEMORY,
              (xdrproc_t) xdr_remote_node_get_cells_free_memory_args, (char *)&args,
              (xdrproc_t) xdr_remote_node_get_cells_free_memory_ret, (char *)&ret) == -1)
        goto done;

    for (i = 0 ; i < ret.cells.cells_len ; i++)
        freeMems[i] = ret.cells.cells_val[i];

    xdr_free((xdrproc_t) xdr_remote_node_get_cells_free_memory_ret, (char *) &ret);

    rv = ret.cells.cells_len;

done:
    remoteDriverUnlock(priv);
    return rv;
}

static int
remoteListDomains (virConnectPtr conn, int *ids, int maxids)
{
    int rv = -1;
    int i;
    remote_list_domains_args args;
    remote_list_domains_ret ret;
    struct private_data *priv = conn->privateData;

    remoteDriverLock(priv);

    if (maxids > REMOTE_DOMAIN_ID_LIST_MAX) {
        remoteError(VIR_ERR_RPC,
                    _("too many remote domain IDs: %d > %d"),
                    maxids, REMOTE_DOMAIN_ID_LIST_MAX);
        goto done;
    }
    args.maxids = maxids;

    memset (&ret, 0, sizeof ret);
    if (call (conn, priv, 0, REMOTE_PROC_LIST_DOMAINS,
              (xdrproc_t) xdr_remote_list_domains_args, (char *) &args,
              (xdrproc_t) xdr_remote_list_domains_ret, (char *) &ret) == -1)
        goto done;

    if (ret.ids.ids_len > maxids) {
        remoteError(VIR_ERR_RPC,
                    _("too many remote domain IDs: %d > %d"),
                    ret.ids.ids_len, maxids);
        goto cleanup;
    }

    for (i = 0; i < ret.ids.ids_len; ++i)
        ids[i] = ret.ids.ids_val[i];

    rv = ret.ids.ids_len;

cleanup:
    xdr_free ((xdrproc_t) xdr_remote_list_domains_ret, (char *) &ret);

done:
    remoteDriverUnlock(priv);
    return rv;
}

/* Helper to serialize typed parameters. */
static int
remoteSerializeTypedParameters(virTypedParameterPtr params,
                               int nparams,
                               u_int *args_params_len,
                               remote_typed_param **args_params_val)
{
    int i;
    int rv = -1;
    remote_typed_param *val;

    *args_params_len = nparams;
    if (VIR_ALLOC_N(val, nparams) < 0) {
        virReportOOMError();
        goto cleanup;
    }

    for (i = 0; i < nparams; ++i) {
        /* call() will free this: */
        val[i].field = strdup (params[i].field);
        if (val[i].field == NULL) {
            virReportOOMError();
            goto cleanup;
        }
        val[i].value.type = params[i].type;
        switch (params[i].type) {
        case VIR_TYPED_PARAM_INT:
            val[i].value.remote_typed_param_value_u.i = params[i].value.i;
            break;
        case VIR_TYPED_PARAM_UINT:
            val[i].value.remote_typed_param_value_u.ui = params[i].value.ui;
            break;
        case VIR_TYPED_PARAM_LLONG:
            val[i].value.remote_typed_param_value_u.l = params[i].value.l;
            break;
        case VIR_TYPED_PARAM_ULLONG:
            val[i].value.remote_typed_param_value_u.ul = params[i].value.ul;
            break;
        case VIR_TYPED_PARAM_DOUBLE:
            val[i].value.remote_typed_param_value_u.d = params[i].value.d;
            break;
        case VIR_TYPED_PARAM_BOOLEAN:
            val[i].value.remote_typed_param_value_u.b = params[i].value.b;
            break;
        default:
            remoteError(VIR_ERR_RPC, _("unknown parameter type: %d"),
                params[i].type);
            goto cleanup;
        }
    }

    *args_params_val = val;
    val = NULL;
    rv = 0;

cleanup:
    if (val) {
        for (i = 0; i < nparams; i++)
            VIR_FREE(val[i].field);
        VIR_FREE(val);
    }
    return rv;
}

/* Helper to deserialize typed parameters. */
static int
remoteDeserializeTypedParameters(u_int ret_params_len,
                                 remote_typed_param *ret_params_val,
                                 int limit,
                                 virTypedParameterPtr params,
                                 int *nparams)
{
    int i;
    int rv = -1;

    /* Check the length of the returned list carefully. */
    if (ret_params_len > limit || ret_params_len > *nparams) {
        remoteError(VIR_ERR_RPC, "%s",
                    _("returned number of parameters exceeds limit"));
        goto cleanup;
    }

    *nparams = ret_params_len;

    /* Deserialise the result. */
    for (i = 0; i < ret_params_len; ++i) {
        if (virStrcpyStatic(params[i].field,
                            ret_params_val[i].field) == NULL) {
            remoteError(VIR_ERR_INTERNAL_ERROR,
                        _("Parameter %s too big for destination"),
                        ret_params_val[i].field);
            goto cleanup;
        }
        params[i].type = ret_params_val[i].value.type;
        switch (params[i].type) {
        case VIR_TYPED_PARAM_INT:
            params[i].value.i =
                ret_params_val[i].value.remote_typed_param_value_u.i;
            break;
        case VIR_TYPED_PARAM_UINT:
            params[i].value.ui =
                ret_params_val[i].value.remote_typed_param_value_u.ui;
            break;
        case VIR_TYPED_PARAM_LLONG:
            params[i].value.l =
                ret_params_val[i].value.remote_typed_param_value_u.l;
            break;
        case VIR_TYPED_PARAM_ULLONG:
            params[i].value.ul =
                ret_params_val[i].value.remote_typed_param_value_u.ul;
            break;
        case VIR_TYPED_PARAM_DOUBLE:
            params[i].value.d =
                ret_params_val[i].value.remote_typed_param_value_u.d;
            break;
        case VIR_TYPED_PARAM_BOOLEAN:
            params[i].value.b =
                ret_params_val[i].value.remote_typed_param_value_u.b;
            break;
        default:
            remoteError(VIR_ERR_RPC, _("unknown parameter type: %d"),
                        params[i].type);
            goto cleanup;
        }
    }

    rv = 0;

cleanup:
    return rv;
}

static int
remoteDomainSetMemoryParameters (virDomainPtr domain,
                                 virTypedParameterPtr params,
                                 int nparams,
                                 unsigned int flags)
{
    int rv = -1;
    remote_domain_set_memory_parameters_args args;
    struct private_data *priv = domain->conn->privateData;

    remoteDriverLock(priv);

    make_nonnull_domain (&args.dom, domain);

    args.flags = flags;

    if (remoteSerializeTypedParameters(params, nparams,
                                       &args.params.params_len,
                                       &args.params.params_val) < 0) {
        xdr_free ((xdrproc_t) xdr_remote_domain_set_memory_parameters_args,
                  (char *) &args);
        goto done;
    }

    if (call (domain->conn, priv, 0, REMOTE_PROC_DOMAIN_SET_MEMORY_PARAMETERS,
              (xdrproc_t) xdr_remote_domain_set_memory_parameters_args,
              (char *) &args, (xdrproc_t) xdr_void, (char *) NULL) == -1)
        goto done;

    rv = 0;

done:
    remoteDriverUnlock(priv);
    return rv;
}

static int
remoteDomainGetMemoryParameters (virDomainPtr domain,
                                 virTypedParameterPtr params, int *nparams,
                                 unsigned int flags)
{
    int rv = -1;
    remote_domain_get_memory_parameters_args args;
    remote_domain_get_memory_parameters_ret ret;
    struct private_data *priv = domain->conn->privateData;

    remoteDriverLock(priv);

    make_nonnull_domain (&args.dom, domain);
    args.nparams = *nparams;
    args.flags = flags;

    memset (&ret, 0, sizeof ret);
    if (call (domain->conn, priv, 0, REMOTE_PROC_DOMAIN_GET_MEMORY_PARAMETERS,
              (xdrproc_t) xdr_remote_domain_get_memory_parameters_args, (char *) &args,
              (xdrproc_t) xdr_remote_domain_get_memory_parameters_ret, (char *) &ret) == -1)
        goto done;

    /* Handle the case when the caller does not know the number of parameters
     * and is asking for the number of parameters supported
     */
    if (*nparams == 0) {
        *nparams = ret.nparams;
        rv = 0;
        goto cleanup;
    }

    if (remoteDeserializeTypedParameters(ret.params.params_len,
                                         ret.params.params_val,
                                         REMOTE_DOMAIN_MEMORY_PARAMETERS_MAX,
                                         params,
                                         nparams) < 0)
        goto cleanup;

    rv = 0;

cleanup:
    xdr_free ((xdrproc_t) xdr_remote_domain_get_memory_parameters_ret,
              (char *) &ret);
done:
    remoteDriverUnlock(priv);
    return rv;
}

static int
remoteDomainSetBlkioParameters (virDomainPtr domain,
                                virTypedParameterPtr params,
                                int nparams,
                                unsigned int flags)
{
    int rv = -1;
    remote_domain_set_blkio_parameters_args args;
    struct private_data *priv = domain->conn->privateData;

    remoteDriverLock(priv);

    make_nonnull_domain (&args.dom, domain);

    args.flags = flags;

    if (remoteSerializeTypedParameters(params, nparams,
                                       &args.params.params_len,
                                       &args.params.params_val) < 0) {
        xdr_free ((xdrproc_t) xdr_remote_domain_set_blkio_parameters_args,
                  (char *) &args);
        goto done;
    }

    if (call (domain->conn, priv, 0, REMOTE_PROC_DOMAIN_SET_BLKIO_PARAMETERS,
              (xdrproc_t) xdr_remote_domain_set_blkio_parameters_args,
              (char *) &args, (xdrproc_t) xdr_void, (char *) NULL) == -1)
        goto done;

    rv = 0;

done:
    remoteDriverUnlock(priv);
    return rv;
}

static int
remoteDomainGetBlkioParameters (virDomainPtr domain,
                                virTypedParameterPtr params, int *nparams,
                                unsigned int flags)
{
    int rv = -1;
    remote_domain_get_blkio_parameters_args args;
    remote_domain_get_blkio_parameters_ret ret;
    struct private_data *priv = domain->conn->privateData;

    remoteDriverLock(priv);

    make_nonnull_domain (&args.dom, domain);
    args.nparams = *nparams;
    args.flags = flags;

    memset (&ret, 0, sizeof ret);
    if (call (domain->conn, priv, 0, REMOTE_PROC_DOMAIN_GET_BLKIO_PARAMETERS,
              (xdrproc_t) xdr_remote_domain_get_blkio_parameters_args, (char *) &args,
              (xdrproc_t) xdr_remote_domain_get_blkio_parameters_ret, (char *) &ret) == -1)
        goto done;

    /* Handle the case when the caller does not know the number of parameters
     * and is asking for the number of parameters supported
     */
    if (*nparams == 0) {
        *nparams = ret.nparams;
        rv = 0;
        goto cleanup;
    }

    if (remoteDeserializeTypedParameters(ret.params.params_len,
                                         ret.params.params_val,
                                         REMOTE_DOMAIN_BLKIO_PARAMETERS_MAX,
                                         params,
                                         nparams) < 0)
        goto cleanup;

    rv = 0;

cleanup:
    xdr_free ((xdrproc_t) xdr_remote_domain_get_blkio_parameters_ret,
              (char *) &ret);
done:
    remoteDriverUnlock(priv);
    return rv;
}

static int
remoteDomainGetVcpus (virDomainPtr domain,
                      virVcpuInfoPtr info,
                      int maxinfo,
                      unsigned char *cpumaps,
                      int maplen)
{
    int rv = -1;
    int i;
    remote_domain_get_vcpus_args args;
    remote_domain_get_vcpus_ret ret;
    struct private_data *priv = domain->conn->privateData;

    remoteDriverLock(priv);

    if (maxinfo > REMOTE_VCPUINFO_MAX) {
        remoteError(VIR_ERR_RPC,
                    _("vCPU count exceeds maximum: %d > %d"),
                    maxinfo, REMOTE_VCPUINFO_MAX);
        goto done;
    }
    if (maxinfo * maplen > REMOTE_CPUMAPS_MAX) {
        remoteError(VIR_ERR_RPC,
                    _("vCPU map buffer length exceeds maximum: %d > %d"),
                    maxinfo * maplen, REMOTE_CPUMAPS_MAX);
        goto done;
    }

    make_nonnull_domain (&args.dom, domain);
    args.maxinfo = maxinfo;
    args.maplen = maplen;

    memset (&ret, 0, sizeof ret);
    if (call (domain->conn, priv, 0, REMOTE_PROC_DOMAIN_GET_VCPUS,
              (xdrproc_t) xdr_remote_domain_get_vcpus_args, (char *) &args,
              (xdrproc_t) xdr_remote_domain_get_vcpus_ret, (char *) &ret) == -1)
        goto done;

    if (ret.info.info_len > maxinfo) {
        remoteError(VIR_ERR_RPC,
                    _("host reports too many vCPUs: %d > %d"),
                    ret.info.info_len, maxinfo);
        goto cleanup;
    }
    if (ret.cpumaps.cpumaps_len > maxinfo * maplen) {
        remoteError(VIR_ERR_RPC,
                    _("host reports map buffer length exceeds maximum: %d > %d"),
                    ret.cpumaps.cpumaps_len, maxinfo * maplen);
        goto cleanup;
    }

    memset (info, 0, sizeof (virVcpuInfo) * maxinfo);
    memset (cpumaps, 0, maxinfo * maplen);

    for (i = 0; i < ret.info.info_len; ++i) {
        info[i].number = ret.info.info_val[i].number;
        info[i].state = ret.info.info_val[i].state;
        info[i].cpuTime = ret.info.info_val[i].cpu_time;
        info[i].cpu = ret.info.info_val[i].cpu;
    }

    for (i = 0; i < ret.cpumaps.cpumaps_len; ++i)
        cpumaps[i] = ret.cpumaps.cpumaps_val[i];

    rv = ret.info.info_len;

cleanup:
    xdr_free ((xdrproc_t) xdr_remote_domain_get_vcpus_ret, (char *) &ret);

done:
    remoteDriverUnlock(priv);
    return rv;
}

static int
remoteDomainGetSecurityLabel (virDomainPtr domain, virSecurityLabelPtr seclabel)
{
    remote_domain_get_security_label_args args;
    remote_domain_get_security_label_ret ret;
    struct private_data *priv = domain->conn->privateData;
    int rv = -1;

    remoteDriverLock(priv);

    make_nonnull_domain (&args.dom, domain);
    memset (&ret, 0, sizeof ret);
    memset (seclabel, 0, sizeof (*seclabel));

    if (call (domain->conn, priv, 0, REMOTE_PROC_DOMAIN_GET_SECURITY_LABEL,
              (xdrproc_t) xdr_remote_domain_get_security_label_args, (char *)&args,
              (xdrproc_t) xdr_remote_domain_get_security_label_ret, (char *)&ret) == -1) {
        goto done;
    }

    if (ret.label.label_val != NULL) {
        if (strlen (ret.label.label_val) >= sizeof seclabel->label) {
            remoteError(VIR_ERR_RPC, _("security label exceeds maximum: %zd"),
                        sizeof seclabel->label - 1);
            goto done;
        }
        strcpy (seclabel->label, ret.label.label_val);
        seclabel->enforcing = ret.enforcing;
    }

    rv = 0;

done:
    remoteDriverUnlock(priv);
    return rv;
}

static int
remoteDomainGetState(virDomainPtr domain,
                     int *state,
                     int *reason,
                     unsigned int flags)
{
    int rv = -1;
    remote_domain_get_state_args args;
    remote_domain_get_state_ret ret;
    struct private_data *priv = domain->conn->privateData;

    remoteDriverLock(priv);

    make_nonnull_domain(&args.dom, domain);
    args.flags = flags;

    memset(&ret, 0, sizeof ret);
    if (call(domain->conn, priv, 0, REMOTE_PROC_DOMAIN_GET_STATE,
             (xdrproc_t) xdr_remote_domain_get_state_args, (char *) &args,
             (xdrproc_t) xdr_remote_domain_get_state_ret, (char *) &ret) == -1)
        goto done;

    *state = ret.state;
    if (reason)
        *reason = ret.reason;

    rv = 0;

done:
    remoteDriverUnlock(priv);
    return rv;
}

static int
remoteNodeGetSecurityModel (virConnectPtr conn, virSecurityModelPtr secmodel)
{
    remote_node_get_security_model_ret ret;
    struct private_data *priv = conn->privateData;
    int rv = -1;

    remoteDriverLock(priv);

    memset (&ret, 0, sizeof ret);
    memset (secmodel, 0, sizeof (*secmodel));

    if (call (conn, priv, 0, REMOTE_PROC_NODE_GET_SECURITY_MODEL,
              (xdrproc_t) xdr_void, NULL,
              (xdrproc_t) xdr_remote_node_get_security_model_ret, (char *)&ret) == -1) {
        goto done;
    }

    if (ret.model.model_val != NULL) {
        if (strlen (ret.model.model_val) >= sizeof secmodel->model) {
            remoteError(VIR_ERR_RPC, _("security model exceeds maximum: %zd"),
                        sizeof secmodel->model - 1);
            goto done;
        }
        strcpy (secmodel->model, ret.model.model_val);
    }

    if (ret.doi.doi_val != NULL) {
        if (strlen (ret.doi.doi_val) >= sizeof secmodel->doi) {
            remoteError(VIR_ERR_RPC, _("security doi exceeds maximum: %zd"),
                        sizeof secmodel->doi - 1);
            goto done;
        }
        strcpy (secmodel->doi, ret.doi.doi_val);
    }

    rv = 0;

done:
    remoteDriverUnlock(priv);
    return rv;
}

static int
remoteDomainMigratePrepare (virConnectPtr dconn,
                            char **cookie, int *cookielen,
                            const char *uri_in, char **uri_out,
                            unsigned long flags, const char *dname,
                            unsigned long resource)
{
    int rv = -1;
    remote_domain_migrate_prepare_args args;
    remote_domain_migrate_prepare_ret ret;
    struct private_data *priv = dconn->privateData;

    remoteDriverLock(priv);

    args.uri_in = uri_in == NULL ? NULL : (char **) &uri_in;
    args.flags = flags;
    args.dname = dname == NULL ? NULL : (char **) &dname;
    args.resource = resource;

    memset (&ret, 0, sizeof ret);
    if (call (dconn, priv, 0, REMOTE_PROC_DOMAIN_MIGRATE_PREPARE,
              (xdrproc_t) xdr_remote_domain_migrate_prepare_args, (char *) &args,
              (xdrproc_t) xdr_remote_domain_migrate_prepare_ret, (char *) &ret) == -1)
        goto done;

    if (ret.cookie.cookie_len > 0) {
        *cookie = ret.cookie.cookie_val; /* Caller frees. */
        *cookielen = ret.cookie.cookie_len;
    }
    if (ret.uri_out)
        *uri_out = *ret.uri_out; /* Caller frees. */

    rv = 0;

done:
    remoteDriverUnlock(priv);
    return rv;
}

static int
remoteDomainMigratePrepare2 (virConnectPtr dconn,
                             char **cookie, int *cookielen,
                             const char *uri_in, char **uri_out,
                             unsigned long flags, const char *dname,
                             unsigned long resource,
                             const char *dom_xml)
{
    int rv = -1;
    remote_domain_migrate_prepare2_args args;
    remote_domain_migrate_prepare2_ret ret;
    struct private_data *priv = dconn->privateData;

    remoteDriverLock(priv);

    args.uri_in = uri_in == NULL ? NULL : (char **) &uri_in;
    args.flags = flags;
    args.dname = dname == NULL ? NULL : (char **) &dname;
    args.resource = resource;
    args.dom_xml = (char *) dom_xml;

    memset (&ret, 0, sizeof ret);
    if (call (dconn, priv, 0, REMOTE_PROC_DOMAIN_MIGRATE_PREPARE2,
              (xdrproc_t) xdr_remote_domain_migrate_prepare2_args, (char *) &args,
              (xdrproc_t) xdr_remote_domain_migrate_prepare2_ret, (char *) &ret) == -1)
        goto done;

    if (ret.cookie.cookie_len > 0) {
        if (!cookie || !cookielen) {
            remoteError(VIR_ERR_INTERNAL_ERROR, "%s",
                        _("caller ignores cookie or cookielen"));
            goto error;
        }
        *cookie = ret.cookie.cookie_val; /* Caller frees. */
        *cookielen = ret.cookie.cookie_len;
    }
    if (ret.uri_out) {
        if (!uri_out) {
            remoteError(VIR_ERR_INTERNAL_ERROR, "%s",
                        _("caller ignores uri_out"));
            goto error;
        }
        *uri_out = *ret.uri_out; /* Caller frees. */
    }

    rv = 0;

done:
    remoteDriverUnlock(priv);
    return rv;
error:
    if (ret.cookie.cookie_len)
        VIR_FREE(ret.cookie.cookie_val);
    if (ret.uri_out)
        VIR_FREE(*ret.uri_out);
    goto done;
}

static int
remoteDomainCreate (virDomainPtr domain)
{
    int rv = -1;
    remote_domain_create_args args;
    remote_domain_lookup_by_uuid_args args2;
    remote_domain_lookup_by_uuid_ret ret2;
    struct private_data *priv = domain->conn->privateData;

    remoteDriverLock(priv);

    make_nonnull_domain (&args.dom, domain);

    if (call (domain->conn, priv, 0, REMOTE_PROC_DOMAIN_CREATE,
              (xdrproc_t) xdr_remote_domain_create_args, (char *) &args,
              (xdrproc_t) xdr_void, (char *) NULL) == -1)
        goto done;

    /* Need to do a lookup figure out ID of newly started guest, because
     * bug in design of REMOTE_PROC_DOMAIN_CREATE means we aren't getting
     * it returned.
     */
    memcpy (args2.uuid, domain->uuid, VIR_UUID_BUFLEN);
    memset (&ret2, 0, sizeof ret2);
    if (call (domain->conn, priv, 0, REMOTE_PROC_DOMAIN_LOOKUP_BY_UUID,
              (xdrproc_t) xdr_remote_domain_lookup_by_uuid_args, (char *) &args2,
              (xdrproc_t) xdr_remote_domain_lookup_by_uuid_ret, (char *) &ret2) == -1)
        goto done;

    domain->id = ret2.dom.id;
    xdr_free ((xdrproc_t) &xdr_remote_domain_lookup_by_uuid_ret, (char *) &ret2);

    rv = 0;

done:
    remoteDriverUnlock(priv);
    return rv;
}

static char *
remoteDomainGetSchedulerType (virDomainPtr domain, int *nparams)
{
    char *rv = NULL;
    remote_domain_get_scheduler_type_args args;
    remote_domain_get_scheduler_type_ret ret;
    struct private_data *priv = domain->conn->privateData;

    remoteDriverLock(priv);

    make_nonnull_domain (&args.dom, domain);

    memset (&ret, 0, sizeof ret);
    if (call (domain->conn, priv, 0, REMOTE_PROC_DOMAIN_GET_SCHEDULER_TYPE,
              (xdrproc_t) xdr_remote_domain_get_scheduler_type_args, (char *) &args,
              (xdrproc_t) xdr_remote_domain_get_scheduler_type_ret, (char *) &ret) == -1)
        goto done;

    if (nparams) *nparams = ret.nparams;

    /* Caller frees this. */
    rv = ret.type;

done:
    remoteDriverUnlock(priv);
    return rv;
}

static int
remoteDomainGetSchedulerParameters (virDomainPtr domain,
                                    virTypedParameterPtr params, int *nparams)
{
    int rv = -1;
    remote_domain_get_scheduler_parameters_args args;
    remote_domain_get_scheduler_parameters_ret ret;
    struct private_data *priv = domain->conn->privateData;

    remoteDriverLock(priv);

    make_nonnull_domain (&args.dom, domain);
    args.nparams = *nparams;

    memset (&ret, 0, sizeof ret);
    if (call (domain->conn, priv, 0, REMOTE_PROC_DOMAIN_GET_SCHEDULER_PARAMETERS,
              (xdrproc_t) xdr_remote_domain_get_scheduler_parameters_args, (char *) &args,
              (xdrproc_t) xdr_remote_domain_get_scheduler_parameters_ret, (char *) &ret) == -1)
        goto done;

    if (remoteDeserializeTypedParameters(ret.params.params_len,
                                         ret.params.params_val,
                                         REMOTE_DOMAIN_SCHEDULER_PARAMETERS_MAX,
                                         params,
                                         nparams) < 0)
        goto cleanup;

    rv = 0;

cleanup:
    xdr_free ((xdrproc_t) xdr_remote_domain_get_scheduler_parameters_ret, (char *) &ret);
done:
    remoteDriverUnlock(priv);
    return rv;
}

static int
remoteDomainSetSchedulerParameters (virDomainPtr domain,
                                    virTypedParameterPtr params, int nparams)
{
    int rv = -1;
    remote_domain_set_scheduler_parameters_args args;
    struct private_data *priv = domain->conn->privateData;

    remoteDriverLock(priv);

    make_nonnull_domain (&args.dom, domain);

    if (remoteSerializeTypedParameters(params, nparams,
                                       &args.params.params_len,
                                       &args.params.params_val) < 0) {
        xdr_free ((xdrproc_t) xdr_remote_domain_set_scheduler_parameters_args,
                  (char *) &args);
        goto done;
    }

    if (call (domain->conn, priv, 0, REMOTE_PROC_DOMAIN_SET_SCHEDULER_PARAMETERS,
              (xdrproc_t) xdr_remote_domain_set_scheduler_parameters_args, (char *) &args,
              (xdrproc_t) xdr_void, (char *) NULL) == -1)
        goto done;

    rv = 0;

done:
    remoteDriverUnlock(priv);
    return rv;
}

static int
remoteDomainSetSchedulerParametersFlags(virDomainPtr domain,
                                        virTypedParameterPtr params,
                                        int nparams,
                                        unsigned int flags)
{
    int rv = -1;
    remote_domain_set_scheduler_parameters_flags_args args;
    struct private_data *priv = domain->conn->privateData;

    remoteDriverLock(priv);

    make_nonnull_domain (&args.dom, domain);

    args.flags = flags;

    if (remoteSerializeTypedParameters(params, nparams,
                                       &args.params.params_len,
                                       &args.params.params_val) < 0) {
        xdr_free ((xdrproc_t) xdr_remote_domain_set_scheduler_parameters_flags_args,
                  (char *) &args);
        goto done;
    }

    if (call (domain->conn, priv, 0, REMOTE_PROC_DOMAIN_SET_SCHEDULER_PARAMETERS_FLAGS,
              (xdrproc_t) xdr_remote_domain_set_scheduler_parameters_flags_args, (char *) &args,
              (xdrproc_t) xdr_void, (char *) NULL) == -1)
        goto done;

    rv = 0;

done:
    remoteDriverUnlock(priv);
    return rv;
}

static int
remoteDomainMemoryStats (virDomainPtr domain,
                         struct _virDomainMemoryStat *stats,
                         unsigned int nr_stats)
{
    int rv = -1;
    remote_domain_memory_stats_args args;
    remote_domain_memory_stats_ret ret;
    struct private_data *priv = domain->conn->privateData;
    unsigned int i;

    remoteDriverLock(priv);

    make_nonnull_domain (&args.dom, domain);
    if (nr_stats > REMOTE_DOMAIN_MEMORY_STATS_MAX) {
        remoteError(VIR_ERR_RPC,
                    _("too many memory stats requested: %d > %d"), nr_stats,
                    REMOTE_DOMAIN_MEMORY_STATS_MAX);
        goto done;
    }
    args.maxStats = nr_stats;
    args.flags = 0;
    memset (&ret, 0, sizeof ret);

    if (call (domain->conn, priv, 0, REMOTE_PROC_DOMAIN_MEMORY_STATS,
              (xdrproc_t) xdr_remote_domain_memory_stats_args,
                (char *) &args,
              (xdrproc_t) xdr_remote_domain_memory_stats_ret,
                (char *) &ret) == -1)
        goto done;

    for (i = 0; i < ret.stats.stats_len; i++) {
        stats[i].tag = ret.stats.stats_val[i].tag;
        stats[i].val = ret.stats.stats_val[i].val;
    }
    rv = ret.stats.stats_len;
    xdr_free((xdrproc_t) xdr_remote_domain_memory_stats_ret, (char *) &ret);

done:
    remoteDriverUnlock(priv);
    return rv;
}

static int
remoteDomainBlockPeek (virDomainPtr domain,
                       const char *path,
                       unsigned long long offset,
                       size_t size,
                       void *buffer,
                       unsigned int flags)
{
    int rv = -1;
    remote_domain_block_peek_args args;
    remote_domain_block_peek_ret ret;
    struct private_data *priv = domain->conn->privateData;

    remoteDriverLock(priv);

    if (size > REMOTE_DOMAIN_BLOCK_PEEK_BUFFER_MAX) {
        remoteError(VIR_ERR_RPC,
                    _("block peek request too large for remote protocol, %zi > %d"),
                    size, REMOTE_DOMAIN_BLOCK_PEEK_BUFFER_MAX);
        goto done;
    }

    make_nonnull_domain (&args.dom, domain);
    args.path = (char *) path;
    args.offset = offset;
    args.size = size;
    args.flags = flags;

    memset (&ret, 0, sizeof ret);
    if (call (domain->conn, priv, 0, REMOTE_PROC_DOMAIN_BLOCK_PEEK,
              (xdrproc_t) xdr_remote_domain_block_peek_args,
                (char *) &args,
              (xdrproc_t) xdr_remote_domain_block_peek_ret,
                (char *) &ret) == -1)
        goto done;

    if (ret.buffer.buffer_len != size) {
        remoteError(VIR_ERR_RPC, "%s",
                    _("returned buffer is not same size as requested"));
        goto cleanup;
    }

    memcpy (buffer, ret.buffer.buffer_val, size);
    rv = 0;

cleanup:
    VIR_FREE(ret.buffer.buffer_val);

done:
    remoteDriverUnlock(priv);
    return rv;
}

static int
remoteDomainMemoryPeek (virDomainPtr domain,
                        unsigned long long offset,
                        size_t size,
                        void *buffer,
                        unsigned int flags)
{
    int rv = -1;
    remote_domain_memory_peek_args args;
    remote_domain_memory_peek_ret ret;
    struct private_data *priv = domain->conn->privateData;

    remoteDriverLock(priv);

    if (size > REMOTE_DOMAIN_MEMORY_PEEK_BUFFER_MAX) {
        remoteError(VIR_ERR_RPC,
                    _("memory peek request too large for remote protocol, %zi > %d"),
                    size, REMOTE_DOMAIN_MEMORY_PEEK_BUFFER_MAX);
        goto done;
    }

    make_nonnull_domain (&args.dom, domain);
    args.offset = offset;
    args.size = size;
    args.flags = flags;

    memset (&ret, 0, sizeof ret);
    if (call (domain->conn, priv, 0, REMOTE_PROC_DOMAIN_MEMORY_PEEK,
              (xdrproc_t) xdr_remote_domain_memory_peek_args,
                (char *) &args,
              (xdrproc_t) xdr_remote_domain_memory_peek_ret,
                (char *) &ret) == -1)
        goto done;

    if (ret.buffer.buffer_len != size) {
        remoteError(VIR_ERR_RPC, "%s",
                    _("returned buffer is not same size as requested"));
        goto cleanup;
    }

    memcpy (buffer, ret.buffer.buffer_val, size);
    rv = 0;

cleanup:
    VIR_FREE(ret.buffer.buffer_val);

done:
    remoteDriverUnlock(priv);
    return rv;
}

/*----------------------------------------------------------------------*/

static virDrvOpenStatus ATTRIBUTE_NONNULL (1)
remoteGenericOpen(virConnectPtr conn, virConnectAuthPtr auth,
                  int flags, void **genericPrivateData)
{
    if (inside_daemon)
        return VIR_DRV_OPEN_DECLINED;

    if (conn->driver &&
        STREQ (conn->driver->name, "remote")) {
        struct private_data *priv;

        /* If we're here, the remote driver is already
         * in use due to a) a QEMU uri, or b) a remote
         * URI. So we can re-use existing connection */
        priv = conn->privateData;
        remoteDriverLock(priv);
        priv->localUses++;
        *genericPrivateData = priv;
        remoteDriverUnlock(priv);
        return VIR_DRV_OPEN_SUCCESS;
    } else if (conn->networkDriver &&
               STREQ (conn->networkDriver->name, "remote")) {
        struct private_data *priv = conn->networkPrivateData;
        remoteDriverLock(priv);
        *genericPrivateData = priv;
        priv->localUses++;
        remoteDriverUnlock(priv);
        return VIR_DRV_OPEN_SUCCESS;
    } else {
        /* Using a non-remote driver, so we need to open a
         * new connection for network APIs, forcing it to
         * use the UNIX transport. This handles Xen driver
         * which doesn't have its own impl of the network APIs. */
        struct private_data *priv;
        int ret;
        ret = remoteOpenSecondaryDriver(conn, auth, flags, &priv);
        if (ret == VIR_DRV_OPEN_SUCCESS)
            *genericPrivateData = priv;
        return ret;
    }
}

static int
remoteGenericClose(virConnectPtr conn, void **genericPrivateData)
{
    int rv = 0;
    struct private_data *priv = *genericPrivateData;

    remoteDriverLock(priv);
    priv->localUses--;
    if (!priv->localUses) {
        rv = doRemoteClose(conn, priv);
        *genericPrivateData = NULL;
        remoteDriverUnlock(priv);
        virMutexDestroy(&priv->lock);
        VIR_FREE(priv);
    }
    if (priv)
        remoteDriverUnlock(priv);
    return rv;
}

static virDrvOpenStatus ATTRIBUTE_NONNULL (1)
remoteNetworkOpen(virConnectPtr conn, virConnectAuthPtr auth, int flags)
{
    return remoteGenericOpen(conn, auth, flags, &conn->networkPrivateData);
}

static int
remoteNetworkClose(virConnectPtr conn)
{
    return remoteGenericClose(conn, &conn->networkPrivateData);
}

/*----------------------------------------------------------------------*/

static virDrvOpenStatus ATTRIBUTE_NONNULL (1)
remoteInterfaceOpen(virConnectPtr conn, virConnectAuthPtr auth, int flags)
{
    return remoteGenericOpen(conn, auth, flags, &conn->interfacePrivateData);
}

static int
remoteInterfaceClose(virConnectPtr conn)
{
    return remoteGenericClose(conn, &conn->interfacePrivateData);
}

/*----------------------------------------------------------------------*/

static virDrvOpenStatus ATTRIBUTE_NONNULL (1)
remoteStorageOpen(virConnectPtr conn, virConnectAuthPtr auth, int flags)
{
    return remoteGenericOpen(conn, auth, flags, &conn->storagePrivateData);
}

static int
remoteStorageClose(virConnectPtr conn)
{
    return remoteGenericClose(conn, &conn->storagePrivateData);
}

static char *
remoteFindStoragePoolSources (virConnectPtr conn,
                              const char *type,
                              const char *srcSpec,
                              unsigned int flags)
{
    char *rv = NULL;
    remote_find_storage_pool_sources_args args;
    remote_find_storage_pool_sources_ret ret;
    struct private_data *priv = conn->storagePrivateData;
    const char *emptyString = "";

    remoteDriverLock(priv);

    args.type = (char*)type;
    /*
     * I'd think the following would work here:
     *    args.srcSpec = (char**)&srcSpec;
     * since srcSpec is a remote_string (not a remote_nonnull_string).
     *
     * But when srcSpec is NULL, this yields:
     *    libvir: Remote error : marshaling args
     *
     * So for now I'm working around this by turning NULL srcSpecs
     * into empty strings.
     */
    args.srcSpec = srcSpec ? (char **)&srcSpec : (char **)&emptyString;
    args.flags = flags;

    memset (&ret, 0, sizeof ret);
    if (call (conn, priv, 0, REMOTE_PROC_FIND_STORAGE_POOL_SOURCES,
              (xdrproc_t) xdr_remote_find_storage_pool_sources_args, (char *) &args,
              (xdrproc_t) xdr_remote_find_storage_pool_sources_ret, (char *) &ret) == -1)
        goto done;

    rv = ret.xml;
    ret.xml = NULL; /* To stop xdr_free free'ing it */

    xdr_free ((xdrproc_t) xdr_remote_find_storage_pool_sources_ret, (char *) &ret);

done:
    remoteDriverUnlock(priv);
    return rv;
}

/*----------------------------------------------------------------------*/

static virDrvOpenStatus ATTRIBUTE_NONNULL (1)
remoteDevMonOpen(virConnectPtr conn, virConnectAuthPtr auth, int flags)
{
    return remoteGenericOpen(conn, auth, flags, &conn->devMonPrivateData);
}

static int
remoteDevMonClose(virConnectPtr conn)
{
    return remoteGenericClose(conn, &conn->devMonPrivateData);
}

static int
remoteNodeDeviceDettach (virNodeDevicePtr dev)
{
    int rv = -1;
    remote_node_device_dettach_args args;
    /* This method is unusual in that it uses the HV driver, not the devMon driver
     * hence its use of privateData, instead of devMonPrivateData */
    struct private_data *priv = dev->conn->privateData;

    remoteDriverLock(priv);

    args.name = dev->name;

    if (call (dev->conn, priv, 0, REMOTE_PROC_NODE_DEVICE_DETTACH,
              (xdrproc_t) xdr_remote_node_device_dettach_args, (char *) &args,
              (xdrproc_t) xdr_void, (char *) NULL) == -1)
        goto done;

    rv = 0;

done:
    remoteDriverUnlock(priv);
    return rv;
}

static int
remoteNodeDeviceReAttach (virNodeDevicePtr dev)
{
    int rv = -1;
    remote_node_device_re_attach_args args;
    /* This method is unusual in that it uses the HV driver, not the devMon driver
     * hence its use of privateData, instead of devMonPrivateData */
    struct private_data *priv = dev->conn->privateData;

    remoteDriverLock(priv);

    args.name = dev->name;

    if (call (dev->conn, priv, 0, REMOTE_PROC_NODE_DEVICE_RE_ATTACH,
              (xdrproc_t) xdr_remote_node_device_re_attach_args, (char *) &args,
              (xdrproc_t) xdr_void, (char *) NULL) == -1)
        goto done;

    rv = 0;

done:
    remoteDriverUnlock(priv);
    return rv;
}

static int
remoteNodeDeviceReset (virNodeDevicePtr dev)
{
    int rv = -1;
    remote_node_device_reset_args args;
    /* This method is unusual in that it uses the HV driver, not the devMon driver
     * hence its use of privateData, instead of devMonPrivateData */
    struct private_data *priv = dev->conn->privateData;

    remoteDriverLock(priv);

    args.name = dev->name;

    if (call (dev->conn, priv, 0, REMOTE_PROC_NODE_DEVICE_RESET,
              (xdrproc_t) xdr_remote_node_device_reset_args, (char *) &args,
              (xdrproc_t) xdr_void, (char *) NULL) == -1)
        goto done;

    rv = 0;

done:
    remoteDriverUnlock(priv);
    return rv;
}

/* ------------------------------------------------------------- */

static virDrvOpenStatus ATTRIBUTE_NONNULL (1)
remoteNWFilterOpen(virConnectPtr conn, virConnectAuthPtr auth, int flags)
{
    return remoteGenericOpen(conn, auth, flags, &conn->nwfilterPrivateData);
}

static int
remoteNWFilterClose(virConnectPtr conn)
{
    return remoteGenericClose(conn, &conn->nwfilterPrivateData);
}

/*----------------------------------------------------------------------*/

static int
remoteAuthenticate (virConnectPtr conn, struct private_data *priv,
                    int in_open ATTRIBUTE_UNUSED,
                    virConnectAuthPtr auth ATTRIBUTE_UNUSED,
                    const char *authtype)
{
    struct remote_auth_list_ret ret;
    int err, type = REMOTE_AUTH_NONE;

    memset(&ret, 0, sizeof ret);
    err = call (conn, priv,
                REMOTE_CALL_IN_OPEN | REMOTE_CALL_QUIET_MISSING_RPC,
                REMOTE_PROC_AUTH_LIST,
                (xdrproc_t) xdr_void, (char *) NULL,
                (xdrproc_t) xdr_remote_auth_list_ret, (char *) &ret);
    if (err == -2) /* Missing RPC - old server - ignore */
        return 0;

    if (err < 0)
        return -1;

    if (ret.types.types_len == 0)
        return 0;

    if (authtype) {
        int want, i;
        if (STRCASEEQ(authtype, "sasl") ||
            STRCASEEQLEN(authtype, "sasl.", 5)) {
            want = REMOTE_AUTH_SASL;
        } else if (STRCASEEQ(authtype, "polkit")) {
            want = REMOTE_AUTH_POLKIT;
        } else {
            remoteError(VIR_ERR_AUTH_FAILED,
                        _("unknown authentication type %s"), authtype);
            return -1;
        }
        for (i = 0 ; i < ret.types.types_len ; i++) {
            if (ret.types.types_val[i] == want)
                type = want;
        }
        if (type == REMOTE_AUTH_NONE) {
            remoteError(VIR_ERR_AUTH_FAILED,
                        _("requested authentication type %s rejected"),
                        authtype);
            return -1;
        }
    } else {
        type = ret.types.types_val[0];
    }

    switch (type) {
#if HAVE_SASL
    case REMOTE_AUTH_SASL: {
        const char *mech = NULL;
        if (authtype &&
            STRCASEEQLEN(authtype, "sasl.", 5))
            mech = authtype + 5;

        if (remoteAuthSASL(conn, priv, in_open, auth, mech) < 0) {
            VIR_FREE(ret.types.types_val);
            return -1;
        }
        break;
    }
#endif

#if HAVE_POLKIT
    case REMOTE_AUTH_POLKIT:
        if (remoteAuthPolkit(conn, priv, in_open, auth) < 0) {
            VIR_FREE(ret.types.types_val);
            return -1;
        }
        break;
#endif

    case REMOTE_AUTH_NONE:
        /* Nothing todo, hurrah ! */
        break;

    default:
        remoteError(VIR_ERR_AUTH_FAILED,
                    _("unsupported authentication type %d"),
                    ret.types.types_val[0]);
        VIR_FREE(ret.types.types_val);
        return -1;
    }

    VIR_FREE(ret.types.types_val);

    return 0;
}



#if HAVE_SASL
static int remoteAuthCredVir2SASL(int vircred)
{
    switch (vircred) {
    case VIR_CRED_USERNAME:
        return SASL_CB_USER;

    case VIR_CRED_AUTHNAME:
        return SASL_CB_AUTHNAME;

    case VIR_CRED_LANGUAGE:
        return SASL_CB_LANGUAGE;

    case VIR_CRED_CNONCE:
        return SASL_CB_CNONCE;

    case VIR_CRED_PASSPHRASE:
        return SASL_CB_PASS;

    case VIR_CRED_ECHOPROMPT:
        return SASL_CB_ECHOPROMPT;

    case VIR_CRED_NOECHOPROMPT:
        return SASL_CB_NOECHOPROMPT;

    case VIR_CRED_REALM:
        return SASL_CB_GETREALM;
    }

    return 0;
}

static int remoteAuthCredSASL2Vir(int vircred)
{
    switch (vircred) {
    case SASL_CB_USER:
        return VIR_CRED_USERNAME;

    case SASL_CB_AUTHNAME:
        return VIR_CRED_AUTHNAME;

    case SASL_CB_LANGUAGE:
        return VIR_CRED_LANGUAGE;

    case SASL_CB_CNONCE:
        return VIR_CRED_CNONCE;

    case SASL_CB_PASS:
        return VIR_CRED_PASSPHRASE;

    case SASL_CB_ECHOPROMPT:
        return VIR_CRED_ECHOPROMPT;

    case SASL_CB_NOECHOPROMPT:
        return VIR_CRED_NOECHOPROMPT;

    case SASL_CB_GETREALM:
        return VIR_CRED_REALM;
    }

    return 0;
}

/*
 * @param credtype array of credential types client supports
 * @param ncredtype size of credtype array
 * @return the SASL callback structure, or NULL on error
 *
 * Build up the SASL callback structure. We register one callback for
 * each credential type that the libvirt client indicated they support.
 * We explicitly leav the callback function pointer at NULL though,
 * because we don't actually want to get SASL callbacks triggered.
 * Instead, we want the start/step functions to return SASL_INTERACT.
 * This lets us give the libvirt client a list of all required
 * credentials in one go, rather than triggering the callback one
 * credential at a time,
 */
static sasl_callback_t *remoteAuthMakeCallbacks(int *credtype, int ncredtype)
{
    sasl_callback_t *cbs;
    int i, n;
    if (VIR_ALLOC_N(cbs, ncredtype+1) < 0) {
        return NULL;
    }

    for (i = 0, n = 0 ; i < ncredtype ; i++) {
        int id = remoteAuthCredVir2SASL(credtype[i]);
        if (id != 0)
            cbs[n++].id = id;
        /* Don't fill proc or context fields of sasl_callback_t
         * because we want to use interactions instead */
    }
    cbs[n].id = 0;
    return cbs;
}


/*
 * @param interact SASL interactions required
 * @param cred populated with libvirt credential metadata
 * @return the size of the cred array returned
 *
 * Builds up an array of libvirt credential structs, populating
 * with data from the SASL interaction struct. These two structs
 * are basically a 1-to-1 copy of each other.
 */
static int remoteAuthMakeCredentials(sasl_interact_t *interact,
                                     virConnectCredentialPtr *cred)
{
    int ninteract;
    if (!cred)
        return -1;

    for (ninteract = 0 ; interact[ninteract].id != 0 ; ninteract++)
        ; /* empty */

    if (VIR_ALLOC_N(*cred, ninteract) < 0)
        return -1;

    for (ninteract = 0 ; interact[ninteract].id != 0 ; ninteract++) {
        (*cred)[ninteract].type = remoteAuthCredSASL2Vir(interact[ninteract].id);
        if (!(*cred)[ninteract].type) {
            VIR_FREE(*cred);
            return -1;
        }
        if (interact[ninteract].challenge)
            (*cred)[ninteract].challenge = interact[ninteract].challenge;
        (*cred)[ninteract].prompt = interact[ninteract].prompt;
        if (interact[ninteract].defresult)
            (*cred)[ninteract].defresult = interact[ninteract].defresult;
        (*cred)[ninteract].result = NULL;
    }

    return ninteract;
}

static void remoteAuthFreeCredentials(virConnectCredentialPtr cred,
                                      int ncred)
{
    int i;
    for (i = 0 ; i < ncred ; i++)
        VIR_FREE(cred[i].result);
    VIR_FREE(cred);
}


/*
 * @param cred the populated libvirt credentials
 * @param interact the SASL interactions to fill in results for
 *
 * Fills the SASL interactions with the result from the libvirt
 * callbacks
 */
static void remoteAuthFillInteract(virConnectCredentialPtr cred,
                                   sasl_interact_t *interact)
{
    int ninteract;
    for (ninteract = 0 ; interact[ninteract].id != 0 ; ninteract++) {
        interact[ninteract].result = cred[ninteract].result;
        interact[ninteract].len = cred[ninteract].resultlen;
    }
}

/* Perform the SASL authentication process
 */
static int
remoteAuthSASL (virConnectPtr conn, struct private_data *priv, int in_open,
                virConnectAuthPtr auth, const char *wantmech)
{
    sasl_conn_t *saslconn = NULL;
    sasl_security_properties_t secprops;
    remote_auth_sasl_init_ret iret;
    remote_auth_sasl_start_args sargs;
    remote_auth_sasl_start_ret sret;
    remote_auth_sasl_step_args pargs;
    remote_auth_sasl_step_ret pret;
    const char *clientout;
    char *serverin = NULL;
    unsigned int clientoutlen, serverinlen;
    const char *mech;
    int err, complete;
    virSocketAddr sa;
    char *localAddr = NULL, *remoteAddr = NULL;
    const void *val;
    sasl_ssf_t ssf;
    sasl_callback_t *saslcb = NULL;
    sasl_interact_t *interact = NULL;
    virConnectCredentialPtr cred = NULL;
    int ncred = 0;
    int ret = -1;
    const char *mechlist;

    VIR_DEBUG("Client initialize SASL authentication");
    /* Sets up the SASL library as a whole */
    err = sasl_client_init(NULL);
    if (err != SASL_OK) {
        remoteError(VIR_ERR_AUTH_FAILED,
                    _("failed to initialize SASL library: %d (%s)"),
                    err, sasl_errstring(err, NULL, NULL));
        goto cleanup;
    }

    /* Get local address in form  IPADDR:PORT */
    sa.len = sizeof(sa.data.stor);
    if (getsockname(priv->sock, &sa.data.sa, &sa.len) < 0) {
        virReportSystemError(errno, "%s",
                             _("failed to get sock address"));
        goto cleanup;
    }
    if ((localAddr = virSocketFormatAddrFull(&sa, true, ";")) == NULL)
        goto cleanup;

    /* Get remote address in form  IPADDR:PORT */
    sa.len = sizeof(sa.data.stor);
    if (getpeername(priv->sock, &sa.data.sa, &sa.len) < 0) {
        virReportSystemError(errno, "%s",
                             _("failed to get peer address"));
        goto cleanup;
    }
    if ((remoteAddr = virSocketFormatAddrFull(&sa, true, ";")) == NULL)
        goto cleanup;

    if (auth) {
        if ((saslcb = remoteAuthMakeCallbacks(auth->credtype, auth->ncredtype)) == NULL)
            goto cleanup;
    } else {
        saslcb = NULL;
    }

    /* Setup a handle for being a client */
    err = sasl_client_new("libvirt",
                          priv->hostname,
                          localAddr,
                          remoteAddr,
                          saslcb,
                          SASL_SUCCESS_DATA,
                          &saslconn);

    if (err != SASL_OK) {
        remoteError(VIR_ERR_AUTH_FAILED,
                    _("Failed to create SASL client context: %d (%s)"),
                    err, sasl_errstring(err, NULL, NULL));
        goto cleanup;
    }

    /* Initialize some connection props we care about */
    if (priv->uses_tls) {
        gnutls_cipher_algorithm_t cipher;

        cipher = gnutls_cipher_get(priv->session);
        if (!(ssf = (sasl_ssf_t)gnutls_cipher_get_key_size(cipher))) {
            remoteError(VIR_ERR_INTERNAL_ERROR, "%s",
                        _("invalid cipher size for TLS session"));
            goto cleanup;
        }
        ssf *= 8; /* key size is bytes, sasl wants bits */

        VIR_DEBUG("Setting external SSF %d", ssf);
        err = sasl_setprop(saslconn, SASL_SSF_EXTERNAL, &ssf);
        if (err != SASL_OK) {
            remoteError(VIR_ERR_INTERNAL_ERROR,
                        _("cannot set external SSF %d (%s)"),
                        err, sasl_errstring(err, NULL, NULL));
            goto cleanup;
        }
    }

    memset (&secprops, 0, sizeof secprops);
    /* If we've got a secure channel (TLS or UNIX sock), we don't care about SSF */
    secprops.min_ssf = priv->is_secure ? 0 : 56; /* Equiv to DES supported by all Kerberos */
    secprops.max_ssf = priv->is_secure ? 0 : 100000; /* Very strong ! AES == 256 */
    secprops.maxbufsize = 100000;
    /* If we're not secure, then forbid any anonymous or trivially crackable auth */
    secprops.security_flags = priv->is_secure ? 0 :
        SASL_SEC_NOANONYMOUS | SASL_SEC_NOPLAINTEXT;

    err = sasl_setprop(saslconn, SASL_SEC_PROPS, &secprops);
    if (err != SASL_OK) {
        remoteError(VIR_ERR_INTERNAL_ERROR,
                    _("cannot set security props %d (%s)"),
                    err, sasl_errstring(err, NULL, NULL));
        goto cleanup;
    }

    /* First call is to inquire about supported mechanisms in the server */
    memset (&iret, 0, sizeof iret);
    if (call (conn, priv, in_open, REMOTE_PROC_AUTH_SASL_INIT,
              (xdrproc_t) xdr_void, (char *)NULL,
              (xdrproc_t) xdr_remote_auth_sasl_init_ret, (char *) &iret) != 0)
        goto cleanup;


    mechlist = iret.mechlist;
    if (wantmech) {
        if (strstr(mechlist, wantmech) == NULL) {
            remoteError(VIR_ERR_AUTH_FAILED,
                        _("SASL mechanism %s not supported by server"),
                        wantmech);
            VIR_FREE(iret.mechlist);
            goto cleanup;
        }
        mechlist = wantmech;
    }
 restart:
    /* Start the auth negotiation on the client end first */
    VIR_DEBUG("Client start negotiation mechlist '%s'", mechlist);
    err = sasl_client_start(saslconn,
                            mechlist,
                            &interact,
                            &clientout,
                            &clientoutlen,
                            &mech);
    if (err != SASL_OK && err != SASL_CONTINUE && err != SASL_INTERACT) {
        remoteError(VIR_ERR_AUTH_FAILED,
                    _("Failed to start SASL negotiation: %d (%s)"),
                    err, sasl_errdetail(saslconn));
        VIR_FREE(iret.mechlist);
        goto cleanup;
    }

    /* Need to gather some credentials from the client */
    if (err == SASL_INTERACT) {
        const char *msg;
        if (cred) {
            remoteAuthFreeCredentials(cred, ncred);
            cred = NULL;
        }
        if ((ncred = remoteAuthMakeCredentials(interact, &cred)) < 0) {
            remoteError(VIR_ERR_AUTH_FAILED, "%s",
                        _("Failed to make auth credentials"));
            VIR_FREE(iret.mechlist);
            goto cleanup;
        }
        /* Run the authentication callback */
        if (auth && auth->cb) {
            if ((*(auth->cb))(cred, ncred, auth->cbdata) >= 0) {
                remoteAuthFillInteract(cred, interact);
                goto restart;
            }
            msg = "Failed to collect auth credentials";
        } else {
            msg = "No authentication callback available";
        }
        remoteError(VIR_ERR_AUTH_FAILED, "%s", msg);
        goto cleanup;
    }
    VIR_FREE(iret.mechlist);

    if (clientoutlen > REMOTE_AUTH_SASL_DATA_MAX) {
        remoteError(VIR_ERR_AUTH_FAILED,
                    _("SASL negotiation data too long: %d bytes"),
                    clientoutlen);
        goto cleanup;
    }
    /* NB, distinction of NULL vs "" is *critical* in SASL */
    memset(&sargs, 0, sizeof sargs);
    sargs.nil = clientout ? 0 : 1;
    sargs.data.data_val = (char*)clientout;
    sargs.data.data_len = clientoutlen;
    sargs.mech = (char*)mech;
    VIR_DEBUG("Server start negotiation with mech %s. Data %d bytes %p", mech, clientoutlen, clientout);

    /* Now send the initial auth data to the server */
    memset (&sret, 0, sizeof sret);
    if (call (conn, priv, in_open, REMOTE_PROC_AUTH_SASL_START,
              (xdrproc_t) xdr_remote_auth_sasl_start_args, (char *) &sargs,
              (xdrproc_t) xdr_remote_auth_sasl_start_ret, (char *) &sret) != 0)
        goto cleanup;

    complete = sret.complete;
    /* NB, distinction of NULL vs "" is *critical* in SASL */
    serverin = sret.nil ? NULL : sret.data.data_val;
    serverinlen = sret.data.data_len;
    VIR_DEBUG("Client step result complete: %d. Data %d bytes %p",
          complete, serverinlen, serverin);

    /* Loop-the-loop...
     * Even if the server has completed, the client must *always* do at least one step
     * in this loop to verify the server isn't lying about something. Mutual auth */
    for (;;) {
    restep:
        err = sasl_client_step(saslconn,
                               serverin,
                               serverinlen,
                               &interact,
                               &clientout,
                               &clientoutlen);
        if (err != SASL_OK && err != SASL_CONTINUE && err != SASL_INTERACT) {
            remoteError(VIR_ERR_AUTH_FAILED,
                        _("Failed SASL step: %d (%s)"),
                        err, sasl_errdetail(saslconn));
            goto cleanup;
        }
        /* Need to gather some credentials from the client */
        if (err == SASL_INTERACT) {
            const char *msg;
            if (cred) {
                remoteAuthFreeCredentials(cred, ncred);
                cred = NULL;
            }
            if ((ncred = remoteAuthMakeCredentials(interact, &cred)) < 0) {
                remoteError(VIR_ERR_AUTH_FAILED, "%s",
                            _("Failed to make auth credentials"));
                goto cleanup;
            }
            /* Run the authentication callback */
            if (auth && auth->cb) {
                if ((*(auth->cb))(cred, ncred, auth->cbdata) >= 0) {
                    remoteAuthFillInteract(cred, interact);
                    goto restep;
                }
                msg = _("Failed to collect auth credentials");
            } else {
                msg = _("No authentication callback available");
            }
            remoteError(VIR_ERR_AUTH_FAILED, "%s", msg);
            goto cleanup;
        }

        VIR_FREE(serverin);
        VIR_DEBUG("Client step result %d. Data %d bytes %p", err, clientoutlen, clientout);

        /* Previous server call showed completion & we're now locally complete too */
        if (complete && err == SASL_OK)
            break;

        /* Not done, prepare to talk with the server for another iteration */
        /* NB, distinction of NULL vs "" is *critical* in SASL */
        memset(&pargs, 0, sizeof pargs);
        pargs.nil = clientout ? 0 : 1;
        pargs.data.data_val = (char*)clientout;
        pargs.data.data_len = clientoutlen;
        VIR_DEBUG("Server step with %d bytes %p", clientoutlen, clientout);

        memset (&pret, 0, sizeof pret);
        if (call (conn, priv, in_open, REMOTE_PROC_AUTH_SASL_STEP,
                  (xdrproc_t) xdr_remote_auth_sasl_step_args, (char *) &pargs,
                  (xdrproc_t) xdr_remote_auth_sasl_step_ret, (char *) &pret) != 0)
            goto cleanup;

        complete = pret.complete;
        /* NB, distinction of NULL vs "" is *critical* in SASL */
        serverin = pret.nil ? NULL : pret.data.data_val;
        serverinlen = pret.data.data_len;

        VIR_DEBUG("Client step result complete: %d. Data %d bytes %p",
              complete, serverinlen, serverin);

        /* This server call shows complete, and earlier client step was OK */
        if (complete && err == SASL_OK) {
            VIR_FREE(serverin);
            break;
        }
    }

    /* Check for suitable SSF if not already secure (TLS or UNIX sock) */
    if (!priv->is_secure) {
        err = sasl_getprop(saslconn, SASL_SSF, &val);
        if (err != SASL_OK) {
            remoteError(VIR_ERR_AUTH_FAILED,
                        _("cannot query SASL ssf on connection %d (%s)"),
                        err, sasl_errstring(err, NULL, NULL));
            goto cleanup;
        }
        ssf = *(const int *)val;
        VIR_DEBUG("SASL SSF value %d", ssf);
        if (ssf < 56) { /* 56 == DES level, good for Kerberos */
            remoteError(VIR_ERR_AUTH_FAILED,
                        _("negotiation SSF %d was not strong enough"), ssf);
            goto cleanup;
        }
        priv->is_secure = 1;
    }

    VIR_DEBUG("SASL authentication complete");
    priv->saslconn = saslconn;
    ret = 0;

 cleanup:
    VIR_FREE(localAddr);
    VIR_FREE(remoteAddr);
    VIR_FREE(serverin);

    VIR_FREE(saslcb);
    remoteAuthFreeCredentials(cred, ncred);
    if (ret != 0 && saslconn)
        sasl_dispose(&saslconn);

    return ret;
}
#endif /* HAVE_SASL */


#if HAVE_POLKIT
# if HAVE_POLKIT1
static int
remoteAuthPolkit (virConnectPtr conn, struct private_data *priv, int in_open,
                  virConnectAuthPtr auth ATTRIBUTE_UNUSED)
{
    remote_auth_polkit_ret ret;
    VIR_DEBUG("Client initialize PolicyKit-1 authentication");

    memset (&ret, 0, sizeof ret);
    if (call (conn, priv, in_open, REMOTE_PROC_AUTH_POLKIT,
              (xdrproc_t) xdr_void, (char *)NULL,
              (xdrproc_t) xdr_remote_auth_polkit_ret, (char *) &ret) != 0) {
        return -1; /* virError already set by call */
    }

    VIR_DEBUG("PolicyKit-1 authentication complete");
    return 0;
}
# elif HAVE_POLKIT0
/* Perform the PolicyKit authentication process
 */
static int
remoteAuthPolkit (virConnectPtr conn, struct private_data *priv, int in_open,
                  virConnectAuthPtr auth)
{
    remote_auth_polkit_ret ret;
    int i, allowcb = 0;
    virConnectCredential cred = {
        VIR_CRED_EXTERNAL,
        conn->flags & VIR_CONNECT_RO ? "org.libvirt.unix.monitor" : "org.libvirt.unix.manage",
        "PolicyKit",
        NULL,
        NULL,
        0,
    };
    VIR_DEBUG("Client initialize PolicyKit-0 authentication");

    if (auth && auth->cb) {
        /* Check if the necessary credential type for PolicyKit is supported */
        for (i = 0 ; i < auth->ncredtype ; i++) {
            if (auth->credtype[i] == VIR_CRED_EXTERNAL)
                allowcb = 1;
        }

        if (allowcb) {
            VIR_DEBUG("Client run callback for PolicyKit authentication");
            /* Run the authentication callback */
            if ((*(auth->cb))(&cred, 1, auth->cbdata) < 0) {
                remoteError(VIR_ERR_AUTH_FAILED, "%s",
                            _("Failed to collect auth credentials"));
                return -1;
            }
        } else {
            VIR_DEBUG("Client auth callback does not support PolicyKit");
        }
    } else {
        VIR_DEBUG("No auth callback provided");
    }

    memset (&ret, 0, sizeof ret);
    if (call (conn, priv, in_open, REMOTE_PROC_AUTH_POLKIT,
              (xdrproc_t) xdr_void, (char *)NULL,
              (xdrproc_t) xdr_remote_auth_polkit_ret, (char *) &ret) != 0) {
        return -1; /* virError already set by call */
    }

    VIR_DEBUG("PolicyKit-0 authentication complete");
    return 0;
}
# endif /* HAVE_POLKIT0 */
#endif /* HAVE_POLKIT */
/*----------------------------------------------------------------------*/

static int remoteDomainEventRegister(virConnectPtr conn,
                                     virConnectDomainEventCallback callback,
                                     void *opaque,
                                     virFreeCallback freecb)
{
    int rv = -1;
    struct private_data *priv = conn->privateData;

    remoteDriverLock(priv);

    if (priv->domainEventState->timer < 0) {
         remoteError(VIR_ERR_NO_SUPPORT, "%s", _("no event support"));
         goto done;
    }

    if (virDomainEventCallbackListAdd(conn, priv->domainEventState->callbacks,
                                      callback, opaque, freecb) < 0) {
         remoteError(VIR_ERR_RPC, "%s", _("adding cb to list"));
         goto done;
    }

    if (virDomainEventCallbackListCountID(conn,
                                          priv->domainEventState->callbacks,
                                          VIR_DOMAIN_EVENT_ID_LIFECYCLE) == 1) {
        /* Tell the server when we are the first callback deregistering */
        if (call (conn, priv, 0, REMOTE_PROC_DOMAIN_EVENTS_REGISTER,
                (xdrproc_t) xdr_void, (char *) NULL,
                (xdrproc_t) xdr_void, (char *) NULL) == -1)
            goto done;
    }

    rv = 0;

done:
    remoteDriverUnlock(priv);
    return rv;
}

static int remoteDomainEventDeregister(virConnectPtr conn,
                                       virConnectDomainEventCallback callback)
{
    struct private_data *priv = conn->privateData;
    int rv = -1;

    remoteDriverLock(priv);

    if (virDomainEventStateDeregister(conn,
                                      priv->domainEventState,
                                      callback) < 0)
        goto done;

    if (virDomainEventCallbackListCountID(conn,
                                          priv->domainEventState->callbacks,
                                          VIR_DOMAIN_EVENT_ID_LIFECYCLE) == 0) {
        /* Tell the server when we are the last callback deregistering */
        if (call (conn, priv, 0, REMOTE_PROC_DOMAIN_EVENTS_DEREGISTER,
                  (xdrproc_t) xdr_void, (char *) NULL,
                  (xdrproc_t) xdr_void, (char *) NULL) == -1)
            goto done;
    }

    rv = 0;

done:
    remoteDriverUnlock(priv);
    return rv;
}

/**
 * remoteDomainReadEventLifecycle
 *
 * Read the domain lifecycle event data off the wire
 */
static virDomainEventPtr
remoteDomainReadEventLifecycle(virConnectPtr conn, XDR *xdr)
{
    remote_domain_event_lifecycle_msg msg;
    virDomainPtr dom;
    virDomainEventPtr event = NULL;
    memset (&msg, 0, sizeof msg);

    /* unmarshal parameters, and process it*/
    if (! xdr_remote_domain_event_lifecycle_msg(xdr, &msg) ) {
        remoteError(VIR_ERR_RPC, "%s",
                    _("Unable to demarshal lifecycle event"));
        return NULL;
    }

    dom = get_nonnull_domain(conn,msg.dom);
    if (!dom)
        return NULL;

    event = virDomainEventNewFromDom(dom, msg.event, msg.detail);
    xdr_free ((xdrproc_t) &xdr_remote_domain_event_lifecycle_msg, (char *) &msg);

    virDomainFree(dom);
    return event;
}


static virDomainEventPtr
remoteDomainReadEventReboot(virConnectPtr conn, XDR *xdr)
{
    remote_domain_event_reboot_msg msg;
    virDomainPtr dom;
    virDomainEventPtr event = NULL;
    memset (&msg, 0, sizeof msg);

    /* unmarshal parameters, and process it*/
    if (! xdr_remote_domain_event_reboot_msg(xdr, &msg) ) {
        remoteError(VIR_ERR_RPC, "%s",
                    _("Unable to demarshal reboot event"));
        return NULL;
    }

    dom = get_nonnull_domain(conn,msg.dom);
    if (!dom)
        return NULL;

    event = virDomainEventRebootNewFromDom(dom);
    xdr_free ((xdrproc_t) &xdr_remote_domain_event_reboot_msg, (char *) &msg);

    virDomainFree(dom);
    return event;
}


static virDomainEventPtr
remoteDomainReadEventRTCChange(virConnectPtr conn, XDR *xdr)
{
    remote_domain_event_rtc_change_msg msg;
    virDomainPtr dom;
    virDomainEventPtr event = NULL;
    memset (&msg, 0, sizeof msg);

    /* unmarshal parameters, and process it*/
    if (! xdr_remote_domain_event_rtc_change_msg(xdr, &msg) ) {
        remoteError(VIR_ERR_RPC, "%s",
                    _("Unable to demarshal RTC change event"));
        return NULL;
    }

    dom = get_nonnull_domain(conn,msg.dom);
    if (!dom)
        return NULL;

    event = virDomainEventRTCChangeNewFromDom(dom, msg.offset);
    xdr_free ((xdrproc_t) &xdr_remote_domain_event_rtc_change_msg, (char *) &msg);

    virDomainFree(dom);
    return event;
}


static virDomainEventPtr
remoteDomainReadEventWatchdog(virConnectPtr conn, XDR *xdr)
{
    remote_domain_event_watchdog_msg msg;
    virDomainPtr dom;
    virDomainEventPtr event = NULL;
    memset (&msg, 0, sizeof msg);

    /* unmarshal parameters, and process it*/
    if (! xdr_remote_domain_event_watchdog_msg(xdr, &msg) ) {
        remoteError(VIR_ERR_RPC, "%s",
                    _("Unable to demarshal watchdog event"));
        return NULL;
    }

    dom = get_nonnull_domain(conn,msg.dom);
    if (!dom)
        return NULL;

    event = virDomainEventWatchdogNewFromDom(dom, msg.action);
    xdr_free ((xdrproc_t) &xdr_remote_domain_event_watchdog_msg, (char *) &msg);

    virDomainFree(dom);
    return event;
}


static virDomainEventPtr
remoteDomainReadEventIOError(virConnectPtr conn, XDR *xdr)
{
    remote_domain_event_io_error_msg msg;
    virDomainPtr dom;
    virDomainEventPtr event = NULL;
    memset (&msg, 0, sizeof msg);

    /* unmarshal parameters, and process it*/
    if (! xdr_remote_domain_event_io_error_msg(xdr, &msg) ) {
        remoteError(VIR_ERR_RPC, "%s",
                    _("Unable to demarshal IO error event"));
        return NULL;
    }

    dom = get_nonnull_domain(conn,msg.dom);
    if (!dom)
        return NULL;

    event = virDomainEventIOErrorNewFromDom(dom,
                                            msg.srcPath,
                                            msg.devAlias,
                                            msg.action);
    xdr_free ((xdrproc_t) &xdr_remote_domain_event_io_error_msg, (char *) &msg);

    virDomainFree(dom);
    return event;
}


static virDomainEventPtr
remoteDomainReadEventIOErrorReason(virConnectPtr conn, XDR *xdr)
{
    remote_domain_event_io_error_reason_msg msg;
    virDomainPtr dom;
    virDomainEventPtr event = NULL;
    memset (&msg, 0, sizeof msg);

    /* unmarshal parameters, and process it*/
    if (! xdr_remote_domain_event_io_error_reason_msg(xdr, &msg) ) {
        remoteError(VIR_ERR_RPC, "%s",
                    _("Unable to demarshal IO error reason event"));
        return NULL;
    }

    dom = get_nonnull_domain(conn,msg.dom);
    if (!dom)
        return NULL;

    event = virDomainEventIOErrorReasonNewFromDom(dom,
                                                  msg.srcPath,
                                                  msg.devAlias,
                                                  msg.action,
                                                  msg.reason);
    xdr_free ((xdrproc_t) &xdr_remote_domain_event_io_error_reason_msg, (char *) &msg);

    virDomainFree(dom);
    return event;
}


static virDomainEventPtr
remoteDomainReadEventGraphics(virConnectPtr conn, XDR *xdr)
{
    remote_domain_event_graphics_msg msg;
    virDomainPtr dom;
    virDomainEventPtr event = NULL;
    virDomainEventGraphicsAddressPtr localAddr = NULL;
    virDomainEventGraphicsAddressPtr remoteAddr = NULL;
    virDomainEventGraphicsSubjectPtr subject = NULL;
    int i;

    memset (&msg, 0, sizeof msg);

    /* unmarshal parameters, and process it*/
    if (! xdr_remote_domain_event_graphics_msg(xdr, &msg) ) {
        remoteError(VIR_ERR_RPC, "%s",
                    _("Unable to demarshal graphics event"));
        return NULL;
    }

    dom = get_nonnull_domain(conn,msg.dom);
    if (!dom)
        return NULL;

    if (VIR_ALLOC(localAddr) < 0)
        goto no_memory;
    localAddr->family = msg.local.family;
    if (!(localAddr->service = strdup(msg.local.service)) ||
        !(localAddr->node = strdup(msg.local.node)))
        goto no_memory;

    if (VIR_ALLOC(remoteAddr) < 0)
        goto no_memory;
    remoteAddr->family = msg.remote.family;
    if (!(remoteAddr->service = strdup(msg.remote.service)) ||
        !(remoteAddr->node = strdup(msg.remote.node)))
        goto no_memory;

    if (VIR_ALLOC(subject) < 0)
        goto no_memory;
    if (VIR_ALLOC_N(subject->identities, msg.subject.subject_len) < 0)
        goto no_memory;
    subject->nidentity = msg.subject.subject_len;
    for (i = 0 ; i < subject->nidentity ; i++) {
        if (!(subject->identities[i].type = strdup(msg.subject.subject_val[i].type)) ||
            !(subject->identities[i].name = strdup(msg.subject.subject_val[i].name)))
            goto no_memory;
    }

    event = virDomainEventGraphicsNewFromDom(dom,
                                             msg.phase,
                                             localAddr,
                                             remoteAddr,
                                             msg.authScheme,
                                             subject);
    xdr_free ((xdrproc_t) &xdr_remote_domain_event_graphics_msg, (char *) &msg);

    virDomainFree(dom);
    return event;

no_memory:
    xdr_free ((xdrproc_t) &xdr_remote_domain_event_graphics_msg, (char *) &msg);

    if (localAddr) {
        VIR_FREE(localAddr->service);
        VIR_FREE(localAddr->node);
        VIR_FREE(localAddr);
    }
    if (remoteAddr) {
        VIR_FREE(remoteAddr->service);
        VIR_FREE(remoteAddr->node);
        VIR_FREE(remoteAddr);
    }
    if (subject) {
        for (i = 0 ; i < subject->nidentity ; i++) {
            VIR_FREE(subject->identities[i].type);
            VIR_FREE(subject->identities[i].name);
        }
        VIR_FREE(subject->identities);
        VIR_FREE(subject);
    }
    return NULL;
}


static virDrvOpenStatus ATTRIBUTE_NONNULL (1)
remoteSecretOpen(virConnectPtr conn, virConnectAuthPtr auth, int flags)
{
    return remoteGenericOpen(conn, auth, flags, &conn->secretPrivateData);
}

static int
remoteSecretClose (virConnectPtr conn)
{
    return remoteGenericClose(conn, &conn->secretPrivateData);
}

static unsigned char *
remoteSecretGetValue (virSecretPtr secret, size_t *value_size,
                      unsigned int flags)
{
    unsigned char *rv = NULL;
    remote_secret_get_value_args args;
    remote_secret_get_value_ret ret;
    struct private_data *priv = secret->conn->secretPrivateData;

    remoteDriverLock (priv);

    make_nonnull_secret (&args.secret, secret);
    args.flags = flags;

    memset (&ret, 0, sizeof (ret));
    if (call (secret->conn, priv, 0, REMOTE_PROC_SECRET_GET_VALUE,
              (xdrproc_t) xdr_remote_secret_get_value_args, (char *) &args,
              (xdrproc_t) xdr_remote_secret_get_value_ret, (char *) &ret) == -1)
        goto done;

    *value_size = ret.value.value_len;
    rv = (unsigned char *) ret.value.value_val; /* Caller frees. */

done:
    remoteDriverUnlock (priv);
    return rv;
}

static struct private_stream_data *
remoteStreamOpen(virStreamPtr st,
                 unsigned int proc_nr,
                 unsigned int serial)
{
    struct private_data *priv = st->conn->privateData;
    struct private_stream_data *stpriv;

    if (VIR_ALLOC(stpriv) < 0) {
        virReportOOMError();
        return NULL;
    }

    /* Initialize call object used to receive replies */
    stpriv->proc_nr = proc_nr;
    stpriv->serial = serial;

    stpriv->next = priv->streams;
    priv->streams = stpriv;

    return stpriv;
}


static void
remoteStreamEventTimerUpdate(struct private_stream_data *privst)
{
    if (!privst->cb)
        return;

    VIR_DEBUG("Check timer offset=%d %d", privst->incomingOffset, privst->cbEvents);
    if ((privst->incomingOffset &&
         (privst->cbEvents & VIR_STREAM_EVENT_READABLE)) ||
        (privst->cbEvents & VIR_STREAM_EVENT_WRITABLE)) {
        VIR_DEBUG("Enabling event timer");
        virEventUpdateTimeout(privst->cbTimer, 0);
    } else {
        VIR_DEBUG("Disabling event timer");
        virEventUpdateTimeout(privst->cbTimer, -1);
    }
}


static int
remoteStreamPacket(virStreamPtr st,
                   int status,
                   const char *data,
                   size_t nbytes)
{
    VIR_DEBUG("st=%p status=%d data=%p nbytes=%zu", st, status, data, nbytes);
    struct private_data *priv = st->conn->privateData;
    struct private_stream_data *privst = st->privateData;
    XDR xdr;
    struct remote_thread_call *thiscall;
    remote_message_header hdr;
    int ret;

    memset(&hdr, 0, sizeof hdr);

    if (VIR_ALLOC(thiscall) < 0) {
        virReportOOMError();
        return -1;
    }

    thiscall->mode = REMOTE_MODE_WAIT_TX;
    thiscall->serial = privst->serial;
    thiscall->proc_nr = privst->proc_nr;
    if (status == REMOTE_OK ||
        status == REMOTE_ERROR)
        thiscall->want_reply = 1;

    if (virCondInit(&thiscall->cond) < 0) {
        VIR_FREE(thiscall);
        remoteError(VIR_ERR_INTERNAL_ERROR, "%s",
                    _("cannot initialize mutex"));
        return -1;
    }

    /* Don't fill in any other fields in 'thiscall' since
     * we're not expecting a reply for this */

    hdr.prog = REMOTE_PROGRAM;
    hdr.vers = REMOTE_PROTOCOL_VERSION;
    hdr.proc = privst->proc_nr;
    hdr.type = REMOTE_STREAM;
    hdr.serial = privst->serial;
    hdr.status = status;


    /* Length must include the length word itself (always encoded in
     * 4 bytes as per RFC 4506), so offset start length. We write this
     * later.
     */
    thiscall->bufferLength = REMOTE_MESSAGE_HEADER_XDR_LEN;

    /* Serialise header followed by args. */
    xdrmem_create (&xdr, thiscall->buffer + thiscall->bufferLength,
                   REMOTE_MESSAGE_MAX, XDR_ENCODE);
    if (!xdr_remote_message_header (&xdr, &hdr)) {
        remoteError(VIR_ERR_RPC, "%s", _("xdr_remote_message_header failed"));
        goto error;
    }

    thiscall->bufferLength += xdr_getpos (&xdr);
    xdr_destroy (&xdr);

    if (status == REMOTE_CONTINUE) {
        if (((4 + REMOTE_MESSAGE_MAX) - thiscall->bufferLength) < nbytes) {
            remoteError(VIR_ERR_RPC, _("data size %zu too large for payload %d"),
                        nbytes, ((4 + REMOTE_MESSAGE_MAX) - thiscall->bufferLength));
            goto error;
        }

        memcpy(thiscall->buffer + thiscall->bufferLength, data, nbytes);
        thiscall->bufferLength += nbytes;
    }

    /* Go back to packet start and encode the length word. */
    xdrmem_create (&xdr, thiscall->buffer, REMOTE_MESSAGE_HEADER_XDR_LEN, XDR_ENCODE);
    if (!xdr_u_int (&xdr, &thiscall->bufferLength)) {
        remoteError(VIR_ERR_RPC, "%s", _("xdr_u_int (length word)"));
        goto error;
    }
    xdr_destroy (&xdr);

    ret = remoteIO(st->conn, priv, 0, thiscall);
    ignore_value(virCondDestroy(&thiscall->cond));
    VIR_FREE(thiscall);
    if (ret < 0)
        return -1;

    return nbytes;

error:
    xdr_destroy (&xdr);
    ignore_value(virCondDestroy(&thiscall->cond));
    VIR_FREE(thiscall);
    return -1;
}

static int
remoteStreamHasError(virStreamPtr st) {
    struct private_stream_data *privst = st->privateData;
    if (!privst->has_error) {
        return 0;
    }

    VIR_DEBUG("Raising async error");
    virRaiseErrorFull(__FILE__, __FUNCTION__, __LINE__,
                      privst->err.domain,
                      privst->err.code,
                      privst->err.level,
                      privst->err.str1 ? *privst->err.str1 : NULL,
                      privst->err.str2 ? *privst->err.str2 : NULL,
                      privst->err.str3 ? *privst->err.str3 : NULL,
                      privst->err.int1,
                      privst->err.int2,
                      "%s", privst->err.message ? *privst->err.message : NULL);

    return 1;
}

static void
remoteStreamRelease(virStreamPtr st)
{
    struct private_data *priv = st->conn->privateData;
    struct private_stream_data *privst = st->privateData;

    if (priv->streams == privst)
        priv->streams = privst->next;
    else {
        struct private_stream_data *tmp = priv->streams;
        while (tmp && tmp->next) {
            if (tmp->next == privst) {
                tmp->next = privst->next;
                break;
            }
        }
    }

    if (privst->has_error)
        xdr_free((xdrproc_t)xdr_remote_error,  (char *)&privst->err);

    VIR_FREE(privst);

    st->driver = NULL;
    st->privateData = NULL;
}


static int
remoteStreamSend(virStreamPtr st,
                 const char *data,
                 size_t nbytes)
{
    VIR_DEBUG("st=%p data=%p nbytes=%zu", st, data, nbytes);
    struct private_data *priv = st->conn->privateData;
    int rv = -1;

    remoteDriverLock(priv);

    if (remoteStreamHasError(st))
        goto cleanup;

    rv = remoteStreamPacket(st,
                            REMOTE_CONTINUE,
                            data,
                            nbytes);

cleanup:
    if (rv == -1)
        remoteStreamRelease(st);

    remoteDriverUnlock(priv);

    return rv;
}


static int
remoteStreamRecv(virStreamPtr st,
                 char *data,
                 size_t nbytes)
{
    VIR_DEBUG("st=%p data=%p nbytes=%zu", st, data, nbytes);
    struct private_data *priv = st->conn->privateData;
    struct private_stream_data *privst = st->privateData;
    int rv = -1;

    remoteDriverLock(priv);

    if (remoteStreamHasError(st))
        goto cleanup;

    if (!privst->incomingOffset) {
        struct remote_thread_call *thiscall;
        int ret;

        if (st->flags & VIR_STREAM_NONBLOCK) {
            VIR_DEBUG("Non-blocking mode and no data available");
            rv = -2;
            goto cleanup;
        }

        if (VIR_ALLOC(thiscall) < 0) {
            virReportOOMError();
            goto cleanup;
        }

        /* We're not really doing an RPC calls, so we're
         * skipping straight to RX part */
        thiscall->mode = REMOTE_MODE_WAIT_RX;
        thiscall->serial = privst->serial;
        thiscall->proc_nr = privst->proc_nr;
        thiscall->want_reply = 1;

        if (virCondInit(&thiscall->cond) < 0) {
            VIR_FREE(thiscall);
            remoteError(VIR_ERR_INTERNAL_ERROR, "%s",
                        _("cannot initialize mutex"));
            goto cleanup;
        }

        ret = remoteIO(st->conn, priv, 0, thiscall);
        ignore_value(virCondDestroy(&thiscall->cond));
        VIR_FREE(thiscall);
        if (ret < 0)
            goto cleanup;
    }

    VIR_DEBUG("After IO %d", privst->incomingOffset);
    if (privst->incomingOffset) {
        int want = privst->incomingOffset;
        if (want > nbytes)
            want = nbytes;
        memcpy(data, privst->incoming, want);
        if (want < privst->incomingOffset) {
            memmove(privst->incoming, privst->incoming + want, privst->incomingOffset - want);
            privst->incomingOffset -= want;
        } else {
            VIR_FREE(privst->incoming);
            privst->incomingOffset = privst->incomingLength = 0;
        }
        rv = want;
    } else {
        rv = 0;
    }

    remoteStreamEventTimerUpdate(privst);

    VIR_DEBUG("Done %d", rv);

cleanup:
    if (rv == -1)
        remoteStreamRelease(st);
    remoteDriverUnlock(priv);

    return rv;
}


static void
remoteStreamEventTimer(int timer ATTRIBUTE_UNUSED, void *opaque)
{
    virStreamPtr st = opaque;
    struct private_data *priv = st->conn->privateData;
    struct private_stream_data *privst = st->privateData;
    int events = 0;

    remoteDriverLock(priv);

    if (privst->cb &&
        (privst->cbEvents & VIR_STREAM_EVENT_READABLE) &&
        privst->incomingOffset)
        events |= VIR_STREAM_EVENT_READABLE;
    if (privst->cb &&
        (privst->cbEvents & VIR_STREAM_EVENT_WRITABLE))
        events |= VIR_STREAM_EVENT_WRITABLE;
    VIR_DEBUG("Got Timer dispatch %d %d offset=%d", events, privst->cbEvents, privst->incomingOffset);
    if (events) {
        virStreamEventCallback cb = privst->cb;
        void *cbOpaque = privst->cbOpaque;
        virFreeCallback cbFree = privst->cbFree;

        privst->cbDispatch = 1;
        remoteDriverUnlock(priv);
        (cb)(st, events, cbOpaque);
        remoteDriverLock(priv);
        privst->cbDispatch = 0;

        if (!privst->cb && cbFree)
            (cbFree)(cbOpaque);
    }

    remoteDriverUnlock(priv);
}


static void
remoteStreamEventTimerFree(void *opaque)
{
    virStreamPtr st = opaque;
    virUnrefStream(st);
}


static int
remoteStreamEventAddCallback(virStreamPtr st,
                             int events,
                             virStreamEventCallback cb,
                             void *opaque,
                             virFreeCallback ff)
{
    struct private_data *priv = st->conn->privateData;
    struct private_stream_data *privst = st->privateData;
    int ret = -1;

    remoteDriverLock(priv);

    if (privst->cb) {
        remoteError(VIR_ERR_INTERNAL_ERROR,
                    "%s", _("multiple stream callbacks not supported"));
        goto cleanup;
    }

    virStreamRef(st);
    if ((privst->cbTimer =
         virEventAddTimeout(-1,
                            remoteStreamEventTimer,
                            st,
                            remoteStreamEventTimerFree)) < 0) {
        virUnrefStream(st);
        goto cleanup;
    }

    privst->cb = cb;
    privst->cbOpaque = opaque;
    privst->cbFree = ff;
    privst->cbEvents = events;

    remoteStreamEventTimerUpdate(privst);

    ret = 0;

cleanup:
    remoteDriverUnlock(priv);
    return ret;
}

static int
remoteStreamEventUpdateCallback(virStreamPtr st,
                                int events)
{
    struct private_data *priv = st->conn->privateData;
    struct private_stream_data *privst = st->privateData;
    int ret = -1;

    remoteDriverLock(priv);

    if (!privst->cb) {
        remoteError(VIR_ERR_INTERNAL_ERROR,
                    "%s", _("no stream callback registered"));
        goto cleanup;
    }

    privst->cbEvents = events;

    remoteStreamEventTimerUpdate(privst);

    ret = 0;

cleanup:
    remoteDriverUnlock(priv);
    return ret;
}


static int
remoteStreamEventRemoveCallback(virStreamPtr st)
{
    struct private_data *priv = st->conn->privateData;
    struct private_stream_data *privst = st->privateData;
    int ret = -1;

    remoteDriverLock(priv);

    if (!privst->cb) {
        remoteError(VIR_ERR_INTERNAL_ERROR,
                    "%s", _("no stream callback registered"));
        goto cleanup;
    }

    if (!privst->cbDispatch &&
        privst->cbFree)
        (privst->cbFree)(privst->cbOpaque);
    privst->cb = NULL;
    privst->cbOpaque = NULL;
    privst->cbFree = NULL;
    privst->cbEvents = 0;
    virEventRemoveTimeout(privst->cbTimer);

    ret = 0;

cleanup:
    remoteDriverUnlock(priv);
    return ret;
}

static int
remoteStreamFinish(virStreamPtr st)
{
    struct private_data *priv = st->conn->privateData;
    int ret = -1;

    remoteDriverLock(priv);

    if (remoteStreamHasError(st))
        goto cleanup;

    ret = remoteStreamPacket(st,
                             REMOTE_OK,
                             NULL,
                             0);

cleanup:
    remoteStreamRelease(st);

    remoteDriverUnlock(priv);
    return ret;
}

static int
remoteStreamAbort(virStreamPtr st)
{
    struct private_data *priv = st->conn->privateData;
    int ret = -1;

    remoteDriverLock(priv);

    if (remoteStreamHasError(st))
        goto cleanup;

    ret = remoteStreamPacket(st,
                             REMOTE_ERROR,
                             NULL,
                             0);

cleanup:
    remoteStreamRelease(st);

    remoteDriverUnlock(priv);
    return ret;
}



static virStreamDriver remoteStreamDrv = {
    .streamRecv = remoteStreamRecv,
    .streamSend = remoteStreamSend,
    .streamFinish = remoteStreamFinish,
    .streamAbort = remoteStreamAbort,
    .streamAddCallback = remoteStreamEventAddCallback,
    .streamUpdateCallback = remoteStreamEventUpdateCallback,
    .streamRemoveCallback = remoteStreamEventRemoveCallback,
};

static int remoteDomainEventRegisterAny(virConnectPtr conn,
                                        virDomainPtr dom,
                                        int eventID,
                                        virConnectDomainEventGenericCallback callback,
                                        void *opaque,
                                        virFreeCallback freecb)
{
    int rv = -1;
    struct private_data *priv = conn->privateData;
    remote_domain_events_register_any_args args;
    int callbackID;

    remoteDriverLock(priv);

    if (priv->domainEventState->timer < 0) {
         remoteError(VIR_ERR_NO_SUPPORT, "%s", _("no event support"));
         goto done;
    }

    if ((callbackID = virDomainEventCallbackListAddID(conn,
                                                      priv->domainEventState->callbacks,
                                                      dom, eventID,
                                                      callback, opaque, freecb)) < 0) {
         remoteError(VIR_ERR_RPC, "%s", _("adding cb to list"));
         goto done;
    }

    /* If this is the first callback for this eventID, we need to enable
     * events on the server */
    if (virDomainEventCallbackListCountID(conn,
                                          priv->domainEventState->callbacks,
                                          eventID) == 1) {
        args.eventID = eventID;

        if (call (conn, priv, 0, REMOTE_PROC_DOMAIN_EVENTS_REGISTER_ANY,
                  (xdrproc_t) xdr_remote_domain_events_register_any_args, (char *) &args,
                  (xdrproc_t) xdr_void, (char *)NULL) == -1) {
            virDomainEventCallbackListRemoveID(conn,
                                               priv->domainEventState->callbacks,
                                               callbackID);
            goto done;
        }
    }

    rv = callbackID;

done:
    remoteDriverUnlock(priv);
    return rv;
}


static int remoteDomainEventDeregisterAny(virConnectPtr conn,
                                          int callbackID)
{
    struct private_data *priv = conn->privateData;
    int rv = -1;
    remote_domain_events_deregister_any_args args;
    int eventID;

    remoteDriverLock(priv);

    if ((eventID = virDomainEventCallbackListEventID(conn,
                                                     priv->domainEventState->callbacks,
                                                     callbackID)) < 0) {
        remoteError(VIR_ERR_RPC, _("unable to find callback ID %d"), callbackID);
        goto done;
    }

    if (virDomainEventStateDeregisterAny(conn,
                                         priv->domainEventState,
                                         callbackID) < 0)
        goto done;

    /* If that was the last callback for this eventID, we need to disable
     * events on the server */
    if (virDomainEventCallbackListCountID(conn,
                                          priv->domainEventState->callbacks,
                                          eventID) == 0) {
        args.eventID = eventID;

        if (call (conn, priv, 0, REMOTE_PROC_DOMAIN_EVENTS_DEREGISTER_ANY,
                  (xdrproc_t) xdr_remote_domain_events_deregister_any_args, (char *) &args,
                  (xdrproc_t) xdr_void, (char *) NULL) == -1)
            goto done;
    }

    rv = 0;

done:
    remoteDriverUnlock(priv);
    return rv;
}

/*----------------------------------------------------------------------*/

static int
remoteQemuDomainMonitorCommand (virDomainPtr domain, const char *cmd,
                                char **result, unsigned int flags)
{
    int rv = -1;
    qemu_monitor_command_args args;
    qemu_monitor_command_ret ret;
    struct private_data *priv = domain->conn->privateData;

    remoteDriverLock(priv);

    make_nonnull_domain(&args.dom, domain);
    args.cmd = (char *)cmd;
    args.flags = flags;

    memset (&ret, 0, sizeof ret);
    if (call (domain->conn, priv, REMOTE_CALL_QEMU, QEMU_PROC_MONITOR_COMMAND,
              (xdrproc_t) xdr_qemu_monitor_command_args, (char *) &args,
              (xdrproc_t) xdr_qemu_monitor_command_ret, (char *) &ret) == -1)
        goto done;

    *result = strdup(ret.result);
    if (*result == NULL) {
        virReportOOMError();
        goto cleanup;
    }

    rv = 0;

cleanup:
    xdr_free ((xdrproc_t) xdr_qemu_monitor_command_ret, (char *) &ret);

done:
    remoteDriverUnlock(priv);
    return rv;
}


static char *
remoteDomainMigrateBegin3(virDomainPtr domain,
                          const char *xmlin,
                          char **cookieout,
                          int *cookieoutlen,
                          unsigned long flags,
                          const char *dname,
                          unsigned long resource)
{
    char *rv = NULL;
    remote_domain_migrate_begin3_args args;
    remote_domain_migrate_begin3_ret ret;
    struct private_data *priv = domain->conn->privateData;

    remoteDriverLock(priv);

    memset(&args, 0, sizeof(args));
    memset(&ret, 0, sizeof(ret));

    make_nonnull_domain (&args.dom, domain);
    args.xmlin = xmlin == NULL ? NULL : (char **) &xmlin;
    args.flags = flags;
    args.dname = dname == NULL ? NULL : (char **) &dname;
    args.resource = resource;

    if (call (domain->conn, priv, 0, REMOTE_PROC_DOMAIN_MIGRATE_BEGIN3,
              (xdrproc_t) xdr_remote_domain_migrate_begin3_args, (char *) &args,
              (xdrproc_t) xdr_remote_domain_migrate_begin3_ret, (char *) &ret) == -1)
        goto done;

    if (ret.cookie_out.cookie_out_len > 0) {
        if (!cookieout || !cookieoutlen) {
            remoteError(VIR_ERR_INTERNAL_ERROR, "%s",
                        _("caller ignores cookieout or cookieoutlen"));
            goto error;
        }
        *cookieout = ret.cookie_out.cookie_out_val; /* Caller frees. */
        *cookieoutlen = ret.cookie_out.cookie_out_len;
    }

    rv = ret.xml; /* caller frees */

done:
    remoteDriverUnlock(priv);
    return rv;

error:
    VIR_FREE(ret.cookie_out.cookie_out_val);
    goto done;
}


static int
remoteDomainMigratePrepare3(virConnectPtr dconn,
                            const char *cookiein,
                            int cookieinlen,
                            char **cookieout,
                            int *cookieoutlen,
                            const char *uri_in,
                            char **uri_out,
                            unsigned long flags,
                            const char *dname,
                            unsigned long resource,
                            const char *dom_xml)
{
    int rv = -1;
    remote_domain_migrate_prepare3_args args;
    remote_domain_migrate_prepare3_ret ret;
    struct private_data *priv = dconn->privateData;

    remoteDriverLock(priv);

    memset(&args, 0, sizeof(args));
    memset(&ret, 0, sizeof(ret));

    args.cookie_in.cookie_in_val = (char *)cookiein;
    args.cookie_in.cookie_in_len = cookieinlen;
    args.uri_in = uri_in == NULL ? NULL : (char **) &uri_in;
    args.flags = flags;
    args.dname = dname == NULL ? NULL : (char **) &dname;
    args.resource = resource;
    args.dom_xml = (char *) dom_xml;

    memset (&ret, 0, sizeof ret);
    if (call (dconn, priv, 0, REMOTE_PROC_DOMAIN_MIGRATE_PREPARE3,
              (xdrproc_t) xdr_remote_domain_migrate_prepare3_args, (char *) &args,
              (xdrproc_t) xdr_remote_domain_migrate_prepare3_ret, (char *) &ret) == -1)
        goto done;

    if (ret.cookie_out.cookie_out_len > 0) {
        if (!cookieout || !cookieoutlen) {
            remoteError(VIR_ERR_INTERNAL_ERROR, "%s",
                        _("caller ignores cookieout or cookieoutlen"));
            goto error;
        }
        *cookieout = ret.cookie_out.cookie_out_val; /* Caller frees. */
        *cookieoutlen = ret.cookie_out.cookie_out_len;
    }
    if (ret.uri_out) {
        if (!uri_out) {
            remoteError(VIR_ERR_INTERNAL_ERROR, "%s",
                        _("caller ignores uri_out"));
            goto error;
        }
        *uri_out = *ret.uri_out; /* Caller frees. */
    }

    rv = 0;

done:
    remoteDriverUnlock(priv);
    return rv;
error:
    VIR_FREE(ret.cookie_out.cookie_out_val);
    if (ret.uri_out)
        VIR_FREE(*ret.uri_out);
    goto done;
}


static int
remoteDomainMigratePrepareTunnel3(virConnectPtr dconn,
                                  virStreamPtr st,
                                  const char *cookiein,
                                  int cookieinlen,
                                  char **cookieout,
                                  int *cookieoutlen,
                                  unsigned long flags,
                                  const char *dname,
                                  unsigned long resource,
                                  const char *dom_xml)
{
    struct private_data *priv = dconn->privateData;
    struct private_stream_data *privst = NULL;
    int rv = -1;
    remote_domain_migrate_prepare_tunnel3_args args;
    remote_domain_migrate_prepare_tunnel3_ret ret;

    remoteDriverLock(priv);

    memset(&args, 0, sizeof(args));
    memset(&ret, 0, sizeof(ret));

    if (!(privst = remoteStreamOpen(st,
                                    REMOTE_PROC_DOMAIN_MIGRATE_PREPARE_TUNNEL3,
                                    priv->counter)))
        goto done;

    st->driver = &remoteStreamDrv;
    st->privateData = privst;

    args.cookie_in.cookie_in_val = (char *)cookiein;
    args.cookie_in.cookie_in_len = cookieinlen;
    args.flags = flags;
    args.dname = dname == NULL ? NULL : (char **) &dname;
    args.resource = resource;
    args.dom_xml = (char *) dom_xml;

    if (call(dconn, priv, 0, REMOTE_PROC_DOMAIN_MIGRATE_PREPARE_TUNNEL3,
             (xdrproc_t) xdr_remote_domain_migrate_prepare_tunnel3_args, (char *) &args,
             (xdrproc_t) xdr_remote_domain_migrate_prepare_tunnel3_ret, (char *) &ret) == -1) {
        remoteStreamRelease(st);
        goto done;
    }

    if (ret.cookie_out.cookie_out_len > 0) {
        if (!cookieout || !cookieoutlen) {
            remoteError(VIR_ERR_INTERNAL_ERROR, "%s",
                        _("caller ignores cookieout or cookieoutlen"));
            goto error;
        }
        *cookieout = ret.cookie_out.cookie_out_val; /* Caller frees. */
        *cookieoutlen = ret.cookie_out.cookie_out_len;
    }

    rv = 0;

done:
    remoteDriverUnlock(priv);
    return rv;

error:
    VIR_FREE(ret.cookie_out.cookie_out_val);
    goto done;
}


static int
remoteDomainMigratePerform3(virDomainPtr dom,
                            const char *xmlin,
                            const char *cookiein,
                            int cookieinlen,
                            char **cookieout,
                            int *cookieoutlen,
                            const char *dconnuri,
                            const char *uri,
                            unsigned long flags,
                            const char *dname,
                            unsigned long resource)
{
    int rv = -1;
    remote_domain_migrate_perform3_args args;
    remote_domain_migrate_perform3_ret ret;
    struct private_data *priv = dom->conn->privateData;

    remoteDriverLock(priv);

    memset(&args, 0, sizeof(args));
    memset(&ret, 0, sizeof(ret));

    make_nonnull_domain(&args.dom, dom);

    args.xmlin = xmlin == NULL ? NULL : (char **) &xmlin;
    args.cookie_in.cookie_in_val = (char *)cookiein;
    args.cookie_in.cookie_in_len = cookieinlen;
    args.flags = flags;
    args.dname = dname == NULL ? NULL : (char **) &dname;
    args.uri = uri == NULL ? NULL : (char **) &uri;
    args.dconnuri = dconnuri == NULL ? NULL : (char **) &dconnuri;
    args.resource = resource;

    if (call (dom->conn, priv, 0, REMOTE_PROC_DOMAIN_MIGRATE_PERFORM3,
              (xdrproc_t) xdr_remote_domain_migrate_perform3_args, (char *) &args,
              (xdrproc_t) xdr_remote_domain_migrate_perform3_ret, (char *) &ret) == -1)
        goto done;

    if (ret.cookie_out.cookie_out_len > 0) {
        if (!cookieout || !cookieoutlen) {
            remoteError(VIR_ERR_INTERNAL_ERROR, "%s",
                        _("caller ignores cookieout or cookieoutlen"));
            goto error;
        }
        *cookieout = ret.cookie_out.cookie_out_val; /* Caller frees. */
        *cookieoutlen = ret.cookie_out.cookie_out_len;
    }

    rv = 0;

done:
    remoteDriverUnlock(priv);
    return rv;

error:
    VIR_FREE(ret.cookie_out.cookie_out_val);
    goto done;
}


static virDomainPtr
remoteDomainMigrateFinish3(virConnectPtr dconn,
                           const char *dname,
                           const char *cookiein,
                           int cookieinlen,
                           char **cookieout,
                           int *cookieoutlen,
                           const char *dconnuri,
                           const char *uri,
                           unsigned long flags,
                           int cancelled)
{
    remote_domain_migrate_finish3_args args;
    remote_domain_migrate_finish3_ret ret;
    struct private_data *priv = dconn->privateData;
    virDomainPtr rv = NULL;

    remoteDriverLock(priv);

    memset(&args, 0, sizeof(args));
    memset(&ret, 0, sizeof(ret));

    args.cookie_in.cookie_in_val = (char *)cookiein;
    args.cookie_in.cookie_in_len = cookieinlen;
    args.dname = (char *) dname;
    args.uri = uri == NULL ? NULL : (char **) &uri;
    args.dconnuri = dconnuri == NULL ? NULL : (char **) &dconnuri;
    args.flags = flags;
    args.cancelled = cancelled;

    if (call (dconn, priv, 0, REMOTE_PROC_DOMAIN_MIGRATE_FINISH3,
              (xdrproc_t) xdr_remote_domain_migrate_finish3_args, (char *) &args,
              (xdrproc_t) xdr_remote_domain_migrate_finish3_ret, (char *) &ret) == -1)
        goto done;

    rv = get_nonnull_domain(dconn, ret.dom);

    if (ret.cookie_out.cookie_out_len > 0) {
        if (!cookieout || !cookieoutlen) {
            remoteError(VIR_ERR_INTERNAL_ERROR, "%s",
                        _("caller ignores cookieout or cookieoutlen"));
            goto error;
        }
        *cookieout = ret.cookie_out.cookie_out_val; /* Caller frees. */
        *cookieoutlen = ret.cookie_out.cookie_out_len;
        ret.cookie_out.cookie_out_val = NULL;
        ret.cookie_out.cookie_out_len = 0;
    }

    xdr_free ((xdrproc_t) &xdr_remote_domain_migrate_finish3_ret, (char *) &ret);

done:
    remoteDriverUnlock(priv);
    return rv;

error:
    VIR_FREE(ret.cookie_out.cookie_out_val);
    goto done;
}


static int
remoteDomainMigrateConfirm3(virDomainPtr domain,
                            const char *cookiein,
                            int cookieinlen,
                            unsigned long flags,
                            int cancelled)
{
    int rv = -1;
    remote_domain_migrate_confirm3_args args;
    struct private_data *priv = domain->conn->privateData;

    remoteDriverLock(priv);

    memset(&args, 0, sizeof(args));

    make_nonnull_domain (&args.dom, domain);
    args.cookie_in.cookie_in_len = cookieinlen;
    args.cookie_in.cookie_in_val = (char *) cookiein;
    args.flags = flags;
    args.cancelled = cancelled;

    if (call (domain->conn, priv, 0, REMOTE_PROC_DOMAIN_MIGRATE_CONFIRM3,
              (xdrproc_t) xdr_remote_domain_migrate_confirm3_args, (char *) &args,
              (xdrproc_t) xdr_void, (char *) NULL) == -1)
        goto done;

    rv = 0;

done:
    remoteDriverUnlock(priv);
    return rv;
}


#include "remote_client_bodies.h"
#include "qemu_client_bodies.h"


/*----------------------------------------------------------------------*/

static struct remote_thread_call *
prepareCall(struct private_data *priv,
            int flags,
            int proc_nr,
            xdrproc_t args_filter, char *args,
            xdrproc_t ret_filter, char *ret)
{
    XDR xdr;
    struct remote_message_header hdr;
    struct remote_thread_call *rv;

    if (VIR_ALLOC(rv) < 0) {
        virReportOOMError();
        return NULL;
    }

    if (virCondInit(&rv->cond) < 0) {
        VIR_FREE(rv);
        remoteError(VIR_ERR_INTERNAL_ERROR, "%s",
                    _("cannot initialize mutex"));
        return NULL;
    }

    /* Get a unique serial number for this message. */
    rv->serial = priv->counter++;
    rv->proc_nr = proc_nr;
    rv->ret_filter = ret_filter;
    rv->ret = ret;
    rv->want_reply = 1;

    if (flags & REMOTE_CALL_QEMU) {
        hdr.prog = QEMU_PROGRAM;
        hdr.vers = QEMU_PROTOCOL_VERSION;
    }
    else {
        hdr.prog = REMOTE_PROGRAM;
        hdr.vers = REMOTE_PROTOCOL_VERSION;
    }
    hdr.proc = proc_nr;
    hdr.type = REMOTE_CALL;
    hdr.serial = rv->serial;
    hdr.status = REMOTE_OK;

    /* Serialise header followed by args. */
    xdrmem_create (&xdr, rv->buffer+4, REMOTE_MESSAGE_MAX, XDR_ENCODE);
    if (!xdr_remote_message_header (&xdr, &hdr)) {
        remoteError(VIR_ERR_RPC, "%s", _("xdr_remote_message_header failed"));
        goto error;
    }

    if (!(*args_filter) (&xdr, args)) {
        remoteError(VIR_ERR_RPC,
                    _("Unable to marshal arguments for program %d version %d procedure %d type %d status %d"),
                    hdr.prog, hdr.vers, hdr.proc, hdr.type, hdr.status);
        goto error;
    }

    /* Get the length stored in buffer. */
    rv->bufferLength = xdr_getpos (&xdr);
    xdr_destroy (&xdr);

    /* Length must include the length word itself (always encoded in
     * 4 bytes as per RFC 4506).
     */
    rv->bufferLength += REMOTE_MESSAGE_HEADER_XDR_LEN;

    /* Encode the length word. */
    xdrmem_create (&xdr, rv->buffer, REMOTE_MESSAGE_HEADER_XDR_LEN, XDR_ENCODE);
    if (!xdr_u_int (&xdr, &rv->bufferLength)) {
        remoteError(VIR_ERR_RPC, "%s", _("xdr_u_int (length word)"));
        goto error;
    }
    xdr_destroy (&xdr);

    return rv;

error:
    xdr_destroy (&xdr);
    ignore_value(virCondDestroy(&rv->cond));
    VIR_FREE(rv);
    return NULL;
}



static int
remoteIOWriteBuffer(struct private_data *priv,
                    const char *bytes, int len)
{
    int ret;

    if (priv->uses_tls) {
    tls_resend:
        ret = gnutls_record_send (priv->session, bytes, len);
        if (ret < 0) {
            if (ret == GNUTLS_E_INTERRUPTED)
                goto tls_resend;
            if (ret == GNUTLS_E_AGAIN)
                return 0;

            remoteError(VIR_ERR_GNUTLS_ERROR, "%s", gnutls_strerror (ret));
            return -1;
        }
    } else {
    resend:
        ret = send (priv->sock, bytes, len, 0);
        if (ret == -1) {
            if (errno == EINTR)
                goto resend;
            if (errno == EWOULDBLOCK)
                return 0;

            virReportSystemError(errno, "%s", _("cannot send data"));
            return -1;

        }
    }

    return ret;
}


static int
remoteIOReadBuffer(struct private_data *priv,
                   char *bytes, int len)
{
    int ret;

    if (priv->uses_tls) {
    tls_resend:
        ret = gnutls_record_recv (priv->session, bytes, len);
        if (ret == GNUTLS_E_INTERRUPTED)
            goto tls_resend;
        if (ret == GNUTLS_E_AGAIN)
            return 0;

        /* Treat 0 == EOF as an error */
        if (ret <= 0) {
            if (ret < 0)
                remoteError(VIR_ERR_GNUTLS_ERROR,
                            _("failed to read from TLS socket %s"),
                            gnutls_strerror (ret));
            else
                remoteError(VIR_ERR_SYSTEM_ERROR, "%s",
                            _("server closed connection"));
            return -1;
        }
    } else {
    resend:
        ret = recv (priv->sock, bytes, len, 0);
        if (ret <= 0) {
            if (ret == -1) {
                if (errno == EINTR)
                    goto resend;
                if (errno == EWOULDBLOCK)
                    return 0;

                char errout[1024] = "\0";
                if (priv->errfd != -1) {
                    if (saferead(priv->errfd, errout, sizeof(errout)) < 0) {
                        virReportSystemError(errno, "%s",
                                             _("cannot recv data"));
                        return -1;
                    }
                }

                virReportSystemError(errno,
                                     _("cannot recv data: %s"), errout);

            } else {
                char errout[1024] = "\0";
                if (priv->errfd != -1) {
                    if (saferead(priv->errfd, errout, sizeof(errout)) < 0) {
                        remoteError(VIR_ERR_SYSTEM_ERROR,
                                    _("server closed connection: %s"),
                                    virStrerror(errno, errout, sizeof errout));
                        return -1;
                    }
                }

                remoteError(VIR_ERR_SYSTEM_ERROR,
                            _("server closed connection: %s"), errout);
            }
            return -1;
        }
    }

    return ret;
}


static int
remoteIOWriteMessage(struct private_data *priv,
                     struct remote_thread_call *thecall)
{
#if HAVE_SASL
    if (priv->saslconn) {
        const char *output;
        unsigned int outputlen;
        int err, ret;

        if (!priv->saslEncoded) {
            err = sasl_encode(priv->saslconn,
                              thecall->buffer + thecall->bufferOffset,
                              thecall->bufferLength - thecall->bufferOffset,
                              &output, &outputlen);
            if (err != SASL_OK) {
                remoteError(VIR_ERR_INTERNAL_ERROR,
                            _("failed to encode SASL data: %s"),
                            sasl_errstring(err, NULL, NULL));
                return -1;
            }
            priv->saslEncoded = output;
            priv->saslEncodedLength = outputlen;
            priv->saslEncodedOffset = 0;

            thecall->bufferOffset = thecall->bufferLength;
        }

        ret = remoteIOWriteBuffer(priv,
                                  priv->saslEncoded + priv->saslEncodedOffset,
                                  priv->saslEncodedLength - priv->saslEncodedOffset);
        if (ret < 0)
            return ret;
        priv->saslEncodedOffset += ret;

        if (priv->saslEncodedOffset == priv->saslEncodedLength) {
            priv->saslEncoded = NULL;
            priv->saslEncodedOffset = priv->saslEncodedLength = 0;
            if (thecall->want_reply)
                thecall->mode = REMOTE_MODE_WAIT_RX;
            else
                thecall->mode = REMOTE_MODE_COMPLETE;
        }
    } else {
#endif
        int ret;
        ret = remoteIOWriteBuffer(priv,
                                  thecall->buffer + thecall->bufferOffset,
                                  thecall->bufferLength - thecall->bufferOffset);
        if (ret < 0)
            return ret;
        thecall->bufferOffset += ret;

        if (thecall->bufferOffset == thecall->bufferLength) {
            thecall->bufferOffset = thecall->bufferLength = 0;
            if (thecall->want_reply)
                thecall->mode = REMOTE_MODE_WAIT_RX;
            else
                thecall->mode = REMOTE_MODE_COMPLETE;
        }
#if HAVE_SASL
    }
#endif
    return 0;
}


static int
remoteIOHandleOutput(struct private_data *priv) {
    struct remote_thread_call *thecall = priv->waitDispatch;

    while (thecall &&
           thecall->mode != REMOTE_MODE_WAIT_TX)
        thecall = thecall->next;

    if (!thecall)
        return -1; /* Shouldn't happen, but you never know... */

    while (thecall) {
        int ret = remoteIOWriteMessage(priv, thecall);
        if (ret < 0)
            return ret;

        if (thecall->mode == REMOTE_MODE_WAIT_TX)
            return 0; /* Blocking write, to back to event loop */

        thecall = thecall->next;
    }

    return 0; /* No more calls to send, all done */
}

static int
remoteIOReadMessage(struct private_data *priv) {
    unsigned int wantData;

    /* Start by reading length word */
    if (priv->bufferLength == 0)
        priv->bufferLength = 4;

    wantData = priv->bufferLength - priv->bufferOffset;

#if HAVE_SASL
    if (priv->saslconn) {
        if (priv->saslDecoded == NULL) {
            int ret, err;
            ret = remoteIOReadBuffer(priv, priv->saslTemporary,
                                     sizeof(priv->saslTemporary));
            if (ret < 0)
                return -1;
            if (ret == 0)
                return 0;

            err = sasl_decode(priv->saslconn, priv->saslTemporary, ret,
                              &priv->saslDecoded, &priv->saslDecodedLength);
            if (err != SASL_OK) {
                remoteError(VIR_ERR_INTERNAL_ERROR,
                            _("failed to decode SASL data: %s"),
                            sasl_errstring(err, NULL, NULL));
                return -1;
            }
            priv->saslDecodedOffset = 0;
        }

        if ((priv->saslDecodedLength - priv->saslDecodedOffset) < wantData)
            wantData = (priv->saslDecodedLength - priv->saslDecodedOffset);

        memcpy(priv->buffer + priv->bufferOffset,
               priv->saslDecoded + priv->saslDecodedOffset,
               wantData);
        priv->saslDecodedOffset += wantData;
        priv->bufferOffset += wantData;
        if (priv->saslDecodedOffset == priv->saslDecodedLength) {
            priv->saslDecodedOffset = priv->saslDecodedLength = 0;
            priv->saslDecoded = NULL;
        }

        return wantData;
    } else {
#endif
        int ret;

        ret = remoteIOReadBuffer(priv,
                                 priv->buffer + priv->bufferOffset,
                                 wantData);
        if (ret < 0)
            return -1;
        if (ret == 0)
            return 0;

        priv->bufferOffset += ret;

        return ret;
#if HAVE_SASL
    }
#endif
}


static int
remoteIODecodeMessageLength(struct private_data *priv) {
    XDR xdr;
    unsigned int len;

    xdrmem_create (&xdr, priv->buffer, priv->bufferLength, XDR_DECODE);
    if (!xdr_u_int (&xdr, &len)) {
        remoteError(VIR_ERR_RPC, "%s", _("xdr_u_int (length word, reply)"));
        return -1;
    }
    xdr_destroy (&xdr);

    if (len < REMOTE_MESSAGE_HEADER_XDR_LEN) {
        remoteError(VIR_ERR_RPC, "%s",
                    _("packet received from server too small"));
        return -1;
    }

    /* Length includes length word - adjust to real length to read. */
    len -= REMOTE_MESSAGE_HEADER_XDR_LEN;

    if (len > REMOTE_MESSAGE_MAX) {
        remoteError(VIR_ERR_RPC, "%s",
                    _("packet received from server too large"));
        return -1;
    }

    /* Extend our declared buffer length and carry
       on reading the header + payload */
    priv->bufferLength += len;
    VIR_DEBUG("Got length, now need %d total (%d more)", priv->bufferLength, len);
    return 0;
}


static int
processCallDispatchReply(virConnectPtr conn, struct private_data *priv,
                         remote_message_header *hdr,
                         XDR *xdr);

static int
processCallDispatchMessage(virConnectPtr conn, struct private_data *priv,
                           int in_open,
                           remote_message_header *hdr,
                           XDR *xdr);

static int
processCallDispatchStream(virConnectPtr conn, struct private_data *priv,
                          remote_message_header *hdr,
                          XDR *xdr);


static int
processCallDispatch(virConnectPtr conn, struct private_data *priv,
                    int flags) {
    XDR xdr;
    struct remote_message_header hdr;
    int len = priv->bufferLength - 4;
    int rv = -1;
    int expectedprog;
    int expectedvers;

    /* Length word has already been read */
    priv->bufferOffset = 4;

    /* Deserialise reply header. */
    xdrmem_create (&xdr, priv->buffer + priv->bufferOffset, len, XDR_DECODE);
    if (!xdr_remote_message_header (&xdr, &hdr)) {
        remoteError(VIR_ERR_RPC, "%s", _("invalid header in reply"));
        return -1;
    }

    priv->bufferOffset += xdr_getpos(&xdr);

    expectedprog = REMOTE_PROGRAM;
    expectedvers = REMOTE_PROTOCOL_VERSION;
    if (flags & REMOTE_CALL_QEMU) {
        expectedprog = QEMU_PROGRAM;
        expectedvers = QEMU_PROTOCOL_VERSION;
    }

    /* Check program, version, etc. are what we expect. */
    if (hdr.prog != expectedprog) {
        remoteError(VIR_ERR_RPC,
                    _("unknown program (received %x, expected %x)"),
                    hdr.prog, expectedprog);
        return -1;
    }
    if (hdr.vers != expectedvers) {
        remoteError(VIR_ERR_RPC,
                    _("unknown protocol version (received %x, expected %x)"),
                    hdr.vers, expectedvers);
        return -1;
    }


    switch (hdr.type) {
    case REMOTE_REPLY: /* Normal RPC replies */
        rv = processCallDispatchReply(conn, priv, &hdr, &xdr);
        break;

    case REMOTE_MESSAGE: /* Async notifications */
        VIR_DEBUG("Dispatch event %d %d", hdr.proc, priv->bufferLength);
        rv = processCallDispatchMessage(conn, priv, flags & REMOTE_CALL_IN_OPEN,
                                        &hdr, &xdr);
        break;

    case REMOTE_STREAM: /* Stream protocol */
        rv = processCallDispatchStream(conn, priv, &hdr, &xdr);
        break;

    default:
        remoteError(VIR_ERR_RPC,
                    _("got unexpected RPC call %d from server"),
                    hdr.proc);
        rv = -1;
        break;
    }

    xdr_destroy(&xdr);
    return rv;
}


static int
processCallDispatchReply(virConnectPtr conn ATTRIBUTE_UNUSED,
                         struct private_data *priv,
                         remote_message_header *hdr,
                         XDR *xdr) {
    struct remote_thread_call *thecall;

    /* Ok, definitely got an RPC reply now find
       out who's been waiting for it */
    thecall = priv->waitDispatch;
    while (thecall &&
           thecall->serial != hdr->serial)
        thecall = thecall->next;

    if (!thecall) {
        remoteError(VIR_ERR_RPC,
                    _("no call waiting for reply with serial %d"),
                    hdr->serial);
        return -1;
    }

    if (hdr->proc != thecall->proc_nr) {
        remoteError(VIR_ERR_RPC,
                    _("unknown procedure (received %x, expected %x)"),
                    hdr->proc, thecall->proc_nr);
        return -1;
    }

    /* Status is either REMOTE_OK (meaning that what follows is a ret
     * structure), or REMOTE_ERROR (and what follows is a remote_error
     * structure).
     */
    switch (hdr->status) {
    case REMOTE_OK:
        if (!(*thecall->ret_filter) (xdr, thecall->ret)) {
            remoteError(VIR_ERR_RPC,
                        _("Unable to marshal reply for program %d version %d procedure %d type %d status %d"),
                        hdr->prog, hdr->vers, hdr->proc, hdr->type, hdr->status);
            return -1;
        }
        thecall->mode = REMOTE_MODE_COMPLETE;
        return 0;

    case REMOTE_ERROR:
        memset (&thecall->err, 0, sizeof thecall->err);
        if (!xdr_remote_error (xdr, &thecall->err)) {
            remoteError(VIR_ERR_RPC,
                        _("Unable to marshal error for program %d version %d procedure %d type %d status %d"),
                        hdr->prog, hdr->vers, hdr->proc, hdr->type, hdr->status);
            return -1;
        }
        thecall->mode = REMOTE_MODE_ERROR;
        return 0;

    default:
        remoteError(VIR_ERR_RPC, _("unknown status (received %x)"), hdr->status);
        return -1;
    }
}

static int
processCallDispatchMessage(virConnectPtr conn, struct private_data *priv,
                           int in_open,
                           remote_message_header *hdr,
                           XDR *xdr) {
    virDomainEventPtr event = NULL;
    /* An async message has come in while we were waiting for the
     * response. Process it to pull it off the wire, and try again
     */

    if (in_open) {
        VIR_DEBUG("Ignoring bogus event %d received while in open", hdr->proc);
        return -1;
    }

    switch (hdr->proc) {
    case REMOTE_PROC_DOMAIN_EVENT_LIFECYCLE:
        event = remoteDomainReadEventLifecycle(conn, xdr);
        break;

    case REMOTE_PROC_DOMAIN_EVENT_REBOOT:
        event = remoteDomainReadEventReboot(conn, xdr);
        break;

    case REMOTE_PROC_DOMAIN_EVENT_RTC_CHANGE:
        event = remoteDomainReadEventRTCChange(conn, xdr);
        break;

    case REMOTE_PROC_DOMAIN_EVENT_WATCHDOG:
        event = remoteDomainReadEventWatchdog(conn, xdr);
        break;

    case REMOTE_PROC_DOMAIN_EVENT_IO_ERROR:
        event = remoteDomainReadEventIOError(conn, xdr);
        break;

    case REMOTE_PROC_DOMAIN_EVENT_IO_ERROR_REASON:
        event = remoteDomainReadEventIOErrorReason(conn, xdr);
        break;

    case REMOTE_PROC_DOMAIN_EVENT_GRAPHICS:
        event = remoteDomainReadEventGraphics(conn, xdr);
        break;

    default:
        VIR_DEBUG("Unexpected event proc %d", hdr->proc);
        break;
    }
    VIR_DEBUG("Event ready for queue %p %p", event, conn);

    if (!event)
        return -1;

    remoteDomainEventQueue(priv, event);
    return 0;
}

static int
processCallDispatchStream(virConnectPtr conn ATTRIBUTE_UNUSED,
                          struct private_data *priv,
                          remote_message_header *hdr,
                          XDR *xdr) {
    struct private_stream_data *privst;
    struct remote_thread_call *thecall;

    /* Try and find a matching stream */
    privst = priv->streams;
    while (privst &&
           privst->serial != hdr->serial &&
           privst->proc_nr != hdr->proc)
        privst = privst->next;

    if (!privst) {
        VIR_DEBUG("No registered stream matching serial=%d, proc=%d",
                  hdr->serial, hdr->proc);
        return -1;
    }

    /* See if there's also a (optional) call waiting for this reply */
    thecall = priv->waitDispatch;
    while (thecall &&
           thecall->serial != hdr->serial)
        thecall = thecall->next;


    /* Status is either REMOTE_OK (meaning that what follows is a ret
     * structure), or REMOTE_ERROR (and what follows is a remote_error
     * structure).
     */
    switch (hdr->status) {
    case REMOTE_CONTINUE: {
        int avail = privst->incomingLength - privst->incomingOffset;
        int need = priv->bufferLength - priv->bufferOffset;
        VIR_DEBUG("Got a stream data packet");

        /* XXX flag stream as complete somwhere if need==0 */

        if (need > avail) {
            int extra = need - avail;
            if (VIR_REALLOC_N(privst->incoming,
                              privst->incomingLength + extra) < 0) {
                VIR_DEBUG("Out of memory handling stream data");
                return -1;
            }
            privst->incomingLength += extra;
        }

        memcpy(privst->incoming + privst->incomingOffset,
               priv->buffer + priv->bufferOffset,
               priv->bufferLength - priv->bufferOffset);
        privst->incomingOffset += (priv->bufferLength - priv->bufferOffset);

        if (thecall && thecall->want_reply) {
            VIR_DEBUG("Got sync data packet offset=%d", privst->incomingOffset);
            thecall->mode = REMOTE_MODE_COMPLETE;
        } else {
            VIR_DEBUG("Got aysnc data packet offset=%d", privst->incomingOffset);
            remoteStreamEventTimerUpdate(privst);
        }
        return 0;
    }

    case REMOTE_OK:
        VIR_DEBUG("Got a synchronous confirm");
        if (!thecall) {
            VIR_DEBUG("Got unexpected stream finish confirmation");
            return -1;
        }
        thecall->mode = REMOTE_MODE_COMPLETE;
        return 0;

    case REMOTE_ERROR:
        if (thecall && thecall->want_reply) {
            VIR_DEBUG("Got a synchronous error");
            /* Give the error straight to this call */
            memset (&thecall->err, 0, sizeof thecall->err);
            if (!xdr_remote_error (xdr, &thecall->err)) {
                remoteError(VIR_ERR_RPC, "%s", _("unmarshaling remote_error"));
                return -1;
            }
            thecall->mode = REMOTE_MODE_ERROR;
        } else {
            VIR_DEBUG("Got a asynchronous error");
            /* No call, so queue the error against the stream */
            if (privst->has_error) {
                VIR_DEBUG("Got unexpected duplicate stream error");
                return -1;
            }
            privst->has_error = 1;
            memset (&privst->err, 0, sizeof privst->err);
            if (!xdr_remote_error (xdr, &privst->err)) {
                VIR_DEBUG("Failed to unmarshal error");
                return -1;
            }
        }
        return 0;

    default:
        VIR_WARN("Stream with unexpected serial=%d, proc=%d, status=%d",
                 hdr->serial, hdr->proc, hdr->status);
        return -1;
    }
}

static int
remoteIOHandleInput(virConnectPtr conn, struct private_data *priv,
                    int flags)
{
    /* Read as much data as is available, until we get
     * EAGAIN
     */
    for (;;) {
        int ret = remoteIOReadMessage(priv);

        if (ret < 0)
            return -1;
        if (ret == 0)
            return 0;  /* Blocking on read */

        /* Check for completion of our goal */
        if (priv->bufferOffset == priv->bufferLength) {
            if (priv->bufferOffset == 4) {
                ret = remoteIODecodeMessageLength(priv);
                if (ret < 0)
                    return -1;

                /*
                 * We'll carry on around the loop to immediately
                 * process the message body, because it has probably
                 * already arrived. Worst case, we'll get EAGAIN on
                 * next iteration.
                 */
            } else {
                ret = processCallDispatch(conn, priv, flags);
                priv->bufferOffset = priv->bufferLength = 0;
                /*
                 * We've completed one call, but we don't want to
                 * spin around the loop forever if there are many
                 * incoming async events, or replies for other
                 * thread's RPC calls. We want to get out & let
                 * any other thread take over as soon as we've
                 * got our reply. When SASL is active though, we
                 * may have read more data off the wire than we
                 * initially wanted & cached it in memory. In this
                 * case, poll() would not detect that there is more
                 * ready todo.
                 *
                 * So if SASL is active *and* some SASL data is
                 * already cached, then we'll process that now,
                 * before returning.
                 */
#if HAVE_SASL
                if (ret == 0 &&
                    priv->saslconn &&
                    priv->saslDecoded)
                    continue;
#endif
                return ret;
            }
        }
    }
}

/*
 * Process all calls pending dispatch/receive until we
 * get a reply to our own call. Then quit and pass the buck
 * to someone else.
 */
static int
remoteIOEventLoop(virConnectPtr conn,
                  struct private_data *priv,
                  int flags,
                  struct remote_thread_call *thiscall)
{
    struct pollfd fds[2];
    int ret;

    fds[0].fd = priv->sock;
    fds[1].fd = priv->wakeupReadFD;

    for (;;) {
        struct remote_thread_call *tmp = priv->waitDispatch;
        struct remote_thread_call *prev;
        char ignore;
#ifdef HAVE_PTHREAD_SIGMASK
        sigset_t oldmask, blockedsigs;
#endif
        int timeout = -1;

        /* If we have existing SASL decoded data we
         * don't want to sleep in the poll(), just
         * check if any other FDs are also ready
         */
#if HAVE_SASL
        if (priv->saslDecoded)
            timeout = 0;
#endif

        fds[0].events = fds[0].revents = 0;
        fds[1].events = fds[1].revents = 0;

        fds[1].events = POLLIN;
        while (tmp) {
            if (tmp->mode == REMOTE_MODE_WAIT_RX)
                fds[0].events |= POLLIN;
            if (tmp->mode == REMOTE_MODE_WAIT_TX)
                fds[0].events |= POLLOUT;

            tmp = tmp->next;
        }

        if (priv->streams)
            fds[0].events |= POLLIN;

        /* Release lock while poll'ing so other threads
         * can stuff themselves on the queue */
        remoteDriverUnlock(priv);

        /* Block SIGWINCH from interrupting poll in curses programs,
         * then restore the original signal mask again immediately
         * after the call (RHBZ#567931).  Same for SIGCHLD and SIGPIPE
         * at the suggestion of Paolo Bonzini and Daniel Berrange.
         */
#ifdef HAVE_PTHREAD_SIGMASK
        sigemptyset (&blockedsigs);
        sigaddset (&blockedsigs, SIGWINCH);
        sigaddset (&blockedsigs, SIGCHLD);
        sigaddset (&blockedsigs, SIGPIPE);
        ignore_value(pthread_sigmask(SIG_BLOCK, &blockedsigs, &oldmask));
#endif

    repoll:
        ret = poll(fds, ARRAY_CARDINALITY(fds), timeout);
        if (ret < 0 && errno == EAGAIN)
            goto repoll;

#ifdef HAVE_PTHREAD_SIGMASK
        ignore_value(pthread_sigmask(SIG_SETMASK, &oldmask, NULL));
#endif

        remoteDriverLock(priv);

        /* If we have existing SASL decoded data, pretend
         * the socket became readable so we consume it
         */
#if HAVE_SASL
        if (priv->saslDecoded)
            fds[0].revents |= POLLIN;
#endif

        if (fds[1].revents) {
            ssize_t s;
            VIR_DEBUG("Woken up from poll by other thread");
            s = saferead(priv->wakeupReadFD, &ignore, sizeof(ignore));
            if (s < 0) {
                virReportSystemError(errno, "%s",
                                     _("read on wakeup fd failed"));
                goto error;
            } else if (s != sizeof(ignore)) {
                remoteError(VIR_ERR_INTERNAL_ERROR, "%s",
                            _("read on wakeup fd failed"));
                goto error;
            }
        }

        if (ret < 0) {
            if (errno == EWOULDBLOCK)
                continue;
            virReportSystemError(errno,
                                 "%s", _("poll on socket failed"));
            goto error;
        }

        if (fds[0].revents & POLLOUT) {
            if (remoteIOHandleOutput(priv) < 0)
                goto error;
        }

        if (fds[0].revents & POLLIN) {
            if (remoteIOHandleInput(conn, priv, flags) < 0)
                goto error;
        }

        /* Iterate through waiting threads and if
         * any are complete then tell 'em to wakeup
         */
        tmp = priv->waitDispatch;
        prev = NULL;
        while (tmp) {
            if (tmp != thiscall &&
                (tmp->mode == REMOTE_MODE_COMPLETE ||
                 tmp->mode == REMOTE_MODE_ERROR)) {
                /* Take them out of the list */
                if (prev)
                    prev->next = tmp->next;
                else
                    priv->waitDispatch = tmp->next;

                /* And wake them up....
                 * ...they won't actually wakeup until
                 * we release our mutex a short while
                 * later...
                 */
                VIR_DEBUG("Waking up sleep %d %p %p", tmp->proc_nr, tmp, priv->waitDispatch);
                virCondSignal(&tmp->cond);
            } else {
                prev = tmp;
            }
            tmp = tmp->next;
        }

        /* Now see if *we* are done */
        if (thiscall->mode == REMOTE_MODE_COMPLETE ||
            thiscall->mode == REMOTE_MODE_ERROR) {
            /* We're at head of the list already, so
             * remove us
             */
            priv->waitDispatch = thiscall->next;
            VIR_DEBUG("Giving up the buck %d %p %p", thiscall->proc_nr, thiscall, priv->waitDispatch);
            /* See if someone else is still waiting
             * and if so, then pass the buck ! */
            if (priv->waitDispatch) {
                VIR_DEBUG("Passing the buck to %d %p", priv->waitDispatch->proc_nr, priv->waitDispatch);
                virCondSignal(&priv->waitDispatch->cond);
            }
            return 0;
        }


        if (fds[0].revents & (POLLHUP | POLLERR)) {
            remoteError(VIR_ERR_INTERNAL_ERROR, "%s",
                        _("received hangup / error event on socket"));
            goto error;
        }
    }


error:
    priv->waitDispatch = thiscall->next;
    VIR_DEBUG("Giving up the buck due to I/O error %d %p %p", thiscall->proc_nr, thiscall, priv->waitDispatch);
    /* See if someone else is still waiting
     * and if so, then pass the buck ! */
    if (priv->waitDispatch) {
        VIR_DEBUG("Passing the buck to %d %p", priv->waitDispatch->proc_nr, priv->waitDispatch);
        virCondSignal(&priv->waitDispatch->cond);
    }
    return -1;
}

/*
 * This function sends a message to remote server and awaits a reply
 *
 * NB. This does not free the args structure (not desirable, since you
 * often want this allocated on the stack or else it contains strings
 * which come from the user).  It does however free any intermediate
 * results, eg. the error structure if there is one.
 *
 * NB(2). Make sure to memset (&ret, 0, sizeof ret) before calling,
 * else Bad Things will happen in the XDR code.
 *
 * NB(3) You must have the private_data lock before calling this
 *
 * NB(4) This is very complicated. Due to connection cloning, multiple
 * threads can want to use the socket at once. Obviously only one of
 * them can. So if someone's using the socket, other threads are put
 * to sleep on condition variables. The existing thread may completely
 * send & receive their RPC call/reply while they're asleep. Or it
 * may only get around to dealing with sending the call. Or it may
 * get around to neither. So upon waking up from slumber, the other
 * thread may or may not have more work todo.
 *
 * We call this dance  'passing the buck'
 *
 *      http://en.wikipedia.org/wiki/Passing_the_buck
 *
 *   "Buck passing or passing the buck is the action of transferring
 *    responsibility or blame unto another person. It is also used as
 *    a strategy in power politics when the actions of one country/
 *    nation are blamed on another, providing an opportunity for war."
 *
 * NB(5) Don't Panic!
 */
static int
remoteIO(virConnectPtr conn,
         struct private_data *priv,
         int flags,
         struct remote_thread_call *thiscall)
{
    int rv;

    VIR_DEBUG("Do proc=%d serial=%d length=%d wait=%p",
          thiscall->proc_nr, thiscall->serial,
          thiscall->bufferLength, priv->waitDispatch);

    /* Check to see if another thread is dispatching */
    if (priv->waitDispatch) {
        /* Stick ourselves on the end of the wait queue */
        struct remote_thread_call *tmp = priv->waitDispatch;
        char ignore = 1;
        ssize_t s;
        while (tmp && tmp->next)
            tmp = tmp->next;
        if (tmp)
            tmp->next = thiscall;
        else
            priv->waitDispatch = thiscall;

        /* Force other thread to wakeup from poll */
        s = safewrite(priv->wakeupSendFD, &ignore, sizeof(ignore));
        if (s < 0) {
            char errout[1024];
            remoteError(VIR_ERR_INTERNAL_ERROR,
                        _("failed to wake up polling thread: %s"),
                        virStrerror(errno, errout, sizeof errout));
            return -1;
        } else if (s != sizeof(ignore)) {
            remoteError(VIR_ERR_INTERNAL_ERROR, "%s",
                        _("failed to wake up polling thread"));
            return -1;
        }

        VIR_DEBUG("Going to sleep %d %p %p", thiscall->proc_nr, priv->waitDispatch, thiscall);
        /* Go to sleep while other thread is working... */
        if (virCondWait(&thiscall->cond, &priv->lock) < 0) {
            if (priv->waitDispatch == thiscall) {
                priv->waitDispatch = thiscall->next;
            } else {
                tmp = priv->waitDispatch;
                while (tmp && tmp->next &&
                       tmp->next != thiscall) {
                    tmp = tmp->next;
                }
                if (tmp && tmp->next == thiscall)
                    tmp->next = thiscall->next;
            }
            remoteError(VIR_ERR_INTERNAL_ERROR, "%s",
                        _("failed to wait on condition"));
            return -1;
        }

        VIR_DEBUG("Wokeup from sleep %d %p %p", thiscall->proc_nr, priv->waitDispatch, thiscall);
        /* Two reasons we can be woken up
         *  1. Other thread has got our reply ready for us
         *  2. Other thread is all done, and it is our turn to
         *     be the dispatcher to finish waiting for
         *     our reply
         */
        if (thiscall->mode == REMOTE_MODE_COMPLETE ||
            thiscall->mode == REMOTE_MODE_ERROR) {
            /*
             * We avoided catching the buck and our reply is ready !
             * We've already had 'thiscall' removed from the list
             * so just need to (maybe) handle errors & free it
             */
            goto cleanup;
        }

        /* Grr, someone passed the buck onto us ... */

    } else {
        /* We're first to catch the buck */
        priv->waitDispatch = thiscall;
    }

    VIR_DEBUG("We have the buck %d %p %p", thiscall->proc_nr, priv->waitDispatch, thiscall);
    /*
     * The buck stops here!
     *
     * At this point we're about to own the dispatch
     * process...
     */

    /*
     * Avoid needless wake-ups of the event loop in the
     * case where this call is being made from a different
     * thread than the event loop. These wake-ups would
     * cause the event loop thread to be blocked on the
     * mutex for the duration of the call
     */
    if (priv->watch >= 0)
        virEventUpdateHandle(priv->watch, 0);

    rv = remoteIOEventLoop(conn, priv, flags, thiscall);

    if (priv->watch >= 0)
        virEventUpdateHandle(priv->watch, VIR_EVENT_HANDLE_READABLE);

    if (rv < 0)
        return -1;

cleanup:
    VIR_DEBUG("All done with our call %d %p %p", thiscall->proc_nr,
          priv->waitDispatch, thiscall);
    if (thiscall->mode == REMOTE_MODE_ERROR) {
        /* Interop for virErrorNumber glitch in 0.8.0, if server is
         * 0.7.1 through 0.7.7; see comments in virterror.h. */
        switch (thiscall->err.code) {
        case VIR_WAR_NO_NWFILTER:
            /* no way to tell old VIR_WAR_NO_SECRET apart from
             * VIR_WAR_NO_NWFILTER, but both are very similar
             * warnings, so ignore the difference */
            break;
        case VIR_ERR_INVALID_NWFILTER:
        case VIR_ERR_NO_NWFILTER:
        case VIR_ERR_BUILD_FIREWALL:
            /* server was trying to pass VIR_ERR_INVALID_SECRET,
             * VIR_ERR_NO_SECRET, or VIR_ERR_CONFIG_UNSUPPORTED */
            if (thiscall->err.domain != VIR_FROM_NWFILTER)
                thiscall->err.code += 4;
            break;
        case VIR_WAR_NO_SECRET:
            if (thiscall->err.domain == VIR_FROM_QEMU)
                thiscall->err.code = VIR_ERR_OPERATION_TIMEOUT;
            break;
        case VIR_ERR_INVALID_SECRET:
            if (thiscall->err.domain == VIR_FROM_XEN)
                thiscall->err.code = VIR_ERR_MIGRATE_PERSIST_FAILED;
            break;
        default:
            /* Nothing to alter. */
            break;
        }

        /* See if caller asked us to keep quiet about missing RPCs
         * eg for interop with older servers */
        if (flags & REMOTE_CALL_QUIET_MISSING_RPC &&
            thiscall->err.domain == VIR_FROM_REMOTE &&
            thiscall->err.code == VIR_ERR_RPC &&
            thiscall->err.level == VIR_ERR_ERROR &&
            thiscall->err.message &&
            STRPREFIX(*thiscall->err.message, "unknown procedure")) {
            rv = -2;
        } else if (thiscall->err.domain == VIR_FROM_REMOTE &&
                   thiscall->err.code == VIR_ERR_RPC &&
                   thiscall->err.level == VIR_ERR_ERROR &&
                   thiscall->err.message &&
                   STRPREFIX(*thiscall->err.message, "unknown procedure")) {
            /*
             * convert missing remote entry points into the unsupported
             * feature error
             */
            virRaiseErrorFull(__FILE__, __FUNCTION__, __LINE__,
                              thiscall->err.domain,
                              VIR_ERR_NO_SUPPORT,
                              thiscall->err.level,
                              thiscall->err.str1 ? *thiscall->err.str1 : NULL,
                              thiscall->err.str2 ? *thiscall->err.str2 : NULL,
                              thiscall->err.str3 ? *thiscall->err.str3 : NULL,
                              thiscall->err.int1,
                              thiscall->err.int2,
                              "%s", *thiscall->err.message);
            rv = -1;
        } else {
            virRaiseErrorFull(__FILE__, __FUNCTION__, __LINE__,
                              thiscall->err.domain,
                              thiscall->err.code,
                              thiscall->err.level,
                              thiscall->err.str1 ? *thiscall->err.str1 : NULL,
                              thiscall->err.str2 ? *thiscall->err.str2 : NULL,
                              thiscall->err.str3 ? *thiscall->err.str3 : NULL,
                              thiscall->err.int1,
                              thiscall->err.int2,
                              "%s", thiscall->err.message ? *thiscall->err.message : "unknown");
            rv = -1;
        }
        xdr_free((xdrproc_t)xdr_remote_error,  (char *)&thiscall->err);
    } else {
        rv = 0;
    }
    return rv;
}


/*
 * Serial a set of arguments into a method call message,
 * send that to the server and wait for reply
 */
static int
call (virConnectPtr conn, struct private_data *priv,
      int flags,
      int proc_nr,
      xdrproc_t args_filter, char *args,
      xdrproc_t ret_filter, char *ret)
{
    struct remote_thread_call *thiscall;
    int rv;

    thiscall = prepareCall(priv, flags, proc_nr, args_filter, args,
                           ret_filter, ret);

    if (!thiscall) {
        return -1;
    }

    rv = remoteIO(conn, priv, flags, thiscall);
    ignore_value(virCondDestroy(&thiscall->cond));
    VIR_FREE(thiscall);
    return rv;
}


/** remoteDomainEventFired:
 *
 * The callback for monitoring the remote socket
 * for event data
 */
void
remoteDomainEventFired(int watch,
                       int fd,
                       int event,
                       void *opaque)
{
    virConnectPtr        conn = opaque;
    struct private_data *priv = conn->privateData;

    remoteDriverLock(priv);

    /* This should be impossible, but it doesn't hurt to check */
    if (priv->waitDispatch)
        goto done;

    VIR_DEBUG("Event fired %d %d %d %X", watch, fd, event, event);

    if (event & (VIR_EVENT_HANDLE_HANGUP | VIR_EVENT_HANDLE_ERROR)) {
         VIR_DEBUG("%s : VIR_EVENT_HANDLE_HANGUP or "
               "VIR_EVENT_HANDLE_ERROR encountered", __FUNCTION__);
         virEventRemoveHandle(watch);
         priv->watch = -1;
         goto done;
    }

    if (fd != priv->sock) {
        virEventRemoveHandle(watch);
        priv->watch = -1;
        goto done;
    }

    if (remoteIOHandleInput(conn, priv, 0) < 0)
        VIR_DEBUG("Something went wrong during async message processing");

done:
    remoteDriverUnlock(priv);
}

static void remoteDomainEventDispatchFunc(virConnectPtr conn,
                                          virDomainEventPtr event,
                                          virConnectDomainEventGenericCallback cb,
                                          void *cbopaque,
                                          void *opaque)
{
    struct private_data *priv = opaque;

    /* Drop the lock whle dispatching, for sake of re-entrancy */
    remoteDriverUnlock(priv);
    VIR_DEBUG("Dispatch event %p %p", event, conn);
    virDomainEventDispatchDefaultFunc(conn, event, cb, cbopaque, NULL);
    remoteDriverLock(priv);
}

void
remoteDomainEventQueueFlush(int timer ATTRIBUTE_UNUSED, void *opaque)
{
    virConnectPtr conn = opaque;
    struct private_data *priv = conn->privateData;


    remoteDriverLock(priv);
    VIR_DEBUG("Event queue flush %p", conn);

    virDomainEventStateFlush(priv->domainEventState,
                             remoteDomainEventDispatchFunc,
                             priv);
    remoteDriverUnlock(priv);
}

void
remoteDomainEventQueue(struct private_data *priv, virDomainEventPtr event)
{
    virDomainEventStateQueue(priv->domainEventState, event);
}

/* get_nonnull_domain and get_nonnull_network turn an on-wire
 * (name, uuid) pair into virDomainPtr or virNetworkPtr object.
 * These can return NULL if underlying memory allocations fail,
 * but if they do then virterror_internal.has been set.
 */
static virDomainPtr
get_nonnull_domain (virConnectPtr conn, remote_nonnull_domain domain)
{
    virDomainPtr dom;
    dom = virGetDomain (conn, domain.name, BAD_CAST domain.uuid);
    if (dom) dom->id = domain.id;
    return dom;
}

static virNetworkPtr
get_nonnull_network (virConnectPtr conn, remote_nonnull_network network)
{
    return virGetNetwork (conn, network.name, BAD_CAST network.uuid);
}

static virInterfacePtr
get_nonnull_interface (virConnectPtr conn, remote_nonnull_interface iface)
{
    return virGetInterface (conn, iface.name, iface.mac);
}

static virStoragePoolPtr
get_nonnull_storage_pool (virConnectPtr conn, remote_nonnull_storage_pool pool)
{
    return virGetStoragePool (conn, pool.name, BAD_CAST pool.uuid);
}

static virStorageVolPtr
get_nonnull_storage_vol (virConnectPtr conn, remote_nonnull_storage_vol vol)
{
    return virGetStorageVol (conn, vol.pool, vol.name, vol.key);
}

static virNodeDevicePtr
get_nonnull_node_device (virConnectPtr conn, remote_nonnull_node_device dev)
{
    return virGetNodeDevice(conn, dev.name);
}

static virSecretPtr
get_nonnull_secret (virConnectPtr conn, remote_nonnull_secret secret)
{
    return virGetSecret(conn, BAD_CAST secret.uuid, secret.usageType, secret.usageID);
}

static virNWFilterPtr
get_nonnull_nwfilter (virConnectPtr conn, remote_nonnull_nwfilter nwfilter)
{
    return virGetNWFilter (conn, nwfilter.name, BAD_CAST nwfilter.uuid);
}

static virDomainSnapshotPtr
get_nonnull_domain_snapshot (virDomainPtr domain, remote_nonnull_domain_snapshot snapshot)
{
    return virGetDomainSnapshot(domain, snapshot.name);
}


/* Make remote_nonnull_domain and remote_nonnull_network. */
static void
make_nonnull_domain (remote_nonnull_domain *dom_dst, virDomainPtr dom_src)
{
    dom_dst->id = dom_src->id;
    dom_dst->name = dom_src->name;
    memcpy (dom_dst->uuid, dom_src->uuid, VIR_UUID_BUFLEN);
}

static void
make_nonnull_network (remote_nonnull_network *net_dst, virNetworkPtr net_src)
{
    net_dst->name = net_src->name;
    memcpy (net_dst->uuid, net_src->uuid, VIR_UUID_BUFLEN);
}

static void
make_nonnull_interface (remote_nonnull_interface *interface_dst,
                        virInterfacePtr interface_src)
{
    interface_dst->name = interface_src->name;
    interface_dst->mac = interface_src->mac;
}

static void
make_nonnull_storage_pool (remote_nonnull_storage_pool *pool_dst, virStoragePoolPtr pool_src)
{
    pool_dst->name = pool_src->name;
    memcpy (pool_dst->uuid, pool_src->uuid, VIR_UUID_BUFLEN);
}

static void
make_nonnull_storage_vol (remote_nonnull_storage_vol *vol_dst, virStorageVolPtr vol_src)
{
    vol_dst->pool = vol_src->pool;
    vol_dst->name = vol_src->name;
    vol_dst->key = vol_src->key;
}

static void
make_nonnull_secret (remote_nonnull_secret *secret_dst, virSecretPtr secret_src)
{
    memcpy (secret_dst->uuid, secret_src->uuid, VIR_UUID_BUFLEN);
    secret_dst->usageType = secret_src->usageType;
    secret_dst->usageID = secret_src->usageID;
}

static void
make_nonnull_nwfilter (remote_nonnull_nwfilter *nwfilter_dst, virNWFilterPtr nwfilter_src)
{
    nwfilter_dst->name = nwfilter_src->name;
    memcpy (nwfilter_dst->uuid, nwfilter_src->uuid, VIR_UUID_BUFLEN);
}

static void
make_nonnull_domain_snapshot (remote_nonnull_domain_snapshot *snapshot_dst, virDomainSnapshotPtr snapshot_src)
{
    snapshot_dst->name = snapshot_src->name;
    make_nonnull_domain(&snapshot_dst->dom, snapshot_src->domain);
}

/*----------------------------------------------------------------------*/

unsigned long remoteVersion(void)
{
    return REMOTE_PROTOCOL_VERSION;
}

static virDriver remote_driver = {
    .no = VIR_DRV_REMOTE,
    .name = "remote",
    .open = remoteOpen, /* 0.3.0 */
    .close = remoteClose, /* 0.3.0 */
    .supports_feature = remoteSupportsFeature, /* 0.3.0 */
    .type = remoteType, /* 0.3.0 */
    .version = remoteGetVersion, /* 0.3.0 */
    .libvirtVersion = remoteGetLibVersion, /* 0.7.3 */
    .getHostname = remoteGetHostname, /* 0.3.0 */
    .getSysinfo = remoteGetSysinfo, /* 0.8.8 */
    .getMaxVcpus = remoteGetMaxVcpus, /* 0.3.0 */
    .nodeGetInfo = remoteNodeGetInfo, /* 0.3.0 */
    .getCapabilities = remoteGetCapabilities, /* 0.3.0 */
    .listDomains = remoteListDomains, /* 0.3.0 */
    .numOfDomains = remoteNumOfDomains, /* 0.3.0 */
    .domainCreateXML = remoteDomainCreateXML, /* 0.3.0 */
    .domainLookupByID = remoteDomainLookupByID, /* 0.3.0 */
    .domainLookupByUUID = remoteDomainLookupByUUID, /* 0.3.0 */
    .domainLookupByName = remoteDomainLookupByName, /* 0.3.0 */
    .domainSuspend = remoteDomainSuspend, /* 0.3.0 */
    .domainResume = remoteDomainResume, /* 0.3.0 */
    .domainShutdown = remoteDomainShutdown, /* 0.3.0 */
    .domainReboot = remoteDomainReboot, /* 0.3.0 */
    .domainDestroy = remoteDomainDestroy, /* 0.3.0 */
    .domainGetOSType = remoteDomainGetOSType, /* 0.3.0 */
    .domainGetMaxMemory = remoteDomainGetMaxMemory, /* 0.3.0 */
    .domainSetMaxMemory = remoteDomainSetMaxMemory, /* 0.3.0 */
    .domainSetMemory = remoteDomainSetMemory, /* 0.3.0 */
    .domainSetMemoryFlags = remoteDomainSetMemoryFlags, /* 0.9.0 */
    .domainSetMemoryParameters = remoteDomainSetMemoryParameters, /* 0.8.5 */
    .domainGetMemoryParameters = remoteDomainGetMemoryParameters, /* 0.8.5 */
    .domainSetBlkioParameters = remoteDomainSetBlkioParameters, /* 0.9.0 */
    .domainGetBlkioParameters = remoteDomainGetBlkioParameters, /* 0.9.0 */
    .domainGetInfo = remoteDomainGetInfo, /* 0.3.0 */
    .domainGetState = remoteDomainGetState, /* 0.9.2 */
    .domainSave = remoteDomainSave, /* 0.3.0 */
    .domainRestore = remoteDomainRestore, /* 0.3.0 */
    .domainCoreDump = remoteDomainCoreDump, /* 0.3.0 */
    .domainScreenshot = remoteDomainScreenshot, /* 0.9.2 */
    .domainSetVcpus = remoteDomainSetVcpus, /* 0.3.0 */
    .domainSetVcpusFlags = remoteDomainSetVcpusFlags, /* 0.8.5 */
    .domainGetVcpusFlags = remoteDomainGetVcpusFlags, /* 0.8.5 */
    .domainPinVcpu = remoteDomainPinVcpu, /* 0.3.0 */
    .domainGetVcpus = remoteDomainGetVcpus, /* 0.3.0 */
    .domainGetMaxVcpus = remoteDomainGetMaxVcpus, /* 0.3.0 */
    .domainGetSecurityLabel = remoteDomainGetSecurityLabel, /* 0.6.1 */
    .nodeGetSecurityModel = remoteNodeGetSecurityModel, /* 0.6.1 */
    .domainGetXMLDesc = remoteDomainGetXMLDesc, /* 0.3.0 */
    .domainXMLFromNative = remoteDomainXMLFromNative, /* 0.6.4 */
    .domainXMLToNative = remoteDomainXMLToNative, /* 0.6.4 */
    .listDefinedDomains = remoteListDefinedDomains, /* 0.3.0 */
    .numOfDefinedDomains = remoteNumOfDefinedDomains, /* 0.3.0 */
    .domainCreate = remoteDomainCreate, /* 0.3.0 */
    .domainCreateWithFlags = remoteDomainCreateWithFlags, /* 0.8.2 */
    .domainDefineXML = remoteDomainDefineXML, /* 0.3.0 */
    .domainUndefine = remoteDomainUndefine, /* 0.3.0 */
    .domainAttachDevice = remoteDomainAttachDevice, /* 0.3.0 */
    .domainAttachDeviceFlags = remoteDomainAttachDeviceFlags, /* 0.7.7 */
    .domainDetachDevice = remoteDomainDetachDevice, /* 0.3.0 */
    .domainDetachDeviceFlags = remoteDomainDetachDeviceFlags, /* 0.7.7 */
    .domainUpdateDeviceFlags = remoteDomainUpdateDeviceFlags, /* 0.8.0 */
    .domainGetAutostart = remoteDomainGetAutostart, /* 0.3.0 */
    .domainSetAutostart = remoteDomainSetAutostart, /* 0.3.0 */
    .domainGetSchedulerType = remoteDomainGetSchedulerType, /* 0.3.0 */
    .domainGetSchedulerParameters = remoteDomainGetSchedulerParameters, /* 0.3.0 */
    .domainSetSchedulerParameters = remoteDomainSetSchedulerParameters, /* 0.3.0 */
    .domainMigratePrepare = remoteDomainMigratePrepare, /* 0.3.2 */
    .domainMigratePerform = remoteDomainMigratePerform, /* 0.3.2 */
    .domainMigrateFinish = remoteDomainMigrateFinish, /* 0.3.2 */
    .domainBlockStats = remoteDomainBlockStats, /* 0.3.2 */
    .domainInterfaceStats = remoteDomainInterfaceStats, /* 0.3.2 */
    .domainMemoryStats = remoteDomainMemoryStats, /* 0.7.5 */
    .domainBlockPeek = remoteDomainBlockPeek, /* 0.4.2 */
    .domainMemoryPeek = remoteDomainMemoryPeek, /* 0.4.2 */
    .domainGetBlockInfo = remoteDomainGetBlockInfo, /* 0.8.1 */
    .nodeGetCellsFreeMemory = remoteNodeGetCellsFreeMemory, /* 0.3.3 */
    .nodeGetFreeMemory = remoteNodeGetFreeMemory, /* 0.3.3 */
    .domainEventRegister = remoteDomainEventRegister, /* 0.5.0 */
    .domainEventDeregister = remoteDomainEventDeregister, /* 0.5.0 */
    .domainMigratePrepare2 = remoteDomainMigratePrepare2, /* 0.5.0 */
    .domainMigrateFinish2 = remoteDomainMigrateFinish2, /* 0.5.0 */
    .nodeDeviceDettach = remoteNodeDeviceDettach, /* 0.6.1 */
    .nodeDeviceReAttach = remoteNodeDeviceReAttach, /* 0.6.1 */
    .nodeDeviceReset = remoteNodeDeviceReset, /* 0.6.1 */
    .domainMigratePrepareTunnel = remoteDomainMigratePrepareTunnel, /* 0.7.2 */
    .isEncrypted = remoteIsEncrypted, /* 0.7.3 */
    .isSecure = remoteIsSecure, /* 0.7.3 */
    .domainIsActive = remoteDomainIsActive, /* 0.7.3 */
    .domainIsPersistent = remoteDomainIsPersistent, /* 0.7.3 */
    .domainIsUpdated = remoteDomainIsUpdated, /* 0.8.6 */
    .cpuCompare = remoteCPUCompare, /* 0.7.5 */
    .cpuBaseline = remoteCPUBaseline, /* 0.7.7 */
    .domainGetJobInfo = remoteDomainGetJobInfo, /* 0.7.7 */
    .domainAbortJob = remoteDomainAbortJob, /* 0.7.7 */
    .domainMigrateSetMaxDowntime = remoteDomainMigrateSetMaxDowntime, /* 0.8.0 */
    .domainMigrateSetMaxSpeed = remoteDomainMigrateSetMaxSpeed, /* 0.9.0 */
    .domainEventRegisterAny = remoteDomainEventRegisterAny, /* 0.8.0 */
    .domainEventDeregisterAny = remoteDomainEventDeregisterAny, /* 0.8.0 */
    .domainManagedSave = remoteDomainManagedSave, /* 0.8.0 */
    .domainHasManagedSaveImage = remoteDomainHasManagedSaveImage, /* 0.8.0 */
    .domainManagedSaveRemove = remoteDomainManagedSaveRemove, /* 0.8.0 */
    .domainSnapshotCreateXML = remoteDomainSnapshotCreateXML, /* 0.8.0 */
    .domainSnapshotGetXMLDesc = remoteDomainSnapshotGetXMLDesc, /* 0.8.0 */
    .domainSnapshotNum = remoteDomainSnapshotNum, /* 0.8.0 */
    .domainSnapshotListNames = remoteDomainSnapshotListNames, /* 0.8.0 */
    .domainSnapshotLookupByName = remoteDomainSnapshotLookupByName, /* 0.8.0 */
    .domainHasCurrentSnapshot = remoteDomainHasCurrentSnapshot, /* 0.8.0 */
    .domainSnapshotCurrent = remoteDomainSnapshotCurrent, /* 0.8.0 */
    .domainRevertToSnapshot = remoteDomainRevertToSnapshot, /* 0.8.0 */
    .domainSnapshotDelete = remoteDomainSnapshotDelete, /* 0.8.0 */
    .qemuDomainMonitorCommand = remoteQemuDomainMonitorCommand, /* 0.8.3 */
    .domainOpenConsole = remoteDomainOpenConsole, /* 0.8.6 */
    .domainInjectNMI = remoteDomainInjectNMI, /* 0.9.2 */
    .domainMigrateBegin3 = remoteDomainMigrateBegin3, /* 0.9.2 */
    .domainMigratePrepare3 = remoteDomainMigratePrepare3, /* 0.9.2 */
    .domainMigratePrepareTunnel3 = remoteDomainMigratePrepareTunnel3, /* 0.9.2 */
    .domainMigratePerform3 = remoteDomainMigratePerform3, /* 0.9.2 */
    .domainMigrateFinish3 = remoteDomainMigrateFinish3, /* 0.9.2 */
    .domainMigrateConfirm3 = remoteDomainMigrateConfirm3, /* 0.9.2 */
    .domainSetSchedulerParametersFlags = remoteDomainSetSchedulerParametersFlags, /* 0.9.2 */
};

static virNetworkDriver network_driver = {
    .name = "remote",
    .open = remoteNetworkOpen, /* 0.3.0 */
    .close = remoteNetworkClose, /* 0.3.0 */
    .numOfNetworks = remoteNumOfNetworks, /* 0.3.0 */
    .listNetworks = remoteListNetworks, /* 0.3.0 */
    .numOfDefinedNetworks = remoteNumOfDefinedNetworks, /* 0.3.0 */
    .listDefinedNetworks = remoteListDefinedNetworks, /* 0.3.0 */
    .networkLookupByUUID = remoteNetworkLookupByUUID, /* 0.3.0 */
    .networkLookupByName = remoteNetworkLookupByName, /* 0.3.0 */
    .networkCreateXML = remoteNetworkCreateXML, /* 0.3.0 */
    .networkDefineXML = remoteNetworkDefineXML, /* 0.3.0 */
    .networkUndefine = remoteNetworkUndefine, /* 0.3.0 */
    .networkCreate = remoteNetworkCreate, /* 0.3.0 */
    .networkDestroy = remoteNetworkDestroy, /* 0.3.0 */
    .networkGetXMLDesc = remoteNetworkGetXMLDesc, /* 0.3.0 */
    .networkGetBridgeName = remoteNetworkGetBridgeName, /* 0.3.0 */
    .networkGetAutostart = remoteNetworkGetAutostart, /* 0.3.0 */
    .networkSetAutostart = remoteNetworkSetAutostart, /* 0.3.0 */
    .networkIsActive = remoteNetworkIsActive, /* 0.7.3 */
    .networkIsPersistent = remoteNetworkIsPersistent, /* 0.7.3 */
};

static virInterfaceDriver interface_driver = {
    .name = "remote",
    .open = remoteInterfaceOpen, /* 0.7.2 */
    .close = remoteInterfaceClose, /* 0.7.2 */
    .numOfInterfaces = remoteNumOfInterfaces, /* 0.7.2 */
    .listInterfaces = remoteListInterfaces, /* 0.7.2 */
    .numOfDefinedInterfaces = remoteNumOfDefinedInterfaces, /* 0.7.2 */
    .listDefinedInterfaces = remoteListDefinedInterfaces, /* 0.7.2 */
    .interfaceLookupByName = remoteInterfaceLookupByName, /* 0.7.2 */
    .interfaceLookupByMACString = remoteInterfaceLookupByMACString, /* 0.7.2 */
    .interfaceGetXMLDesc = remoteInterfaceGetXMLDesc, /* 0.7.2 */
    .interfaceDefineXML = remoteInterfaceDefineXML, /* 0.7.2 */
    .interfaceUndefine = remoteInterfaceUndefine, /* 0.7.2 */
    .interfaceCreate = remoteInterfaceCreate, /* 0.7.2 */
    .interfaceDestroy = remoteInterfaceDestroy, /* 0.7.2 */
    .interfaceIsActive = remoteInterfaceIsActive, /* 0.7.3 */
    .interfaceChangeBegin = remoteInterfaceChangeBegin, /* 0.9.2 */
    .interfaceChangeCommit = remoteInterfaceChangeCommit, /* 0.9.2 */
    .interfaceChangeRollback = remoteInterfaceChangeRollback, /* 0.9.2 */
};

static virStorageDriver storage_driver = {
    .name = "remote",
    .open = remoteStorageOpen, /* 0.4.1 */
    .close = remoteStorageClose, /* 0.4.1 */
    .numOfPools = remoteNumOfStoragePools, /* 0.4.1 */
    .listPools = remoteListStoragePools, /* 0.4.1 */
    .numOfDefinedPools = remoteNumOfDefinedStoragePools, /* 0.4.1 */
    .listDefinedPools = remoteListDefinedStoragePools, /* 0.4.1 */
    .findPoolSources = remoteFindStoragePoolSources, /* 0.4.5 */
    .poolLookupByName = remoteStoragePoolLookupByName, /* 0.4.1 */
    .poolLookupByUUID = remoteStoragePoolLookupByUUID, /* 0.4.1 */
    .poolLookupByVolume = remoteStoragePoolLookupByVolume, /* 0.4.1 */
    .poolCreateXML = remoteStoragePoolCreateXML, /* 0.4.1 */
    .poolDefineXML = remoteStoragePoolDefineXML, /* 0.4.1 */
    .poolBuild = remoteStoragePoolBuild, /* 0.4.1 */
    .poolUndefine = remoteStoragePoolUndefine, /* 0.4.1 */
    .poolCreate = remoteStoragePoolCreate, /* 0.4.1 */
    .poolDestroy = remoteStoragePoolDestroy, /* 0.4.1 */
    .poolDelete = remoteStoragePoolDelete, /* 0.4.1 */
    .poolRefresh = remoteStoragePoolRefresh, /* 0.4.1 */
    .poolGetInfo = remoteStoragePoolGetInfo, /* 0.4.1 */
    .poolGetXMLDesc = remoteStoragePoolGetXMLDesc, /* 0.4.1 */
    .poolGetAutostart = remoteStoragePoolGetAutostart, /* 0.4.1 */
    .poolSetAutostart = remoteStoragePoolSetAutostart, /* 0.4.1 */
    .poolNumOfVolumes = remoteStoragePoolNumOfVolumes, /* 0.4.1 */
    .poolListVolumes = remoteStoragePoolListVolumes, /* 0.4.1 */

    .volLookupByName = remoteStorageVolLookupByName, /* 0.4.1 */
    .volLookupByKey = remoteStorageVolLookupByKey, /* 0.4.1 */
    .volLookupByPath = remoteStorageVolLookupByPath, /* 0.4.1 */
    .volCreateXML = remoteStorageVolCreateXML, /* 0.4.1 */
    .volCreateXMLFrom = remoteStorageVolCreateXMLFrom, /* 0.6.4 */
    .volDownload = remoteStorageVolDownload, /* 0.9.0 */
    .volUpload = remoteStorageVolUpload, /* 0.9.0 */
    .volDelete = remoteStorageVolDelete, /* 0.4.1 */
    .volWipe = remoteStorageVolWipe, /* 0.8.0 */
    .volGetInfo = remoteStorageVolGetInfo, /* 0.4.1 */
    .volGetXMLDesc = remoteStorageVolGetXMLDesc, /* 0.4.1 */
    .volGetPath = remoteStorageVolGetPath, /* 0.4.1 */
    .poolIsActive = remoteStoragePoolIsActive, /* 0.7.3 */
    .poolIsPersistent = remoteStoragePoolIsPersistent, /* 0.7.3 */
};

static virSecretDriver secret_driver = {
    .name = "remote",
    .open = remoteSecretOpen, /* 0.7.1 */
    .close = remoteSecretClose, /* 0.7.1 */
    .numOfSecrets = remoteNumOfSecrets, /* 0.7.1 */
    .listSecrets = remoteListSecrets, /* 0.7.1 */
    .lookupByUUID = remoteSecretLookupByUUID, /* 0.7.1 */
    .lookupByUsage = remoteSecretLookupByUsage, /* 0.7.1 */
    .defineXML = remoteSecretDefineXML, /* 0.7.1 */
    .getXMLDesc = remoteSecretGetXMLDesc, /* 0.7.1 */
    .setValue = remoteSecretSetValue, /* 0.7.1 */
    .getValue = remoteSecretGetValue, /* 0.7.1 */
    .undefine = remoteSecretUndefine /* 0.7.1 */
};

static virDeviceMonitor dev_monitor = {
    .name = "remote",
    .open = remoteDevMonOpen, /* 0.5.0 */
    .close = remoteDevMonClose, /* 0.5.0 */
    .numOfDevices = remoteNodeNumOfDevices, /* 0.5.0 */
    .listDevices = remoteNodeListDevices, /* 0.5.0 */
    .deviceLookupByName = remoteNodeDeviceLookupByName, /* 0.5.0 */
    .deviceGetXMLDesc = remoteNodeDeviceGetXMLDesc, /* 0.5.0 */
    .deviceGetParent = remoteNodeDeviceGetParent, /* 0.5.0 */
    .deviceNumOfCaps = remoteNodeDeviceNumOfCaps, /* 0.5.0 */
    .deviceListCaps = remoteNodeDeviceListCaps, /* 0.5.0 */
    .deviceCreateXML = remoteNodeDeviceCreateXML, /* 0.6.3 */
    .deviceDestroy = remoteNodeDeviceDestroy /* 0.6.3 */
};

static virNWFilterDriver nwfilter_driver = {
    .name = "remote",
    .open = remoteNWFilterOpen, /* 0.8.0 */
    .close = remoteNWFilterClose, /* 0.8.0 */
    .nwfilterLookupByUUID = remoteNWFilterLookupByUUID, /* 0.8.0 */
    .nwfilterLookupByName = remoteNWFilterLookupByName, /* 0.8.0 */
    .getXMLDesc           = remoteNWFilterGetXMLDesc, /* 0.8.0 */
    .defineXML            = remoteNWFilterDefineXML, /* 0.8.0 */
    .undefine             = remoteNWFilterUndefine, /* 0.8.0 */
    .numOfNWFilters       = remoteNumOfNWFilters, /* 0.8.0 */
    .listNWFilters        = remoteListNWFilters, /* 0.8.0 */
};


#ifdef WITH_LIBVIRTD
static virStateDriver state_driver = {
    .name = "Remote",
    .initialize = remoteStartup,
};
#endif


/** remoteRegister:
 *
 * Register driver with libvirt driver system.
 *
 * Returns -1 on error.
 */
int
remoteRegister (void)
{
    if (virRegisterDriver (&remote_driver) == -1) return -1;
    if (virRegisterNetworkDriver (&network_driver) == -1) return -1;
    if (virRegisterInterfaceDriver (&interface_driver) == -1) return -1;
    if (virRegisterStorageDriver (&storage_driver) == -1) return -1;
    if (virRegisterDeviceMonitor (&dev_monitor) == -1) return -1;
    if (virRegisterSecretDriver (&secret_driver) == -1) return -1;
    if (virRegisterNWFilterDriver(&nwfilter_driver) == -1) return -1;
#ifdef WITH_LIBVIRTD
    if (virRegisterStateDriver (&state_driver) == -1) return -1;
#endif

    return 0;
}
