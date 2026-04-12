#ifdef __EMSCRIPTEN__

#include "libwebsockets_asmjs.h"

#include <emscripten.h>
#include <stdio.h>
// TODO: should have a way to disable this on release builds
#define LOG printf

struct lws_context {
	lws_protocols* protocols; // we dont have any state to manage.
};


extern "C" int EMSCRIPTEN_KEEPALIVE lws_helper( int protocol, struct lws_context *context,
														struct lws *wsi,
														enum lws_callback_reasons reason, void *user,
														void *in, size_t len ) {
	LOG("%d -> %d -> %p -> %d\n", protocol, reason, in, (int)len );

	if( reason == LWS_CALLBACK_WSI_DESTROY ) {
		context->protocols[protocol].callback( wsi, reason, user, in, len );
		// TODO See if we need to destroy the user_data..currently we dont allocate it, so we never would need to free it.
		return 0;
	}
	else
		return context->protocols[protocol].callback( wsi, reason, user, in, len );
}

struct lws_context* lws_create_context( struct lws_context_creation_info* info ){
	return lws_create_context_extended( info );
}

struct lws_context* lws_create_context_extended( struct lws_context_creation_info* info ) {
	struct lws_context* ctx = new lws_context();
	ctx->protocols = info->protocols;

	EM_ASM_({
		var libwebsocket = {};
		var ctx = $0;

		libwebsocket.sockets = new Map();
		libwebsocket.on_event = Module.cwrap('lws_helper', 'number', ['number', 'number', 'number', 'number', 'number', 'number', 'number']);
		libwebsocket.connect = function( url, protocol, user_data ) {
			try {
				var socket = new WebSocket(url,protocol);
				socket.binaryType = "arraybuffer";
				socket.user_data = user_data;
				socket.protocol_id = 0;

				socket.onopen = this.on_connect;
				socket.onmessage = this.on_message;
				socket.onclose = this.on_close;
				socket.onerror = this.on_error;
				socket.destroy = this.destroy;

				socket.id = this.sockets.size + 1;

				this.sockets.set( socket.id, socket );

				return socket;
			} catch(e) {
				Module.out("Socket creation failed:" + e);
				return 0;
			}
		};
		libwebsocket.on_connect = function() {
			var stack = stackSave();
			// filter protocol //
			const array = intArrayFromString(this.protocol);
			const buffer = stackAlloc(array.length);
			Module.HEAPU8.set(array, buffer);
			var ret = libwebsocket.on_event( 0, ctx, this.id, 9, this.user_data, buffer, this.protocol.length );
			if( !ret ) {
				// client established
				ret = libwebsocket.on_event( this.protocol_id, ctx, this.id, 3, this.user_data, 0, 0 );
			}
			if( ret ) {
				this.close();
			}
			stackRestore(stack);
		};
		libwebsocket.on_message = function(event) {
			var stack = stackSave();
			var len = event.data.byteLength;
			var data = new Uint8Array( event.data );
			const ptr = stackAlloc(data.length);
			Module.HEAPU8.set(data, ptr);

			// client receive //
			if( libwebsocket.on_event( this.protocol_id, ctx, this.id, 6, this.user_data, ptr, len ) ) {
				this.close();
			}
			stackRestore(stack);
		};
		libwebsocket.on_close = function() {
			if (!this.user_data) return;
			// closed //
			libwebsocket.on_event( this.protocol_id, ctx, this.id, 4, this.user_data, 0, 0 );
			this.destroy();
		};
		libwebsocket.on_error = function() {
			if (!this.user_data) return;
			// client connection error //
			libwebsocket.on_event( this.protocol_id, ctx, this.id, 2, this.user_data, 0, 0 );
			this.destroy();
		};
		libwebsocket.destroy = function() {
			if (!this.user_data) return;
			var user_data = this.user_data;
			this.user_data = 0;
			libwebsocket.sockets.set( this.id, undefined );
			libwebsocket.on_event( this.protocol_id, ctx, this.id, 11, user_data, 0, 0 );
		};

		Module.__libwebsocket = libwebsocket;
	}, ctx  );

	return ctx;
}

void lws_context_destroy(struct lws_context* ctx ) {
	delete ctx;
}

struct lws* lws_client_connect_extended(struct lws_context* ctx , const char* url, const char* protocol, void* user_data ) {
	
	struct lws* s =  (struct lws*)EM_ASM_INT({
		var socket = Module.__libwebsocket.connect( UTF8ToString($0), UTF8ToString($1), $2);
		if( ! socket ) {
			return 0;
		}

		return socket.id;
	}, url, protocol, user_data);
	
	return s;
}

int lws_write( struct lws* socket, const void* data, int len, enum lws_write_protocol protocol ) {
	return EM_ASM_INT({
		var socket = Module.__libwebsocket.sockets.get( $0 );
		if( ! socket || socket.readyState !== 1) {
			return -1;
		}

		// alloc a Uint8Array backed by the incoming data.
		var data_in = new Uint8Array(Module.HEAPU8.buffer, $1, $2 );
		// allow the dest array
		var data = new Uint8Array($2);
		// set the dest from the src
		data.set(data_in);

		socket.send( data );

		return $2;

	}, socket, data, len );
}

void lws_callback_on_writable( struct lws* socket ) {
	// no-op
}

#endif

