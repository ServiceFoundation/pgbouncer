/*
 * PgBouncer - Lightweight connection pooler for PostgreSQL.
 * 
 * Copyright (c) 2007-2009  Marko Kreen, Skype Technologies O脺
 * 
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/*
 * Handling of server connections
 */

#include "bouncer.h"

static bool load_parameter(PgSocket *server, PktHdr *pkt, bool startup)
{
	const char *key, *val;
	PgSocket *client = server->link;

	/*
	 * Want to see complete packet.  That means SMALL_PKT
	 * in sbuf.c must be larger than max param pkt.
	 */
	if (incomplete_pkt(pkt)) {
		log_debug("receive one incomplete package");
		return false;
	}

	if (!mbuf_get_string(&pkt->data, &key))
		goto failed;
	if (!mbuf_get_string(&pkt->data, &val))
		goto failed;
	slog_debug(server, "S: param: %s = %s", key, val);

	varcache_set(&server->vars, key, val);

	if (client) {
		varcache_set(&client->vars, key, val);
	}

	if (startup) {
		if (!add_welcome_parameter(server->pool, key, val))
			goto failed_store;
	}

	return true;
failed:
	release_disconn_pgSocket(server, SV_DISCONN, true, "broken ParameterStatus packet");
	return false;
failed_store:
	release_disconn_pgSocket(server, SV_DISCONN, true, "failed to store ParameterStatus");
	return false;
}

/* we cannot log in at all, notify clients */
static void kill_pool_logins(PgPool *pool, PktHdr *errpkt)
{
	const char *level, *msg;

	parse_server_error(errpkt, &level, &msg);

	log_warning("server login failed: %s %s", level, msg);
}

/* process packets on server auth phase */
static bool handle_server_startup(PgSocket *server, PktHdr *pkt)
{
	SBuf *sbuf = &server->sbuf;
	bool res = false;
	const uint8_t *ckey;

	if (incomplete_pkt(pkt)) {
		release_disconn_pgSocket(server, SV_DISCONN, true, "partial pkt in login phase");
		return false;
	}

	/* ignore most that happens during connect_query */
	if (server->exec_on_connect) {
		switch (pkt->type) {
		case 'Z'://ReadyForQuery
		case 'S'://ParameterStatus/* handle them below */
			break;

		case 'E':	/* log & ignore errors */
			sbuf_prepare_skip(sbuf, pkt->len);
			return true;
		default:	/* ignore rest */
			sbuf_prepare_skip(sbuf, pkt->len);
			return true;
		}
	}

	switch (pkt->type) {
	default:
		slog_error(server, "unknown pkt from server: '%c'", pkt_desc(pkt));
		release_disconn_pgSocket(server, SV_DISCONN, true, "unknown pkt from server");
		break;

	case 'E':		/* ErrorResponse */
		if (!server->pool->welcome_msg_ready)
			kill_pool_logins(server->pool, pkt);
		else
			log_server_error("S: login failed", pkt);

		release_disconn_pgSocket(server, SV_DISCONN, true, "login failed");
		break;

	/* packets that need closer look */
	case 'R':		/* AuthenticationXXX */
		slog_debug(server, "calling login_answer");
		res = answer_authreq(server, pkt);
		if (!res)
			release_disconn_pgSocket(server, SV_DISCONN, false, "failed to answer authreq");
		break;

	case 'S':		/* ParameterStatus */
		//res = load_parameter(server, pkt, true);
		res = true;
		break;

	case 'Z':		/* ReadyForQuery */
		if (server->exec_on_connect) {
			server->exec_on_connect = 0;
			/* deliberately ignore transaction status */
		} else if (server->pool->db->connect_query) {
			server->exec_on_connect = 1;
			slog_debug(server, "server conect ok, send exec_on_connect");
			SEND_generic(res, server, 'Q', "s", server->pool->db->connect_query);
			if (!res)
				release_disconn_pgSocket(server, SV_DISCONN, false, "exec_on_connect query failed");
			break;
		}

		/* login ok */
		slog_debug(server, "server login ok, start accepting queries");
		server->ready = 1;

		/* got all params */
		finish_welcome_msg(server);

		/* need to notify sbuf if server was closed */
		res = release_server(server);

		/* let the takeover process handle it */
		if (res && server->pool->db->admin) {
			res = takeover_login(server);
		}
		break;

	/* ignorable packets */
	case 'K':		/* BackendKeyData */
		if (!mbuf_get_bytes(&pkt->data, BACKENDKEY_LEN, &ckey)) {
			release_disconn_pgSocket(server, SV_DISCONN, true, "bad cancel key");
			return false;
		}
		memcpy(server->cancel_key, ckey, BACKENDKEY_LEN);
		res = true;
		break;

	case 'N':		/* NoticeResponse */
		slog_noise(server, "skipping pkt: %c", pkt_desc(pkt));
		res = true;
		break;
	}

	if (res)
		sbuf_prepare_skip(sbuf, pkt->len);

	return res;
}

/* process packets on logged in connection */
static bool handle_server_work(PgSocket *server, PktHdr *pkt)
{
	bool ready = false;
	bool idle_tx = false;
	char state;
	SBuf *sbuf = &server->sbuf;
	PgSocket *client = server->link;

	Assert(!server->pool->db->admin);
	switch (pkt->type) {
	default:
		slog_error(server, "unknown pkt: '%c'", pkt_desc(pkt));
		release_disconn_pgSocket(server, SV_DISCONN, true, "unknown pkt");
		return false;
	
	/* pooling decisions will be based on this packet */
	case 'Z':		/* ReadyForQuery */
		/* if partial pkt, wait */
		if (!mbuf_get_char(&pkt->data, &state))
			return false;

		/* set ready only if no tx */
		if (state == 'I') {//current not in transactions
			ready = true;
		} else if (cf_pool_mode == POOL_STMT) {
			release_disconn_pgSocket(server, SV_DISCONN, true, "Long transactions not allowed");
			return false;
		} else if (state == 'T' || state == 'E') {//T:current in transactions, E:current in failed transactions
			idle_tx = true;
		}
		break;

	case 'S':		/* ParameterStatus */
		if (!load_parameter(server, pkt, false))
			return false;
		break;

	/*
	 * 'E' and 'N' packets currently set ->ready to 0.  Correct would
	 * be to leave ->ready as-is, because overal TX state stays same.
	 * It matters for connections in IDLE or USED state which get dirty
	 * suddenly but should not as they are still usable.
	 *
	 * But the 'E' or 'N' packet between transactions signifies probably
	 * dying backend.  This its better to tag server as dirty and drop
	 * it later.
	 */
	case 'E':		/* ErrorResponse */
		if (server->setting_vars) {
			/*
			 * the SET and user query will be different TX
			 * so we cannot report SET error to user.
			 */
			log_server_error("varcache_apply failed", pkt);

			/*
			 * client probably gave invalid values in startup pkt.
			 *
			 * no reason to keep such guys.
			 */
			release_disconn_pgSocket(server, SV_DISCONN, true, "invalid server parameter");
			return false;
		}
		break;

	case 'N':		/* NoticeResponse */
		break;

	/* reply to LISTEN, don't change connection state */
	case 'A':		/* NotificationResponse */
		idle_tx = server->idle_tx;
		ready = server->ready;
		break;

	/* chat packets */
	case '2':		/* BindComplete */
	case '3':		/* CloseComplete */
	case 'c':		/* CopyDone(F/B) */
	case 'f':		/* CopyFail(F/B) */
	case 'I':		/* EmptyQueryResponse == CommandComplete */
	case 'V':		/* FunctionCallResponse */
	case 'n':		/* NoData */
	case 'G':		/* CopyInResponse */
	case 'H':		/* CopyOutResponse */
	case '1':		/* ParseComplete */
	case 's':		/* PortalSuspended */
	case 'C':		/* CommandComplete */

	/* data packets, there will be more coming */
	case 'd':		/* CopyData(F/B) */
	case 'D':		/* DataRow */
	case 't':		/* ParameterDescription */
	case 'T':		/* RowDescription */
		break;
	}
	server->idle_tx = idle_tx;
	server->ready = ready;
	server->pool->stats.server_bytes += pkt->len;

	if (client) {
		sbuf_prepare_send(sbuf, &client->sbuf, pkt->len);
		if (ready && client->query_start) {
			usec_t total;
			total = get_cached_time() - client->query_start;
			client->query_start = 0;
			server->pool->stats.query_time += total;
			//slog_debug(client, "query time: %d us", (int)total);
		} else if (ready) {
			slog_warning(client, "FIXME: query end, but query_start == 0");
		}
	} else {
		if (server->state != SV_TESTED) {
			slog_warning(server,
				     "got packet '%c' from server when not linked",
				     pkt_desc(pkt));
		}
		sbuf_prepare_skip(sbuf, pkt->len);
	}

	return true;
}


/* got connection, decide what to do */
static bool handle_connect(PgSocket *server)
{
	bool res = false;
	PgPool *pool = server->pool;

	fill_local_addr(server, sbuf_socket(&server->sbuf), pga_is_unix(&server->remote_addr));

	if (!statlist_empty(&pool->cancel_server_list)) {
		slog_debug(server, "use it for pending cancel req");
		/* if pending cancel req, send it */
		forward_cancel_request(server);
		/* notify disconnect_server() that connect did not fail */
		server->ready = 1;
		release_disconn_pgSocket(server, SV_DISCONN, false, "sent cancel req");
	} else {
		/* proceed with login */
		res = send_startup_packet(server);
		if (!res)
			release_disconn_pgSocket(server, SV_DISCONN, false, "startup pkt failed");
	}
	return res;
}

/* callback from SBuf */
bool server_proto(SBuf *sbuf, SBufEvent evtype, struct MBuf *data)
{
	bool res = false;
	PgSocket *server;
	PgPool *pool;
	PktHdr pkt;
//	PgSocket *client;

	server = container_of(sbuf, PgSocket, sbuf);
	Assert(is_server_socket(server));
	Assert(server->state != SV_FREE);
	pool = server->pool;

	/* may happen if close failed */
	if (server->state == SV_JUSTFREE)
		return false;

	switch (evtype) {
	case SBUF_EV_RECV_FAILED:
		release_disconn_pgSocket(server, SV_DISCONN, false, "server conn crashed?");
		break;
	case SBUF_EV_SEND_FAILED:
		{
			struct List *item;
			PgSocket *sk;
			statlist_for_each(item, &server->thread->serverList) {
				sk = container_of(item, PgSocket, client_thread_head);
				if (sk->index == server->index) {
					log_error("server->index: %d, server->sock: %d", server->index, server->fd);
				}
			}
		}
		release_disconn_pgSocket(server->link, CL_DISCONN, false, "unexpected eof");
		break;
	case SBUF_EV_READ:
		if (mbuf_avail_for_read(data) < NEW_HEADER_LEN) {
			slog_noise(server, "S: got partial header, trying to wait a bit");
			break;
		}
		/* parse pkt header */
		if (!get_header(data, &pkt)) {
			release_disconn_pgSocket(server, SV_DISCONN, true, "bad pkt header");
			break;
		}
		slog_noise(server, "S: pkt '%c', len=%d", pkt_desc(&pkt), pkt.len);

		server->request_time = get_cached_time();
		switch (server->state) {
		case SV_LOGIN:
			res = handle_server_startup(server, &pkt);
			break;
		case SV_TESTED:
		case SV_USED:
		case SV_ACTIVE:
		case SV_IDLE:
		case SV_OLDACTIVE:
			res = handle_server_work(server, &pkt);
			break;
		default:
			fatal("server_proto: server in bad state: %d", server->state);
		}
		break;
	case SBUF_EV_CONNECT_FAILED:
		Assert(server->state == SV_LOGIN);
		release_disconn_pgSocket(server, SV_DISCONN, false, "connect failed");
		break;
	case SBUF_EV_CONNECT_OK:
		slog_debug(server, "S: connect ok");
		Assert(server->state == SV_LOGIN);
		server->request_time = get_cached_time();
		res = handle_connect(server);
		break;
	case SBUF_EV_FLUSH:
		res = true;
		if (!server->ready) {
			log_debug("server->ready is 0");
			break;
		}

		if (cf_pool_mode  != POOL_SESSION || server->state == SV_TESTED) {
			switch (server->state) {
			case SV_ACTIVE:
			case SV_TESTED:
				/* retval does not matter here */
				release_server(server);
				break;
			case SV_IDLE:
				break;
			case SV_WAITING:
				change_server_state_safe(server, SV_IDLE);
				break;
			default:
				slog_warning(server, "EV_FLUSH with state=%d", server->state);
			}
		}
		break;
	case SBUF_EV_PKT_CALLBACK:
		slog_warning(server, "SBUF_EV_PKT_CALLBACK with state=%d", server->state);
		break;
	}
	if (!res && pool->db->admin)
		takeover_login_failed();
	return res;
}
