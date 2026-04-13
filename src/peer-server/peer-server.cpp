#include <signal.h>

#include <cstdlib>
#include <cstring>

#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <iostream>
#include <fstream>
#include <algorithm>

#ifdef _WIN32
#	define WIN32_LEAN_AND_MEAN
#	define NOMINMAX
#	include <windows.h>
#else  // posix
#	include <sys/socket.h>
#	include <netinet/in.h>
#	include <arpa/inet.h>

#	include <sys/types.h>
#	include <sys/stat.h>
#	include <unistd.h>
#endif

#include <libwebsockets.h>

#include "humblenet.h"
#include "humblepeer.h"

#include "game.h"
#include "p2p_connection.h"
#include "server.h"
#include "logging.h"
#include "game_db.h"

using namespace humblenet;

namespace humblenet {

	ha_bool sendP2PMessage(P2PSignalConnection *conn, const uint8_t *buff, size_t length) {
		conn->sendMessage(buff, length);
		return true;
	}

}  // namespace humblenet

static std::unique_ptr<Server> peerServer;


static ha_bool p2pSignalProcess(const humblenet::HumblePeer::Message *msg, void *user_data) {
	return reinterpret_cast<P2PSignalConnection *>(user_data)->processMsg(msg);
}

static bool lookup_peer(const std::string& hostname) {
	if (peerServer->games.empty()) return false;
	auto& aliases = peerServer->games.begin()->second->aliases;
	return aliases.find(hostname) != aliases.end();
}

static const char* get_http_body(void *user) {
	if (*(char*)user) return "{\"found\":true}";
	else return "{\"found\":false}";
}

int callback_humblepeer(struct lws *wsi
				  , enum lws_callback_reasons reason
				  , void *user, void *in, size_t len) {


	switch (reason) {
	case LWS_CALLBACK_HTTP:
		{
			const char *uri = (const char *)in;
			if (strncmp(uri, "/lookup/", 8) == 0) {
				const char *hostname = uri + 8;
				(*(char*)user) = lookup_peer(hostname) ? 1 : 0;
				unsigned char buffer[8192];
				memset(buffer, 0, sizeof(buffer));
				unsigned char *p = buffer;
				unsigned char *end = buffer + sizeof(buffer);
				if (lws_add_http_header_status(wsi, HTTP_STATUS_OK, &p, end))
					return 1;
				if (lws_add_http_header_by_token(wsi, WSI_TOKEN_HTTP_CACHE_CONTROL, (unsigned char *)"no-cache", 8, &p, end))
					return 1;
				if (lws_add_http_header_by_token(wsi, WSI_TOKEN_HTTP_CONTENT_TYPE, (unsigned char *)"application/json", 16, &p, end))
					return 1;
				if (lws_add_http_header_by_token(wsi, WSI_TOKEN_HTTP_ACCESS_CONTROL_ALLOW_ORIGIN, (unsigned char *)"*", 1, &p, end))
					return 1;
				if (lws_add_http_header_content_length(wsi, strlen(get_http_body(user)), &p, end))
					return 1;
				if (lws_finalize_write_http_header(wsi, buffer, &p, end))
					return 1;
				// lws_write(wsi, buffer, p - buffer, LWS_WRITE_HTTP_HEADERS);
				lws_callback_on_writable(wsi);
				return 0;
			} else {
				lws_return_http_status(wsi, HTTP_STATUS_NOT_FOUND, NULL);
			}
		}
		break;
	case LWS_CALLBACK_HTTP_WRITEABLE:
		{
			const char* body = NULL;
			body = get_http_body(user);
			size_t len = strlen(body);
			unsigned char buffer[8192];
			memset(buffer, 0, sizeof(buffer));
			strncpy((char*)buffer, body, len);

			if (lws_write(wsi, buffer, len, LWS_WRITE_HTTP_FINAL) != len) {
				return 1;
			}
			if (lws_http_transaction_completed(wsi)) {
				return -1;
			}
			return 0;
		}
		break;

	case LWS_CALLBACK_ESTABLISHED:
		{
			std::unique_ptr<P2PSignalConnection> conn(new P2PSignalConnection(peerServer.get()));
			conn->wsi = wsi;

			struct sockaddr_storage addr;
			socklen_t len = sizeof(addr);
			size_t bufsize = std::max(INET_ADDRSTRLEN, INET6_ADDRSTRLEN) + 1;
			std::vector<char> ipstr(bufsize, 0);
			int port;

			int socket = lws_get_socket_fd(wsi);
			getpeername(socket, (struct sockaddr*)&addr, &len);

			if (addr.ss_family == AF_INET) {
				struct sockaddr_in *s = (struct sockaddr_in*)&addr;
				port = ntohs(s->sin_port);
				inet_ntop(AF_INET, &s->sin_addr, &ipstr[0], INET_ADDRSTRLEN);
			}
			else { // AF_INET6
				struct sockaddr_in6 *s = (struct sockaddr_in6*)&addr;
				port = ntohs(s->sin6_port);
				inet_ntop(AF_INET6, &s->sin6_addr, &ipstr[0], INET6_ADDRSTRLEN);
			}

			conn->url = std::string(&ipstr[0]);
			conn->url += std::string(":");
			conn->url += std::to_string(port);

			LOG_INFO("New connection from \"%s\"\n", conn->url.c_str());

			peerServer->signalConnections.emplace(wsi, std::move(conn));
		}

		break;


	case LWS_CALLBACK_CLOSED:
		{
			auto it = peerServer->signalConnections.find(wsi);
			if (it == peerServer->signalConnections.end()) {
				// Tried to close nonexistent signal connection
				LOG_ERROR("Tried to close a signaling connection which doesn't appear to exist\n");
				return 0;
			}

			P2PSignalConnection *conn = it->second.get();
			assert(conn != NULL);

			LOG_INFO("Closing connection to peer %u (%s)\n", conn->peerId, conn->url.c_str());

			if (conn->peerId != 0) {
				// remove it from list of peers
				assert(conn->game != NULL);
				auto it2 = conn->game->peers.find(conn->peerId);
				// if peerId is valid (nonzero)
				// this MUST exist
				assert(it2 != conn->game->peers.end());
				conn->game->peers.erase(it2);

				// remove any aliases to this peer
				conn->game->erasePeerAliases(conn->peerId);
			}

			// and finally remove from list of signal connections
			// this is unique_ptr so it also destroys the object
			peerServer->signalConnections.erase(it);
		}

		break;

	case LWS_CALLBACK_RECEIVE:
		{
			auto it = peerServer->signalConnections.find(wsi);
			if (it == peerServer->signalConnections.end()) {
				// Receive on nonexistent signal connection
				return 0;
			}

			char *inBuf = reinterpret_cast<char *>(in);
			it->second->recvBuf.insert(it->second->recvBuf.end(), inBuf, inBuf + len);

			// If we finished receiving a whole message
			if (!lws_remaining_packet_payload(wsi) && lws_is_final_fragment(wsi)) {
				// function which will parse recvBuf
				ha_bool retval = parseMessage(it->second->recvBuf, p2pSignalProcess, it->second.get());
				if (!retval) {
					// error in parsing, close connection
					LOG_ERROR("Error in parsing message from \"%s\"\n", it->second->url.c_str());
					return -1;
				}
			}
		}

		break;

	case LWS_CALLBACK_SERVER_WRITEABLE:
		{
			assert(wsi != NULL);
			auto it = peerServer->signalConnections.find(wsi);
			if (it == peerServer->signalConnections.end()) {
				// nonexistent signal connection, close it
				return -1;
			}

			P2PSignalConnection *conn = it->second.get();
			if (conn->wsi != wsi) {
				// this connection is not the one currently active in humbleNetState.
				// this one must be obsolete, close it
				return -1;
			}

			if (conn->sendBuf.empty()) {
				// no data in sendBuf
				return 0;
			}

			size_t bufsize = conn->sendBuf.size();
			std::vector<unsigned char> sendbuf(LWS_SEND_BUFFER_PRE_PADDING + bufsize + LWS_SEND_BUFFER_POST_PADDING, 0);
			memcpy(&sendbuf[LWS_SEND_BUFFER_PRE_PADDING], &conn->sendBuf[0], bufsize);
			int retval = lws_write(conn->wsi, &sendbuf[LWS_SEND_BUFFER_PRE_PADDING], bufsize, LWS_WRITE_BINARY);
			if (retval < 0) {
				// error while sending, close the connection
				return -1;
			}
			if (retval < bufsize) {
				// This should not happen. lws_write returns the number of bytes written but it includes the headers it adds to pre padding which we don't know about.
				// So if it actually does a partial write there is no way for us to know how much of our data was sent and how much was headers, the API would be broken.
				// The docs say it buffers data internally and sends it all, so this shouldn't happen.
				LOG_ERROR("Partial write to peer %u (%s)\n", conn->peerId, conn->url.c_str());
				return -1;
			}

			// successful write
			conn->sendBuf.clear();
		}
		break;

	case LWS_CALLBACK_FILTER_PROTOCOL_CONNECTION:
		break;

	case LWS_CALLBACK_PROTOCOL_INIT:
		break;

	case LWS_CALLBACK_PROTOCOL_DESTROY:
		// we don't care
		break;

	default:
		// LOG_WARNING("callback_humblepeer %p %u %p %p %u\n", wsi, reason, user, in, len);
		break;
	}

	return 0;
}

struct lws_protocols protocols_8080[] = {
	  { "humblepeer", callback_humblepeer, 1 }
	, { NULL, NULL, 0 }
};

struct lws_protocols protocols_443[] = {
	  { "humblepeer", callback_humblepeer, 1 }
	, lws_acme_client_protocols[0]

	, { NULL, NULL, 0 }
};

void help(const std::string& prog, const std::string& error = "")
{
	if (!error.empty()) {
		std::cerr << "Error: " << error << "\n\n";
	}
	std::cerr
		<< "Humblenet peer match-making server\n"
		<< " " << prog << "[-h] [--port 8080] [--tls-port 443] [--acme-staging] --email em@example.com --common-name test.example.com\n"
		<< "   --email Used to request SSL certificate from Let's Encrypt\n"
		<< "   --common-name Domain name Let's Encrypt will ping during ACME and issue certs for\n"
		<< "   --port HTTP / websocket listen port (default 8080)\n"
		<< "   --tls-port HTTPS / WSS listen port (default 443)\n"
		<< "   --acme-staging Use Let's Encrypt staging instead of production\n"
		<< "   -h     Displays this help\n"
		<< std::endl;
}

static bool keepGoing = true;

void sighandler(int sig)
{
	keepGoing = false;
}

int main(int argc, char *argv[]) {
	char* email = nullptr;
	char* common_name = nullptr;
	char* turn_server = nullptr;
	char* turn_username = nullptr;
	char* turn_password = nullptr;
	bool acme_staging = false;
	int http_port = 8080;
	int tls_port = 443;
	bool http_port_overridden = false;
	bool tls_port_overridden = false;
	// Parse command line arguments
	for (int i = 1; i < argc; ++i) {
		std::string arg  = argv[i];
		if (arg == "-h") {
			help(argv[0]);
			exit(1);
		} else if (arg == "--email") {
			++i;
			if (i < argc) {
				email = argv[i];
			} else {
				help(argv[0], "--email option requires an argument");
				exit(2);
			}
		} else if (arg == "--common-name") {
			++i;
			if (i < argc) {
				common_name = argv[i];
			} else {
				help(argv[0], "--common_name option requires an argument");
				exit(2);
			}
		} else if (arg == "--TURN-server") {
			++i;
			if (i < argc) {
				turn_server = argv[i];
			} else {
				help(argv[0], "--TURN-server option requires an argument");
				exit(2);
			}
		} else if (arg == "--TURN-username") {
			++i;
			if (i < argc) {
				turn_username = argv[i];
			} else {
				help(argv[0], "--TURN-username option requires an argument");
				exit(2);
			}
		} else if (arg == "--TURN-password") {
			++i;
			if (i < argc) {
				turn_password = argv[i];
			} else {
				help(argv[0], "--TURN-password option requires an argument");
				exit(2);
			}
		} else if (arg == "--port") {
			++i;
			if (i < argc) {
				http_port = atoi(argv[i]);
				http_port_overridden = true;
			} else {
				help(argv[0], "--port option requires an argument");
				exit(2);
			}
		} else if (arg == "--tls-port") {
			++i;
			if (i < argc) {
				tls_port = atoi(argv[i]);
				tls_port_overridden = true;
			} else {
				help(argv[0], "--tls-port option requires an argument");
				exit(2);
			}
		} else if (arg == "--acme-staging") {
			acme_staging = true;
		}
	}

	if (email == nullptr || common_name == nullptr) {
		help(argv[0], "--email and --common-name are required if you want to run with TLS\n");
	}

	if (turn_server != nullptr || turn_username != nullptr || turn_password != nullptr) {
		if (turn_server == nullptr || turn_username == nullptr || turn_password == nullptr) {
			help(argv[0], "--TURN-server, --TURN-username, and --TURN-password must all be specified together\n");
			exit(2);
		}
	
	}

	if (email != nullptr && common_name != nullptr && !http_port_overridden) {
		http_port = 80;
	}

	// logFileOpen("peer-server.log");
	logFileOpen("");
	lws_set_log_level(LLL_ERR | LLL_WARN, NULL);
	// lws_set_log_level(LLL_ERR | LLL_WARN | LLL_NOTICE | LLL_INFO | LLL_DEBUG | LLL_EXT, NULL);

	std::ofstream ofs("peer-server.pidfile");
#ifndef _WIN32
	ofs << (int)getpid() << std::endl;
#endif // _WIN32

	signal(SIGINT, sighandler);
	signal(SIGTERM, sighandler);

	std::shared_ptr<GameDB> gameDB;

	gameDB.reset(new GameDBAnonymous());
	// gameDB.reset(new GameDBFlatFile(config.gameDB.substr(5)));

	peerServer.reset(new Server(gameDB));
	// peerServer->stunServerAddress = "stun.cloudflare.com:3478";
	if (turn_server) {
		peerServer->turnSurver = turn_server;
		peerServer->turnUsername = turn_username;
		peerServer->turnPassword = turn_password;
	}

	struct lws_context_creation_info info;
	memset(&info, 0, sizeof(info));
	info.gid = -1;
	info.uid = -1;
	info.options = LWS_SERVER_OPTION_IGNORE_MISSING_CERT | LWS_SERVER_OPTION_EXPLICIT_VHOSTS;
	const lws_retry_bo_t retry = {
		.secs_since_valid_ping = 30,
		.secs_since_valid_hangup = 100,
	};
	info.retry_and_idle_policy = &retry;
	
	peerServer->context = lws_create_context(&info);
	if (peerServer->context == NULL) {
		// TODO: error message
		exit(1);
	}

	info.port = http_port;
	info.vhost_name = "HTTP_8080_vhost";
	struct lws_protocol_vhost_options pvo6 = {
		NULL, NULL, "email", email
	}, pvo5 = {
		&pvo6, NULL, "common-name", common_name
	}, pvo4 = {
		&pvo5, NULL, "directory-url", acme_staging ?
			"https://acme-staging-v02.api.letsencrypt.org/directory" :
			"https://acme-v02.api.letsencrypt.org/directory"
	}, pvo3 = {
		&pvo4, NULL, "auth-path", "./auth.jwk"
	}, pvo2 = {
		&pvo3, NULL, "cert-path", "./peer-server.key.crt"
	}, pvo1 = {
		&pvo2, NULL, "key-path", "./peer-server.key.pem" /* would be an absolute path */
	}, pvo = {
		NULL,                  /* "next" pvo linked-list */
		&pvo1,                 /* "child" pvo linked-list */
		"lws-acme-client",        /* protocol name we belong to on this vhost */
		""                     /* ignored */
	};
	info.pvo = &pvo;
	info.protocols = protocols_8080;

	struct lws_vhost *host_8080 = lws_create_vhost(peerServer->context, &info);
	if (host_8080 == NULL) {
		LOG_ERROR("Failed to create vhost for port %d\n", http_port);
		exit(1);
	}

	if (email == nullptr || common_name == nullptr) {
		LOG_WARNING("--email or --common-name not specified, not starting TLS server\n");
	} else {
		info.protocols = protocols_443;
		info.port = tls_port;
		info.vhost_name = "SSL_vhost";
		info.ssl_cert_filepath = "./peer-server.key.crt";
		info.ssl_private_key_filepath = "./peer-server.key.pem";
		info.options |= LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
		struct lws_vhost *host_443 = lws_create_vhost(peerServer->context, &info);
		if (host_443 == NULL) {
			LOG_ERROR("Failed to create vhost for port %d\n", tls_port);
			exit(1);
		}
	}

	while (keepGoing) {
		// use timeout so we will eventually process signals
		// but relatively long to reduce CPU load
		// TODO: configurable timeout
		lws_service(peerServer->context, 1000);
	}

	peerServer.reset();

#ifndef _WIN32
	unlink("peer-server.pidfile");
#endif // _WIN32

	return 0;
}
