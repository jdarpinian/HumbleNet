#ifdef __EMSCRIPTEN__

#include "libwebrtc.h"

#include <emscripten.h>

#define USE_STUN

struct libwebrtc_context {
	lwrtc_callback_function callback;
};

extern "C"
int EMSCRIPTEN_KEEPALIVE libwebrtc_helper(struct libwebrtc_context *context, struct libwebrtc_connection *connection, struct libwebrtc_data_channel* channel,
								  enum libwebrtc_callback_reasons reason, void *user, void *in, int len) {
	return context->callback( context, connection, channel, reason, user, in, len );
}

struct libwebrtc_context* libwebrtc_create_context( lwrtc_callback_function callback ) {
	libwebrtc_context* ctx = new libwebrtc_context();
	ctx->callback = callback;

	bool supported = EM_ASM_INT({
		var libwebrtc = {};

		if( ! self.RTCPeerConnection || ! self.RTCIceCandidate || ! self.RTCSessionDescription )
			return 0;

		var ctx = $0;
		libwebrtc.connections = new Map();
		libwebrtc.channels = new Map();
		libwebrtc.on_event = Module.cwrap('libwebrtc_helper', 'number', ['number', 'number', 'number', 'number', 'number', 'number', 'number']);
		libwebrtc.options = {};

		libwebrtc.create = function() {
			var connection = new RTCPeerConnection(this.options,null);

			connection.trickle = true;

			connection.destroy = this.destroy;

			connection.ondatachannel = this.on_datachannel;
			connection.onicecandidate = this.on_candidate;
			connection.onsignalingstatechange = this.on_signalstatechange;
			connection.oniceconnectionstatechange = this.on_icestatechange;

			connection.id = this.connections.size + 1;

			this.connections.set( connection.id, connection );

			return connection;
		};
		libwebrtc.create_channel = function(connection, name) {
			var channel = connection.createDataChannel( name, {ordered: false, maxRetransmits: 0} );
			channel.parent = connection;
			// use the parents data initially
			channel.user_data = connection.user_data;
			channel.binaryType = 'arraybuffer';

			channel.onopen = libwebrtc.on_channel_connected;
			channel.onclose = libwebrtc.on_channel_close;
			channel.onmessage = libwebrtc.on_channel_message;
			channel.onerror = libwebrtc.on_channel_error;

			channel._id = libwebrtc.channels.size+1;

			libwebrtc.channels.set( channel._id, channel);

			return channel;
		};

		libwebrtc.on_sdp = function(){
			if( ! this.trickle && this.iceGatheringState != "complete" ) {
				return;
			}

			var sdp = this.localDescription.sdp;
			var stack = stackSave();
			// local description //
			const array  = intArrayFromString(sdp);
			const buffer = stackAlloc(array.length);
			Module.HEAPU8.set(array, buffer);
			libwebrtc.on_event( ctx, this.id, 0, 1, this.user_data, buffer, sdp.length);
			stackRestore(stack);
		};
		libwebrtc.on_candidate = function(event){
			if( !event ) {
				return;
			}

			if( this.iceConnectionState === 'completed') {
				Module.out("ignoring ice, were not trying to connect: " + this.iceConnectionState);
				return;
			}

			if( !event.candidate ) {
				Module.out("no more candidates: " + this.iceGatheringState );
				if( ! this.trickle ) {
					libwebrtc.on_sdp.call(this);
				}
				return;
			}

			Module.out("ice candidate " + event.candidate.candidate + " -- " + this.iceGatheringState);

			if( this.trickle ) {
				var stack = stackSave();
				// ice_candidate //
				const array = intArrayFromString(event.candidate.candidate);
				const buffer = stackAlloc(array.length);
				Module.HEAPU8.set(array, buffer);
				libwebrtc.on_event( ctx, this.id, 0, 2, this.user_data, buffer, event.candidate.candidate.length);
				stackRestore(stack);
			}
		};
		libwebrtc.on_signalstatechange = function(event){
			Module.out("signalingState: "+ this.signalingState);
		};
		libwebrtc.on_icestatechange = function(event){
			Module.out( "icestate: " + this.iceConnectionState + " / iceGatheringState: " + this.iceGatheringState);
			if( this.iceConnectionState == 'failed' || this.iceConnectionState == 'disconnected'  ) {
				this.close();
			} else if( this.iceConnectionState == 'closed' ) {
				libwebrtc.on_disconnected.call(this,event);
			} else if( this.iceConnectionState == 'completed' ) {
				// connected //
				libwebrtc.on_event( ctx, this.id, 0, 3, this.user_data, 0, 0);
			}
		};
		libwebrtc.on_disconnected = function(event){
			var stack = stackSave();
			// disconnected //
			libwebrtc.on_event( ctx, this.id, 0, 4, this.user_data, 0, 0);
			stackRestore(stack);
			this.destroy();
		};
		libwebrtc.on_datachannel = function(event){
			Module.out("datachannel");
			var channel = event.channel;

			channel.parent = this;
			// use the parents data initially
			channel.user_data = this.user_data;
			channel.binaryType = 'arraybuffer';

			channel.onopen = libwebrtc.on_channel_accept;
			channel.onclose = libwebrtc.on_channel_close;
			channel.onmessage = libwebrtc.on_channel_message;
			channel.onerror = libwebrtc.on_channel_error;

			channel._id = libwebrtc.channels.size+1;

			libwebrtc.channels.set( channel._id, channel);
		};
		libwebrtc.on_channel_accept = function(event){
			Module.out("accept");
			var stack = stackSave();
			// channel accepted //
			const array = intArrayFromString(this.label);
			const buffer = stackAlloc(array.length);
			Module.HEAPU8.set(array, buffer);
			libwebrtc.on_event(ctx, this.parent.id, this._id, 5, this.user_data, buffer, this.label.length);
			stackRestore(stack);
		};
		libwebrtc.on_channel_connected = function(event){
			Module.out("connect");
			var stack = stackSave();
			// channel connected //
			const array = intArrayFromString(this.label);
			const buffer = stackAlloc(array.length);
			Module.HEAPU8.set(array, buffer);
			libwebrtc.on_event(ctx, this.parent.id, this._id, 6, this.user_data, buffer, this.label.length);
			stackRestore(stack);
		};
		libwebrtc.on_channel_message = function(event){
			var stack = stackSave();
			var len = event.data.byteLength;
			var ptr = stackAlloc(len);

//            Module.out("Data: " + len );
			var data = new Uint8Array( event.data );
			Module.HEAPU8.set(data, ptr);

			// channel data //
			libwebrtc.on_event( ctx, this.parent.id, this._id, 7, this.user_data, ptr, len);
			stackRestore(stack);
		};
		libwebrtc.on_channel_error = function(event){
			Module.out("Got channel error: " + event);
			this.close();
		};
		libwebrtc.on_channel_close = function(event){
			var stack = stackSave();
			// close channel //
			libwebrtc.on_event(ctx, this.parent.id, this._id, 8, this.user_data, 0, 0);
			stackRestore(stack);
		};
		libwebrtc.destroy = function() {
			libwebrtc.connections.set( this.id, undefined );

			this.ondatachannel = undefined;
			this.onicecandidate = undefined;
			this.onsignalingstatechange = undefined;
			this.oniceconnectionstatechange = undefined;

			// destroy (connection) //
			libwebrtc.on_event(ctx, this.id, 0, 10, this.user_data, 0, 0);
			this.close();
			Module.out("Destroy webrtc: " + this.id );
		};


		Module.__libwebrtc = libwebrtc;

		return 1;
	}, ctx);

	if( !supported ) {
		delete ctx;
		return nullptr;
	}

	return ctx;
}

void libwebrtc_destroy_context(struct libwebrtc_context* ctx)
{
	delete ctx;
}

void libwebrtc_set_stun_servers( struct libwebrtc_context* ctx, const char** servers, int count)
{
	EM_ASM({
		Module.__libwebrtc.options.iceServers = Module.__libwebrtc.options.iceServers || [];
	});

	for( int i = 0; i < count; ++i ) {
		EM_ASM_INT({
			var server = {};
			server.urls = "stun:" + UTF8ToString($0);
			Module.__libwebrtc.options.iceServers.push( server );
		}, *servers);
		servers++;
	}
}

void libwebrtc_add_turn_server( struct libwebrtc_context* ctx, const char* server, const char* username, const char* password) {
	EM_ASM({
		Module.__libwebrtc.options.iceServers = Module.__libwebrtc.options.iceServers || [];
		Module.__libwebrtc.options.iceServers.push({
			urls: "turn:" + UTF8ToString($0),
			username: UTF8ToString($1),
			credential: UTF8ToString($2)
		});
	}, server, username, password);
}

struct libwebrtc_connection* libwebrtc_create_connection_extended(struct libwebrtc_context* ctx, void* user_data) {
	return (struct libwebrtc_connection*)EM_ASM_INT({
		var connection = Module.__libwebrtc.create();
		connection.user_data = $0;
		return connection.id;
	}, user_data);
}

void libwebrtc_set_user_data(struct libwebrtc_connection* connection, void* user_data ) {
	EM_ASM_INT({
		var connection = Module.__libwebrtc.connections.get($0);
		if( ! connection ) {
			return;
		}
		connection.user_data = $1;
	}, connection, user_data);
}

int libwebrtc_create_offer( struct libwebrtc_connection* connection ) {
	return EM_ASM_INT({
		var connection = Module.__libwebrtc.connections.get($0);
		if( ! connection ) {
			return 0;
		}

		// in order to create and offer at least one stream must have been added.
		// simply create one, then drop it.
		connection.default_channel = Module.__libwebrtc.create_channel( connection,"default");

		connection.createOffer({})
			.then(function(offer){
				connection.setLocalDescription( new RTCSessionDescription( offer ) )
					.then( function() {
						Module.__libwebrtc.on_sdp.call( connection );
					}).catch(function(error){
						alert( "setLocalDescription(create): " + error );
					});
			}).catch(function(error){
				alert("createOffer: " + error);
			});

		return 1;
	}, connection, 0);
}

int libwebrtc_set_offer( struct libwebrtc_connection* connection, const char* sdp ) {
	return EM_ASM_INT({
		var connection = Module.__libwebrtc.connections.get($0);
		if( ! connection ) {
			return 0;
		}

		var offer = {};
		offer.type = 'offer';
		offer.sdp = UTF8ToString( $1 );

		connection.setRemoteDescription( new RTCSessionDescription( offer ) )
			.then(function() {
				connection.createAnswer()
					.then(function(offer){
						connection.setLocalDescription( new RTCSessionDescription( offer ) )
							.then( function() {
								Module.__libwebrtc.on_sdp.call( connection );
							}).catch(function(error){
								alert( "setLocalDescription(answer): " + error );
							});
					}).catch(function(error){
						alert("createAnswer: " + error);
				});
			}).catch(function(error){
				alert("setRemoteDescriptor(answer): " + error );
			});
		return 1;
	}, connection, sdp );
}

int libwebrtc_set_answer( struct libwebrtc_connection* connection, const char* sdp ) {
	return EM_ASM_INT({
		var connection = Module.__libwebrtc.connections.get($0);
		if( ! connection ) {
			return 0;
		}

		var offer = {};
		offer.type = 'answer';
		offer.sdp = UTF8ToString( $1 );

		connection.setRemoteDescription( new RTCSessionDescription( offer ) )
			.then( function() {
				// nothing, as this is the answer to our offer
			}).catch(function(error){
				alert("setRemoteDescriptor(answer): " + error );
			});
		return 1;
	}, connection, sdp );
}

int libwebrtc_add_ice_candidate( struct libwebrtc_connection* connection, const char* candidate ) {
	return EM_ASM_INT({
		var connection = Module.__libwebrtc.connections.get($0);
		if( ! connection ) {
			return 0;
		}

		var options = {};
		options.candidate = UTF8ToString($1);
		options.sdpMLineIndex = 0;

		if( connection.iceConnectionState == 'checking' || connection.iceConnectionState == 'connected'
		   // FF workaround
		   || connection.iceConnectionState == 'new') {
			Module.out( "AddIce: " + options.candidate );
			connection.addIceCandidate( new RTCIceCandidate( options ) );
		} else {
			Module.out( "Not negotiating (" + connection.iceConnectionState + "), ignored candidate: " + options.candidate );
		}

	}, connection, candidate );
}

struct libwebrtc_data_channel* libwebrtc_create_channel( struct libwebrtc_connection* connection, const char* name ) {
	return (struct libwebrtc_data_channel*)EM_ASM_INT({
		var connection = Module.__libwebrtc.connections.get($0);
		if( ! connection ) {
			return 0;
		}

		var channel;

		if( connection.default_channel ){
			channel = connection.default_channel;
			connection.default_channel = 0;
		}else{
			channel = Module.__libwebrtc.create_channel( connection, UTF8ToString($1) );
		}

		return channel._id;

	}, connection, name );
}

int libwebrtc_write( struct libwebrtc_data_channel* channel, const void* data, int len ) {
	return EM_ASM_INT({
		var channel = Module.__libwebrtc.channels.get($0);
		if( ! channel ) {
			return -1;
		}

		// alloc a Uint8Array backed by the incoming data.
		var data_in = new Uint8Array(Module.HEAPU8.buffer, $1, $2 );
		// allow the dest array
		var data = new Uint8Array($2);
		// set the dest from the src
		data.set(data_in);

		channel.send( data );
		return $2;

	}, channel, data, len );
}

void libwebrtc_close_connection( struct libwebrtc_connection* channel ) {
	EM_ASM_INT({
		var connection = Module.__libwebrtc.connections.get($0);
		if( ! connection ) {
			return -1;
		}

		Module.__libwebrtc.connections.set( connection.id, undefined );

		connection.close();

	}, channel );
}

void libwebrtc_close_channel( struct libwebrtc_data_channel* channel ) {
	EM_ASM_INT({
		var channel = Module.__libwebrtc.channels.get($0);
		if( ! channel ) {
			return -1;
		}

		Module.__libwebrtc.connections.set( channel.id, undefined );

		channel.close();
	}, channel );
}


#endif

