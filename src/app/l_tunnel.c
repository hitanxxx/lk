#include "lk.h"

static queue_t     usable;
static queue_t     in_use;
static tunnel_t *  pool;

static char tunnel_200_response[] = "HTTP/1.1 200 Connection Established\r\n"
"Connection: keep-alive\r\n\r\n";
static char tunnel_400_response[] = "HTTP/1.1 400 Bad Request\r\n"
"Connection: Close\r\n"
"Server: ktunnel-v1.0\r\n\r\n";

static status tunnel_local_process_request( event_t * ev );

// tunnel_free -----------
static status tunnel_free( tunnel_t * t )
{
	queue_remove( &t->queue );
    queue_insert_tail( &usable, &t->queue );

	t->upstream = NULL;
	t->downstream = NULL;

	t->https = 0;
	t->http_response_version_n = 0;
	t->meta_send.pos = t->meta_send.last = t->meta_send.start;
	t->meta_send.next = NULL;

	t->in = NULL;
	t->out = NULL;

	t->request_head = NULL;
	t->request_body = NULL;
	t->response_head = NULL;
	t->response_body = NULL;
	t->trans_body_done = 0;
	return OK;
}
// tunnel_alloc -----------
static status tunnel_alloc( tunnel_t ** tunnel )
{
	queue_t * q;
    tunnel_t * new;

    if( queue_empty( &usable ) ) {
        err_log("%s --- usable empty", __func__ );
        return ERROR;
    }
    q = queue_head( &usable );
    queue_remove( q );
    queue_insert_tail( &in_use, q );
    new = l_get_struct( q, tunnel_t, queue );
    *tunnel = new;
	return OK;
}
// tunnel_free_connection -------
static status tunnel_free_connection( event_t * ev )
{
	connection_t* c;

	c = ev->data;
	net_free( c );
	return OK;
}
// tunnel_close_connection -------
static status tunnel_close_connection( connection_t * c )
{
	status rc;

	if( c->ssl_flag && c->ssl ) {
		rc = ssl_shutdown( c->ssl );
		if( rc == AGAIN ) {
			c->ssl->handler = tunnel_free_connection;
			return AGAIN;
		}
	}
	tunnel_free_connection( c->read );
	return OK;
}
// tunnel_close_tunnel -------
static status tunnel_close_tunnel( tunnel_t * t )
{
	if( t->request_head ) {
		http_request_head_free( t->request_head );
	}
	if( t->request_body ) {
		http_entitybody_free( t->request_body );
	}
	if( t->response_head ) {
		http_response_head_free( t->response_head );
	}
	if( t->response_body ) {
		http_entitybody_free( t->response_body );
	}
	if( t->in ) {
		net_transport_free( t->in );
	}
	if( t->out ) {
		net_transport_free( t->out );
	}
	tunnel_free( t );
	return OK;
}
// tunnel_over --------------
static status tunnel_over( tunnel_t * t )
{
	if( t->downstream ) {
		tunnel_close_connection( t->downstream );
	}
	if( t->upstream ) {
		tunnel_close_connection( t->upstream );
	}
	tunnel_close_tunnel( t );
	return OK;
}
// tunnel_time_out_connection ---------
static void tunnel_time_out_connection( void * data )
{
	connection_t * c;

	c = data;
	tunnel_close_connection( c );
}
// tunnel_time_out ------
static void tunnel_time_out( void * data )
{
	tunnel_t * t;

	t = data;
	tunnel_over( t );
}
// tunnel_connect_failed_response ------
static status tunnel_connect_failed_response( event_t * ev )
{
	connection_t * down;
	tunnel_t * t;
	status rc;

	down = ev->data;
	t = down->data;
	rc = down->send_chain( down, &t->meta_send );
	if( rc == ERROR ) {
		err_log( "%s --- tunnel connect failed response send failed", __func__ );
		tunnel_over( t );
		return ERROR;
	} else if( rc == DONE ) {
		timer_del( &down->write->timer );
		debug_log ( "%s --- tunnel connect failed response send success", __func__ );
		tunnel_over( t );
		return DONE;
	}
	down->write->timer.data = (void*)t;
	down->write->timer.handler = tunnel_time_out;
	timer_add( &down->write->timer, TUNNEL_TIMEOUT );
	return rc;
}
// tunnel_connect_failed -----
static void tunnel_connect_failed( void* data )
{
	tunnel_t * t;
	connection_t * downstream;

	t = data;
	downstream = t->downstream;
	t->meta_send.start = t->meta_send.pos = tunnel_400_response;
	t->meta_send.end = t->meta_send.last = (tunnel_400_response + l_strlen(tunnel_400_response) );
	event_opt( downstream->write, EVENT_WRITE );
	downstream->write->handler = tunnel_connect_failed_response;
	downstream->write->handler( downstream->write );
}

// tunnel_transport_out_recv ------
static status tunnel_transport_out_recv( event_t * ev )
{
	connection_t * c;
	status rc;
	tunnel_t * t;

	c = ev->data;
	t = c->data;

	rc = net_transport( t->out, 0 );
	if( rc == ERROR ) {
		err_log("%s --- net transport out recv failed", __func__ );
		tunnel_over( t );
		return ERROR;
	}
	t->upstream->read->timer.data = (void*)t;
	t->upstream->read->timer.handler = tunnel_time_out;
	timer_add( &t->upstream->read->timer, TUNNEL_TIMEOUT );
	return rc;
}
// tunnel_transport_out_send ------
static status tunnel_transport_out_send( event_t * ev )
{
	connection_t * c;
	status rc;
	tunnel_t * t;

	c = ev->data;
	t = c->data;

	rc = net_transport( t->out, 1 );
	if( rc == ERROR ) {
		err_log("%s --- net transport out send failed", __func__ );
		tunnel_over( t );
		return ERROR;
	}
	t->upstream->read->timer.data = (void*)t;
	t->upstream->read->timer.handler = tunnel_time_out;
	timer_add( &t->upstream->read->timer, TUNNEL_TIMEOUT );
	return rc;
}
// tunnel_transport_in_recv ------
static status tunnel_transport_in_recv( event_t * ev )
{
	connection_t * c;
	status rc;
	tunnel_t * t;

	c = ev->data;
	t = c->data;

	rc = net_transport( t->in, 0 );
	if( rc == ERROR ) {
		err_log("%s --- net transport in recv failed", __func__ );
		tunnel_over( t );
		return ERROR;
	}
	t->downstream->read->timer.data = (void*)t;
	t->downstream->read->timer.handler = tunnel_time_out;
	timer_add( &t->downstream->read->timer, TUNNEL_TIMEOUT );
	return rc;
}
// tunnel_transport_in_send ------
static status tunnel_transport_in_send( event_t * ev )
{
	connection_t * c;
	status rc;
	tunnel_t * t;

	c = ev->data;
	t = c->data;

	rc = net_transport( t->in, 1 );
	if( rc == ERROR ) {
		err_log("%s --- net transport in send failed", __func__ );
		tunnel_over( t );
		return ERROR;
	}
	t->downstream->read->timer.data = (void*)t;
	t->downstream->read->timer.handler = tunnel_time_out;
	timer_add( &t->downstream->read->timer, TUNNEL_TIMEOUT );
	return OK;
}
// https_start ------
static status https_start( event_t * ev )
{
	connection_t * c;
	tunnel_t * t;

	c = ev->data;
	t = c->data;
	if( OK != net_transport_alloc( &t->in ) ) {
		err_log("%s --- net_transport in alloc", __func__ );
		tunnel_over( t );
		return ERROR;
	}
	if( OK != net_transport_alloc( &t->out ) ) {
		err_log("%s --- net_transport out alloc", __func__ );
		tunnel_over( t );
		return ERROR;
	}
	t->in->recv_connection = t->downstream;
	t->in->send_connection = t->upstream;

	t->out->recv_connection = t->upstream;
	t->out->send_connection = t->downstream;

	t->downstream->read->handler = tunnel_transport_in_recv;
	t->upstream->write->handler = tunnel_transport_in_send;

	t->downstream->write->handler = tunnel_transport_out_send;
	t->upstream->read->handler = tunnel_transport_out_recv;

	event_opt( t->upstream->read, EVENT_READ );
	event_opt( t->downstream->write, EVENT_WRITE );
	event_opt( t->downstream->read, EVENT_READ );
	event_opt( t->upstream->write, EVENT_WRITE );
	return AGAIN;
}
// https_connected ------
static status https_connected( event_t * ev )
{
	connection_t * down;
	tunnel_t * t;
	status rc;

	down = ev->data;
	t = down->data;
	rc = down->send_chain( down, &t->meta_send );
	if( rc == ERROR ) {
		err_log( "%s --- https established meta send failed", __func__ );
		tunnel_over( t );
		return ERROR;
	} else if( rc == DONE ) {
		timer_del( &down->write->timer );
		debug_log ( "%s --- 200 established meta send success", __func__ );
		down->read->handler = https_start;
		return down->read->handler( down->read );
	}
	down->write->timer.data = (void*)t;
	down->write->timer.handler = tunnel_time_out;
	timer_add( &down->write->timer, TUNNEL_TIMEOUT );
	return rc;
}
// tunnel_process_finish ---------
static status tunnel_process_finish( tunnel_t * t )
{
	connection_t * down, *up;
	uint32 busy_length = 0;

	down = t->downstream;
	up = t->upstream;
	if( !t->request_head->keepalive_flag ) {
		tunnel_over( t );
		return DONE;
	}
	down->meta->pos = down->meta->last = down->meta->start;
	tunnel_close_connection( up );
	tunnel_close_tunnel( t );
	if( OK != tunnel_alloc( &t ) ) {
		err_log( "%s --- tunnel alloc", __func__ );
		tunnel_close_connection( down );
		return ERROR;
	}
	t->downstream = down;
	down->data = (void*)t;
	if( OK != http_request_head_create( down, &t->request_head ) ) {
		err_log( "%s --- request alloc", __func__ );
		tunnel_over( t );
		return ERROR;
	}
	t->downstream->write->handler = NULL;
	t->downstream->read->handler = tunnel_local_process_request;
	t->downstream->read->handler(t->downstream->read );
	return DONE;
}
// http_body_transport_send_necessary --------
static status http_body_transport_send_necessary( tunnel_t * t )
{
	if( !t->response_body->body_head ) {
		t->response_body->body_last = NULL;
		return ERROR;
	}
	if( t->response_body->body_head == t->response_body->body_last ) {
		if( t->response_body->body_head->pos <
		t->response_body->body_last->last ) {
			return OK;
		} else {
			return ERROR;
		}
	}
	return OK;
}
// http_body_transport -------
static status http_body_transport( tunnel_t * t, uint32 write )
{
	ssize_t rc;
	meta_t * next;
	connection_t * downstream, *upstream;

	downstream = t->downstream;
	upstream = t->upstream;
	while(1) {
		if( write ) {
			while(1) {
				if( OK != http_body_transport_send_necessary( t ) ) {
					if ( t->trans_body_done ) {
						debug_log("%s --- success", __func__ );
						timer_del( &upstream->read->timer );
						tunnel_process_finish( t );
						return DONE;
					}
					return AGAIN;
				}
				rc = downstream->send( downstream, t->response_body->body_head->pos, meta_len( t->response_body->body_head->pos, t->response_body->body_head->last ) );
				if( rc == ERROR ) {
					tunnel_over( t );
					return ERROR;
				} else if( rc == AGAIN ) {
					return AGAIN;
				} else {
					t->response_body->body_head->pos += rc;
				}
				if( t->response_body->body_head->pos == t->response_body->body_head->last ) {
					next = t->response_body->body_head->next;
					meta_free( t->response_body->body_head );
					t->response_body->body_head = next;
				}
			}
		}
		if( !t->trans_body_done ) {
			rc = t->response_body->handler( t->response_body );
			if( rc == ERROR ) {
				tunnel_over( t );
				return ERROR;
			} else if ( rc == DONE ) {
				t->trans_body_done = 1;
			}
		}
		write = 1;
	}
	return OK;
}
// http_local_send -------
static status http_local_send_body( event_t * ev )
{
	connection_t * downstream, *upstream;
	tunnel_t * t;
	status rc;

	downstream = ev->data;
	t = downstream->data;
	upstream = t->upstream;
	rc = http_body_transport( t, 1 );
	if( rc == AGAIN ) {
		upstream->read->timer.data = ( void* )t;
		upstream->read->timer.handler = tunnel_time_out;
		timer_add( &upstream->read->timer, TUNNEL_TIMEOUT );
	}
	return rc;
}
// http_remote_recv_body ------
static status http_remote_recv_body( event_t * ev )
{
	connection_t * upstream;
	tunnel_t *t;
	status rc;

	upstream = ev->data;
	t = upstream->data;
	rc = http_body_transport( t, 0 );
	if( rc == AGAIN ) {
		upstream->read->timer.data = ( void* )t;
		upstream->read->timer.handler = tunnel_time_out;
		timer_add( &upstream->read->timer, TUNNEL_TIMEOUT );
	}
	return rc;
}
// http_local_send_header ------
static status http_local_send_header( event_t * ev )
{
	connection_t * downstream, *upstream;
	tunnel_t * t;
	status rc;

	downstream = ev->data;
	t = downstream->data;
	upstream = t->upstream;
	rc = downstream->send_chain( downstream, &t->response_head->head );
	if ( rc == ERROR ) {
		err_log( "%s --- http local send header", __func__ );
		tunnel_over( t );
		return ERROR;
	} else if ( rc == DONE ) {
		timer_del( &t->downstream->write->timer );
		debug_log("%s --- success", __func__ );
		if( t->response_head->body_type == HTTP_ENTITYBODY_NULL ) {
			debug_log( "%s --- body empty success", __func__ );
			tunnel_process_finish( t );
			return DONE;
		}
		if( OK != http_entitybody_create( upstream, &t->response_body ) ) {
			err_log( "%s --- http entity body create", __func__ );
			tunnel_over( t );
			return ERROR;
		}
		t->response_body->cache = 1;
		t->response_body->body_type = t->response_head->body_type;
		t->response_body->content_length = t->response_head->content_length;

		downstream->write->handler = http_local_send_body;
		upstream->read->handler = http_remote_recv_body;
		return upstream->read->handler( upstream->read );
	}
	downstream->write->timer.data = (void*)t;
	downstream->write->timer.handler = tunnel_time_out;
	timer_add( &downstream->write->timer, TUNNEL_TIMEOUT );
	return rc;
}
// http_remote_recv_header -----
static status http_remote_recv_header( event_t * ev )
{
	int32 rc;
	connection_t * upstream, *downstream;
	tunnel_t * t;

	upstream = ev->data;
	t = upstream->data;
	downstream = t->downstream;
	rc = t->response_head->handler( t->response_head );
	if( rc == ERROR ) {
		err_log( "%s --- http recv response head", __func__ );
		tunnel_over( t );
		return ERROR;
	} else if( rc == DONE ) {
		timer_del( &t->upstream->read->timer );
		debug_log ( "%s --- success", __func__ );
		if( OK != l_atof( t->response_head->http_version.data,
		t->response_head->http_version.len, &t->http_response_version_n ) ) {
			err_log("%s --- get http_version_n", __func__ );
			tunnel_over( t );
			return ERROR;
		}
		if( t->http_response_version_n < 1.0 ) {
			err_log("%s --- http version < 1.0, not support", __func__ );
			tunnel_over( t );
			return ERROR;
		}
		upstream->read->handler = NULL;
		upstream->write->handler = NULL;
		event_opt( t->downstream->write, EVENT_WRITE );
		downstream->write->handler = http_local_send_header;
		return downstream->write->handler( downstream->write );
	}
	upstream->read->timer.data = (void*)t;
	upstream->read->timer.handler = tunnel_time_out;
	timer_add( &upstream->read->timer, TUNNEL_TIMEOUT );
	return rc;
}
// http_remote_send ----------
static status http_remote_send( event_t * ev )
{
	connection_t* upstream;
	tunnel_t * t;
	status rc;

	upstream = ev->data;
	t = upstream->data;
	rc = upstream->send_chain( upstream, &t->meta_send );
	if( rc == ERROR ) {
		err_log( "%s --- http remote send chain", __func__ );
		tunnel_over( t );
		return ERROR;
	} else if ( rc == DONE ) {
		timer_del( &upstream->write->timer );
		debug_log ( "%s --- success", __func__ );
		if( OK != http_response_head_create( upstream, &t->response_head ) ) {
			err_log( "%s --- http response head create", __func__ );
			tunnel_over( t );
			return ERROR;
		}
		upstream->write->handler = NULL;
		event_opt( upstream->read, EVENT_READ );
		upstream->read->handler = http_remote_recv_header;
		return upstream->read->handler( upstream->read );
	}
	upstream->write->timer.data = (void*)t;
	upstream->write->timer.handler = tunnel_time_out;
	timer_add( &upstream->write->timer, TUNNEL_TIMEOUT );
	return AGAIN;
}
// tunnel_start ------
static status tunnel_start( event_t * ev )
{
	tunnel_t * t;
	connection_t* upstream, *downstream;

	upstream = ev->data;
	t = upstream->data;
	downstream = t->downstream;
	if( !t->https ) {
		debug_log("%s --- tunnel http request", __func__ );
		downstream->read->handler = NULL;
		downstream->write->handler = NULL;
		upstream->read->handler = NULL;
		upstream->write->handler = http_remote_send;
		return upstream->write->handler( upstream->write );
	}
	debug_log("%s --- tunnel https request", __func__ );
	t->meta_send.start = t->meta_send.pos = tunnel_200_response;
	t->meta_send.end = t->meta_send.last = (tunnel_200_response + l_strlen(tunnel_200_response) );
	upstream->read->handler = NULL;
	upstream->write->handler = NULL;
	event_opt( downstream->write, EVENT_WRITE );
	downstream->write->handler = https_connected;
	return downstream->write->handler( downstream->write );
}
// tunnel_remote_handshake ------
static status tunnel_remote_handshake( event_t * ev )
{
	tunnel_t * t;
	connection_t* upstream;

	upstream = ev->data;
	t = upstream->data;
	if( ! upstream->ssl->handshaked ) {
		err_log( "%s --- handshake error", __func__ );
		tunnel_over( t );
		return ERROR;
	}
	timer_del( &t->upstream->write->timer );

	t->upstream->recv = ssl_read;
	t->upstream->send = ssl_write;
	t->upstream->recv_chain = NULL;
	t->upstream->send_chain = ssl_write_chain;

	t->upstream->read->handler = NULL;
	t->upstream->write->handler = https_start;
	return t->upstream->write->handler( t->upstream->write );
}
// tunnel_remote_connect_test ------
static status tunnel_remote_connect_test( connection_t * c )
{
	int	err = 0;
    socklen_t  len = sizeof(int);

	if (getsockopt( c->fd, SOL_SOCKET, SO_ERROR, (void *) &err, &len) == -1 ) {
		err = errno;
	}
	if (err) {
		err_log("%s --- remote connect test, [%d]", __func__, errno );
		return ERROR;
	}
	return OK;
}
// tunnel_remote_connect_check ----
static status tunnel_remote_connect_check( event_t * ev )
{
	tunnel_t * t;
	connection_t* upstream;
	status rc;

	upstream = ev->data;
	t = upstream->data;
	if( OK != tunnel_remote_connect_test( t->upstream ) ) {
		err_log( "%s --- tunnel_remote_connect_test failed", __func__ );
		tunnel_over( t );
		return ERROR;
	}
	debug_log ( "%s --- connect success", __func__ );
	net_nodelay( upstream );
	timer_del( &upstream->write->timer );

	if( conf.tunnel_mode == TUNNEL_CLIENT ) {
		t->upstream->ssl_flag = 1;
		if( OK != ssl_create_connection( t->upstream, L_SSL_CLIENT ) ) {
			err_log( "%s --- client upstream ssl create", __func__ );
			tunnel_over( t );
			return ERROR;
		}
		rc = ssl_handshake( t->upstream->ssl );
		if( rc == ERROR ) {
			err_log( "%s --- client upstream ssl handshake", __func__ );
			tunnel_over( t );
			return ERROR;
		} else if ( rc == AGAIN ) {
			upstream->ssl->handler = tunnel_remote_handshake;
			upstream->write->timer.data = (void*)t;
			upstream->write->timer.handler = tunnel_time_out;
			timer_add( &upstream->write->timer, TUNNEL_TIMEOUT );
			return AGAIN;
		}
		return tunnel_remote_handshake( t->upstream->write );
	} else {
		upstream->write->handler = tunnel_start;
		return upstream->write->handler( upstream->write );
	}
}
// tunnel_remote_connect_start ------
static status tunnel_remote_connect_start( event_t * ev )
{
	status rc;
	tunnel_t * t;
	connection_t* upstream;

	upstream = ev->data;
	t = upstream->data;
	rc = event_connect( upstream->write );
	if( rc == ERROR ) {
		err_log( "%s --- connect error", __func__ );
		tunnel_over( t );
		return ERROR;
	}
	upstream->write->handler = tunnel_remote_connect_check;
	event_opt( upstream->write, EVENT_WRITE );

	if( rc == AGAIN ) {
		upstream->write->timer.data = (void*)t;
		upstream->write->timer.handler = tunnel_connect_failed;
		timer_add( &upstream->write->timer, TUNNEL_TIMEOUT_CONNECT );
		return AGAIN;
	}
	return upstream->write->handler( upstream->write );
}
// tunnel_remote_get_addr ---------------------------------
static status tunnel_remote_get_addr( tunnel_t * t )
{
	struct addrinfo * res;
	string_t default_port = string("80");
	string_t server_port = string("7324");
	string_t *port = NULL, *ip = NULL;

	if( conf.tunnel_mode == TUNNEL_SERVER ||
		conf.tunnel_mode == TUNNEL_SINGLE ) {
		ip = &t->request_head->host;
		port = t->request_head->port.len ? &t->request_head->port : &default_port;
	}
	if( conf.tunnel_mode == TUNNEL_CLIENT ) {
		ip = &conf.serverip;
		port = &server_port;
	}
	debug_log ( "%s --- port [%.*s]", __func__, port->len, port->data );
	debug_log ( "%s --- host [%.*s]", __func__, ip->len, ip->data );
	res = net_get_addr( ip, port );
	if( NULL == res ) {
		err_log( "%s --- net_get_addr error", __func__ );
		return ERROR;
	}
	memset( &t->upstream->addr, 0, sizeof(struct sockaddr_in) );
	memcpy( &t->upstream->addr, res->ai_addr, sizeof(struct sockaddr_in) );
	freeaddrinfo( res );
	return OK;
}
// tunnel_remote_init ------
static status tunnel_remote_init ( event_t * ev )
{
	connection_t* downstream;
	tunnel_t * t;

	downstream = ev->data;
	t = downstream->data;
	if( OK != net_alloc( &t->upstream ) ) {
		debug_log ( "%s --- upstream alloc", __func__ );
		tunnel_over( t );
		return ERROR;
	}
	t->upstream->send = sends;
	t->upstream->recv = recvs;
	t->upstream->send_chain = send_chains;
	t->upstream->recv_chain = NULL;
	t->upstream->data = (void*)t;

	if( !t->upstream->meta ) {
		if( OK != meta_alloc( &t->upstream->meta, TUNNEL_HEADER_LENGTH ) ) {
			err_log( "%s --- upstream meta alloc", __func__ );
			tunnel_over( t );
			return ERROR;
		}
	}
	/*
	FIX ME, "getaddrinfo" is blocking function, that will cause program blocking sometime.
	it should use dns analysis.
	*/
	debug_log("%s --- resolve domain name", __func__ );
	t->upstream->write->timer.data = (void*)t;
	t->upstream->write->timer.handler = tunnel_connect_failed;
	timer_add( &t->upstream->write->timer, TUNNEL_TIMEOUT_CONNECT );
	if( OK != tunnel_remote_get_addr( t ) ) {
		err_log( "%s --- tunnel_remote_get_addr failed", __func__ );
		tunnel_over( t );
		return ERROR;
	}
	debug_log("%s --- resolve domain name over", __func__ );
	timer_del( &t->upstream->write->timer );

	downstream->read->handler = NULL;
	downstream->write->handler = NULL;

	t->upstream->read->handler = NULL;
	t->upstream->write->handler = tunnel_remote_connect_start;
	return t->upstream->write->handler( t->upstream->write );
}
// tunnel_local_process_body ----------
static status tunnel_local_process_body( event_t * ev )
{
	int32 status;
	connection_t * downstream;
	tunnel_t * t;

	downstream = ev->data;
	t = downstream->data;
	status = t->request_body->handler( t->request_body );
	if( status == ERROR ) {
		err_log( "%s --- tunnel request_body handler", __func__ );
		tunnel_over( t );
		return ERROR;
	} else if( status == DONE ) {
		timer_del( &downstream->read->timer );
		t->meta_send.next = t->request_body->body_head;
		downstream->read->handler = tunnel_remote_init;
		return downstream->read->handler( downstream->read );
	}
	downstream->read->timer.data = (void*)t;
	downstream->read->timer.handler = tunnel_time_out;
	timer_add( &downstream->read->timer, TUNNEL_TIMEOUT );
	return status;
}
// tunnel_local_process_procotol -------
static status tunnel_local_process_procotol( event_t * ev )
{
	connection_t * downstream;
	tunnel_t * t;

	downstream = ev->data;
	t = downstream->data;
	if( t->https ) {
		// connect request have't body
		if( downstream->meta->pos != downstream->meta->last ) {
			err_log("%s --- connect request have body", __func__ );
			tunnel_over( t );
			return ERROR;
		}
		downstream->read->handler = tunnel_remote_init;
	} else {
		// http request, continue recv request body
		t->meta_send.pos = downstream->meta->start;
		t->meta_send.last = downstream->meta->pos;
		t->meta_send.next = NULL;
		if( t->request_head->body_type == HTTP_ENTITYBODY_NULL ) {
			downstream->read->handler = tunnel_remote_init;
		} else {
			if( OK != http_entitybody_create( downstream, &t->request_body ) ) {
				err_log( "%s --- downstream request body create", __func__ );
				tunnel_over( t );
				return ERROR;
			}
			t->request_body->cache = 1;
			t->request_body->body_type = t->request_head->body_type;
			t->request_body->content_length = t->request_head->content_length;
			downstream->read->handler = tunnel_local_process_body;
		}
	}
	return downstream->read->handler( downstream->read );
}
// tunnel_local_process_request ---------
static status tunnel_local_process_request( event_t * ev )
{
	connection_t * downstream;
	tunnel_t * t;
	status rc;

	downstream = ev->data;
	t = downstream->data;
	rc = t->request_head->handler( t->request_head );
	if( rc == ERROR ) {
		err_log("%s --- request_head handler failed", __func__ );
		tunnel_over( t );
		return ERROR;
	} else if ( rc == DONE ) {
		timer_del( &downstream->read->timer );
		if( OK == l_strncmp_cap( t->request_head->method.data, t->request_head->method.len, "CONNECT", l_strlen("CONNECT") ) ) {
			t->https = 1;
		}
		downstream->read->handler = tunnel_local_process_procotol;
		return downstream->read->handler( downstream->read );
	}
	downstream->read->timer.data = (void*)t;
	downstream->read->timer.handler = tunnel_time_out;
	timer_add( &downstream->read->timer, TUNNEL_TIMEOUT );
	return AGAIN;
}
// tunnel_local_start ---------
static status tunnel_local_start ( event_t * ev )
{
	connection_t* downstream;
	tunnel_t * t;

	downstream = ev->data;
	timer_del( &downstream->read->timer );
	if( !downstream->meta ) {
		if( OK != meta_alloc( &downstream->meta, TUNNEL_HEADER_LENGTH ) ) {
			err_log( "%s --- downstream header meta alloc", __func__ );
			tunnel_close_connection( downstream );
			return ERROR;
		}
	}
	if( OK != tunnel_alloc( &t ) ) {
		err_log( "%s --- tunnel alloc", __func__ );
		tunnel_close_connection( downstream );
		return ERROR;
	}
	t->downstream = downstream;
	downstream->data = (void*)t;

	switch( conf.tunnel_mode ) {
		case TUNNEL_CLIENT:
			downstream->read->handler = tunnel_remote_init;
			break;
		case TUNNEL_SERVER:
		case TUNNEL_SINGLE:
			downstream->read->handler = tunnel_local_process_request;
			if( OK != http_request_head_create( downstream, &t->request_head ) ) {
				err_log( "%s --- downstream request_head alloc", __func__ );
				tunnel_over( t );
				return ERROR;
			}
			break;
		default:;
	}
	return downstream->read->handler( downstream->read );
}
// tunnel_local_handshake ---------
static status tunnel_local_handshake( event_t * ev )
{
	connection_t * downstream;

	downstream = ev->data;
	if( !downstream->ssl->handshaked ) {
		err_log( "%s --- downstream handshake error", __func__ );
		tunnel_close_connection( downstream );
		return ERROR;
	}
	timer_del( &downstream->read->timer );
	downstream->recv = ssl_read;
	downstream->send = ssl_write;
	downstream->recv_chain = NULL;
	downstream->send_chain = ssl_write_chain;

	downstream->write->handler = NULL;
	downstream->read->handler = tunnel_local_start;
	return downstream->read->handler( downstream->read );
}
// tunnel_local_init ---------
static status tunnel_local_init( event_t * ev )
{
	connection_t * downstream;
	status rc;

	downstream = ev->data;
	if( downstream->ssl_flag ) {
		if( OK != ssl_create_connection( downstream, L_SSL_SERVER ) ) {
			err_log( "%s --- downstream ssl create", __func__ );
			tunnel_close_connection( downstream );
			return ERROR;
		}
		rc = ssl_handshake( downstream->ssl );
		if( rc == ERROR ) {
			err_log( "%s -- downstream ssl handshake", __func__ );
			tunnel_close_connection( downstream );
			return ERROR;
		} else if ( rc == AGAIN ) {
			downstream->ssl->handler = tunnel_local_handshake;
			downstream->read->timer.data = (void*)downstream;
			downstream->read->timer.handler = tunnel_time_out_connection;
			timer_add( &downstream->read->timer, TUNNEL_TIMEOUT );
			return AGAIN;
		}
		return tunnel_local_handshake( downstream->read );
	}
	downstream->read->handler = tunnel_local_start;
	return downstream->read->handler( downstream->read );
}
// tunnel_process_init ---------
status tunnel_process_init( void )
{
	uint32 i;

	queue_init( &usable );
	queue_init( &in_use );
	pool = ( tunnel_t * ) l_safe_malloc( sizeof(tunnel_t)*MAXCON );
    if( !pool ) {
        err_log("%s --- l_safe_malloc pool", __func__ );
        return ERROR;
    }
    memset( pool, 0, sizeof(tunnel_t)*MAXCON );
    for( i = 0; i < MAXCON; i ++ ) {
        queue_insert_tail( &usable, &pool[i].queue );
    }
	return OK;
}
// tunnel_process_end ---------
status tunnel_process_end( void )
{
	if( pool ) {
        l_safe_free( pool );
    }
    pool = NULL;
	return OK;
}
// tunnel_init ---------
status tunnel_init( void )
{
	if( conf.tunnel_mode == TUNNEL_CLIENT ||
		conf.tunnel_mode == TUNNEL_SINGLE ) {
		listen_add( 7325, tunnel_local_init, TCP );
	} else if ( conf.tunnel_mode == TUNNEL_SERVER ) {
		listen_add( 7324, tunnel_local_init, HTTPS );
	}
	return OK;
}
// tunnel_end ------
status tunnel_end( void )
{
	return OK;
}
