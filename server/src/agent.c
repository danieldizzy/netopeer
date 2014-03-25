/**
 * @file agent.c
 * @author David Kupka <xkupka01@stud.fit.vutbr.cz>
 * @brief NETCONF agent. Starts as ssh subsystem, performs handshake and passes
 * messages between server and client.
 *
 * Copyright (c) 2011, CESNET, z.s.p.o.
 * All rights reserved.
 *
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name of the CESNET, z.s.p.o. nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/time.h>
#include <libgen.h>
#include <errno.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <pthread.h>
#include <syslog.h>
#include <stdint.h>
#include <string.h>

#include <libxml/tree.h>
#include <libxml/HTMLtree.h>

#ifdef ENABLE_TLS
#	include <openssl/x509.h>
#	include <openssl/pem.h>
#endif

#include <libnetconf_xml.h>
#include <libnetconf_tls.h>

#include "common.c"
#include "comm.h"

volatile int done = 0;

typedef int model_t;

struct ntf_thread_config {
	struct nc_session *session;
	nc_rpc *subscribe_rpc;
};

static void* notification_thread(void* arg)
{
	struct ntf_thread_config *config = (struct ntf_thread_config*)arg;

	ncntf_dispatch_send(config->session, config->subscribe_rpc);
	nc_rpc_free(config->subscribe_rpc);
	free(config);

	return (NULL);
}

/*!
 * \brief Signal handler
 *
 * Handles received UNIX signals and sets value to control main loop
 *
 * \param sig 	signal number
 */
void signal_handler(int sig)
{
	clb_print(NC_VERB_VERBOSE, "Signal received.");

	fprintf(stderr, "Signal %d received.\n", sig);

	switch (sig) {
	case SIGINT:
	case SIGTERM:
	case SIGQUIT:
	case SIGABRT:
	case SIGKILL:
		if (done == 0) {
			/* first attempt */
			done = 1;
		} else {
			/* second attempt */
			clb_print(NC_VERB_ERROR, "Hey! I need some time to stop, be patient next time!");
			exit(EXIT_FAILURE);
		}
		break;
	default:
		clb_print(NC_VERB_ERROR, "exiting on signal.");
		exit(EXIT_FAILURE);
		break;
	}
}

static struct nc_cpblts* get_server_capabilities(conn_t* conn)
{
	struct nc_cpblts* srv_cpblts;
	char **cpblts_list = NULL;
	int i;

	if ((cpblts_list = comm_get_srv_cpblts(conn)) == NULL) {
		clb_print(NC_VERB_ERROR, "Cannot get server capabilities!");
		return (NULL);
	}

	/* Fill server capabilities structure */
	srv_cpblts = nc_cpblts_new((const char* const*)cpblts_list);

	/* cleanup */
	for (i = 0; cpblts_list != NULL && cpblts_list[i] != NULL; i++) {
		free(cpblts_list[i]);
	}
	free(cpblts_list);

	return srv_cpblts;
}

int process_message(struct nc_session *session, conn_t *conn, const nc_rpc *rpc)
{
	nc_reply * reply = NULL;
	struct nc_err * err;
	pthread_t thread;
	struct ntf_thread_config * ntf_config;
	xmlNodePtr op;
	char * sid;

	if (rpc == NULL) {
		nc_verb_error("Invalid RPC to process.");
		return (EXIT_FAILURE);
	}

	/* close-session message */
	switch (nc_rpc_get_op(rpc)) {
	case NC_OP_CLOSESESSION:
		if (comm_close(conn) != EXIT_SUCCESS) {
			err = nc_err_new(NC_ERR_OP_FAILED);
			reply = nc_reply_error(err);
		} else {
			reply = nc_reply_ok();
		}
		done = 1;
		break;
	case NC_OP_KILLSESSION:
		if ((op = ncxml_rpc_get_op_content(rpc)) == NULL || op->name == NULL ||
				xmlStrEqual(op->name, BAD_CAST "kill-session") == 0) {
			clb_print(NC_VERB_ERROR, "Corrupted RPC message.");
			reply = nc_reply_error(nc_err_new(NC_ERR_OP_FAILED));
			goto send_reply;
		}
		if (op->children == NULL || xmlStrEqual(op->children->name, BAD_CAST "session-id") == 0) {
			clb_print(NC_VERB_ERROR, "No session id found.");
			err = nc_err_new(NC_ERR_MISSING_ELEM);
			nc_err_set(err, NC_ERR_PARAM_INFO_BADELEM, "session-id");
			reply = nc_reply_error(err);
			goto send_reply;
		}
		sid = (char *)xmlNodeGetContent(op->children);
		reply = comm_kill_session(conn, sid);
		free(sid);
		break;
	case NC_OP_CREATESUBSCRIPTION:
		/* create-subscription message */
		if (nc_cpblts_enabled(session, "urn:ietf:params:netconf:capability:notification:1.0") == 0) {
			reply = nc_reply_error(nc_err_new(NC_ERR_OP_NOT_SUPPORTED));
			goto send_reply;
		}

		/* check if notifications are allowed on this session */
		if (nc_session_notif_allowed(session) == 0) {
			clb_print(NC_VERB_ERROR, "Notification subscription is not allowed on this session.");
			err = nc_err_new(NC_ERR_OP_FAILED);
			nc_err_set(err, NC_ERR_PARAM_TYPE, "protocol");
			nc_err_set(err, NC_ERR_PARAM_MSG, "Another notification subscription is currently active on this session.");
			reply = nc_reply_error(err);
			goto send_reply;
		}

		reply = ncntf_subscription_check(rpc);
		if (nc_reply_get_type(reply) != NC_REPLY_OK) {
			goto send_reply;
		}

		if ((ntf_config = malloc(sizeof(struct ntf_thread_config))) == NULL) {
			clb_print(NC_VERB_ERROR, "Memory allocation failed.");
			err = nc_err_new(NC_ERR_OP_FAILED);
			nc_err_set(err, NC_ERR_PARAM_MSG, "Memory allocation failed.");
			reply = nc_reply_error(err);
			err = NULL;
			goto send_reply;
		}
		ntf_config->session = (struct nc_session*)session;
		ntf_config->subscribe_rpc = nc_rpc_dup(rpc);

		/* perform notification sending */
		if ((pthread_create(&thread, NULL, notification_thread, ntf_config)) != 0) {
			nc_reply_free(reply);
			err = nc_err_new(NC_ERR_OP_FAILED);
			nc_err_set(err, NC_ERR_PARAM_MSG, "Creating thread for sending Notifications failed.");
			reply = nc_reply_error(err);
			err = NULL;
			goto send_reply;
		}
		pthread_detach(thread);
		break;
	default:
		/* other messages */
		reply = comm_operation(conn, rpc);
		break;
	}

send_reply:
	nc_session_send_reply(session, rpc, reply);
	nc_reply_free(reply);
	return EXIT_SUCCESS;
}

#ifdef ENABLE_TLS
X509 *get_cert(void)
{
	X509 *cert;
	FILE *certf;
	char *auxstr;
	const EVP_MD *fprint_type = NULL;
	unsigned int i, fprint_size;
	unsigned char fprint[EVP_MAX_MD_SIZE];

	auxstr = getenv("SSL_CLIENT_CERT");
	if (!auxstr) {
		/* we have no info about certificate location */
		return (NULL);
	}

	certf = fopen(auxstr, "r");
	if (!certf) {
		clb_print(NC_VERB_ERROR, "Unable to open client certificate file.");
		clb_print(NC_VERB_ERROR, strerror(errno));
		return (NULL);
	}
	unlink(auxstr);

	cert = PEM_read_X509_AUX(certf, NULL, NULL, NULL );
	fclose(certf);

	fprint_type = EVP_sha1();
	X509_digest(cert, fprint_type, fprint, &fprint_size);
	auxstr = malloc((fprint_size + 1) * 3);
	/* According to ietf-x509-cert-to-name YANG data model, 1st
	 * octet of the fingerprint value contains hashing algorithm
	 * identifier from [1]. 02 means SHA1
	 *
	 * [1] http://www.iana.org/assignments/tls-parameters/tls-parameters.xhtml#tls-parameters-18
	 */
	snprintf(&(auxstr[0]), 4, "02:");
	for (i = 0; i < fprint_size; ++i) {
		snprintf(&(auxstr[i * 3]), 4, "%02x:", fprint[i]);
	}
	auxstr[(fprint_size * 3) - 1] = '\0';
	clb_print(NC_VERB_ERROR, auxstr);
	free(auxstr);
	auxstr = NULL;

	return (cert);
}
#endif /* ENABLE_TLS */

int main()
{
	conn_t *con;
	struct nc_session * netconf_con;
	nc_rpc * rpc = NULL;
	struct nc_cpblts * capabilities = NULL;
	int ret;
	NC_MSG_TYPE rpc_type;
	int timeout = 500; /* ms, poll timeout */
	struct pollfd fds;
	struct sigaction action;
#ifdef ENABLE_TLS
	struct passwd *pw;
	X509 *cert;
#endif

	/* set signal handler */
	sigfillset(&action.sa_mask);
	action.sa_handler = signal_handler;
	action.sa_flags = 0;
	sigaction(SIGINT, &action, NULL );
	sigaction(SIGQUIT, &action, NULL );
	sigaction(SIGABRT, &action, NULL );
	sigaction(SIGTERM, &action, NULL );
	sigaction(SIGKILL, &action, NULL );

#ifdef DEBUG
	nc_verbosity(NC_VERB_DEBUG);
#endif
	openlog("netopeer-agent", LOG_PID, LOG_DAEMON);
	nc_callback_print(clb_print);

	/* initialize library */
	if (nc_init(NC_INIT_ALL) < 0) {
		clb_print(NC_VERB_ERROR, "Library initialization failed");
		return EXIT_FAILURE;
	}

	/* connect to server */
	if ((con = comm_connect()) == NULL) {
		clb_print(NC_VERB_ERROR, "Cannot connect to Netopeer server.");
		return EXIT_FAILURE;
	}
	clb_print(NC_VERB_VERBOSE, "Connected with Netopeer server");

	/* get server capabilities */
	if ((capabilities = get_server_capabilities(con)) == NULL) {
		clb_print(NC_VERB_ERROR, "Cannot get server capabilities.");
		return EXIT_FAILURE;
	}

#ifdef ENABLE_TLS
	/*
	 * Are we running with the TLS transport? If yes, the TLS server shoudl
	 * provide SSL_CLIENT_DN environment variable for us.
	 */
	if (getenv("SSL_CLIENT_DN")) {
		/* try to get client certificate from stunnel */
		cert = get_cert();

		/* accept client session and handle capabilities */
		netconf_con = nc_session_accept_tls(capabilities, cert);
		nc_cpblts_free(capabilities);
		X509_free(cert);
	} else {
#else
	{
#endif
		/* there is probably SSH transport */
		netconf_con = nc_session_accept(capabilities);
		nc_cpblts_free(capabilities);
	}
	if (netconf_con == NULL) {
		clb_print(NC_VERB_ERROR, "Failed to connect agent.");
		return EXIT_FAILURE;
	}

#ifdef ENABLE_TLS
	/* switch user if possible/needed */
	/*
	 * OpenSSH (sshd) does this automatically, but TLS server (stunnel) does not,
	 * so in case of SSH transport, we already have different UID, in case of
	 * TLS transport, we are going to try to switch UID if we can
	 */
	if (getuid() == 0) {
		/* we are going to drop privileges forever */
		pw = getpwnam(nc_session_get_user(netconf_con));
		if (pw) {
			setuid(pw->pw_uid);
		}

		/*
		 * if this part fails, we still can continue as user 0 - username is
		 * stored in the NETCONF session information and all actions are (should
		 * be) taken according to this value.
		 */
	}
#endif

	/* monitor this session and build statistics */
	nc_session_monitor(netconf_con);

	if (comm_session_info(con, netconf_con)) {
		clb_print(NC_VERB_ERROR, "Failed to comunicate with server.");
		return EXIT_FAILURE;
	}

	clb_print(NC_VERB_VERBOSE, "Handshake finished");

	fds.fd = nc_session_get_eventfd(netconf_con);
	fds.events = POLLIN;

	while (!done) {
		ret = poll(&fds, 1, timeout);
		if (ret < 0 && errno != EINTR) { /* poll error */
			clb_print(NC_VERB_ERROR, "poll failed.");
			goto cleanup;
		} else if (ret == 0) { /* timeout */
			continue;
		} else if (ret > 0) { /* event occured */
			if (fds.revents & POLLHUP) { /* client hung up */
				clb_print(NC_VERB_VERBOSE, "Connection closed by client");
				comm_close(con);
				goto cleanup;
			} else if (fds.revents & POLLERR) { /* I/O error */
				clb_print(NC_VERB_ERROR, "I/O error.");
				goto cleanup;
			} else if (fds.revents & POLLIN) { /* data ready */
				/* read data from input */
				rpc_type = nc_session_recv_rpc(netconf_con, -1, &rpc);
				if (rpc_type != NC_MSG_RPC) {
					switch (rpc_type) {
					case NC_MSG_NONE:
						/* the request was already processed by libnetconf or no message available */
						/* continue in main while loop */
						break;
					case NC_MSG_UNKNOWN:
						if (nc_session_get_status(netconf_con) != NC_SESSION_STATUS_WORKING) {
							/* something really bad happened, and communication is not possible anymore */
							clb_print(NC_VERB_ERROR, "Failed to receive clinets message");
							goto cleanup;
						}
						/* continue in main while loop */
						break;
					default:
						/* continue in main while loop */
						break;
					}
				} else {
					clb_print(NC_VERB_VERBOSE, "Processing client message");
					if (process_message(netconf_con, con, rpc) != EXIT_SUCCESS) {
						clb_print(NC_VERB_WARNING, "Message processing failed");
					}
					nc_rpc_free(rpc);
					rpc = NULL;
				}
			}
		}
	}

cleanup:
	nc_rpc_free(rpc);
	nc_session_free(netconf_con);
	nc_close(0);

	return (EXIT_SUCCESS);
}

