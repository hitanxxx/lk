#include "lk.h"

config_t conf;

// config_get ----
status config_get ( meta_t ** meta, char * path )
{
	int fd;
	struct stat stconf;
	uint32 length;
	meta_t * new;

	if( stat( path, &stconf ) < 0 ) {
		err_log( "%s --- config.json not found", __func__  );
		return ERROR;
	}
	length = (uint32)stconf.st_size;
	if( length > CONF_SETTING_LENGTH ) {
		err_log( "%s --- config.json too big, limit [32768byte]" );
		return ERROR;
	}
	if( OK != meta_alloc( &new, length ) ) {
		err_log("%s --- meta alloc", __func__ );
		return ERROR;
	}
	fd = open( path, O_RDONLY );
	if( ERROR == fd ) {
		err_log("%s --- can't open config.json", __func__ );
		meta_free( new );
		return ERROR;
	}
	if( ERROR == read( fd, new->last, length ) ) {
		err_log("%s --- read config.json", __func__ );
		meta_free( new );
		close( fd );
		return ERROR;
	}
	new->last += length;
	close(fd);
	*meta = new;
	return OK;
}
// config_parse_global ----------
static status config_parse_global( json_node_t * json )
{
	char str[1024] = {0};
	json_node_t *root_obj, *v;

	json_get_child( json, 1, &root_obj );
	if( OK == json_get_obj_bool(root_obj, "daemon", l_strlen("daemon"), &v ) ) {
		conf.daemon = (v->type == JSON_TRUE) ? 1 : 0;
	}
	if ( OK == json_get_obj_num(root_obj, "worker_process", l_strlen("worker_process"), &v ) ) {
		conf.worker_process = (uint32)v->num;
		if( conf.worker_process > MAXPROCESS ) {
			err_log("%s --- work_procrss too much, [%d]", __func__, conf.worker_process );
			return ERROR;
		}
	}
	if( OK == json_get_obj_bool(root_obj, "reuse_port", l_strlen("reuse_port"), &v ) ) {
		conf.reuse_port = (v->type == JSON_TRUE) ? 1 : 0;
	}
	if( OK == json_get_obj_bool(root_obj, "accept_mutex", l_strlen("accept_mutex"), &v ) ) {
		conf.accept_mutex = (v->type == JSON_TRUE) ? 1 : 0;
	}
	if( OK == json_get_obj_str(root_obj, "sslcrt", l_strlen("sslcrt"), &v ) ) {
		if( v->name.len > L_SSL_CERT_PATH_LEN ) {
			err_log("%s --- ssl cert file path to long, limit = 128", __func__ );
			return ERROR;
		}
		conf.sslcrt.data = v->name.data;
		conf.sslcrt.len = v->name.len;
		memset( str, 0, sizeof(str) );
		l_memcpy( str, conf.sslcrt.data, conf.sslcrt.len );
		if( OK != access( str, F_OK ) ) {
			err_log("%s --- sslcrt file [%.*s] not found", __func__,
			conf.sslcrt.len, conf.sslcrt.data );
			conf.sslcrt.len = 0;
		}
	}
	if( OK == json_get_obj_str(root_obj, "sslkey", l_strlen("sslkey"), &v ) ) {
		if( v->name.len > L_SSL_KEY_PATH_LEN ) {
			err_log("%s --- ssl cert file path to long, limit = 128", __func__ );
			return ERROR;
		}
		conf.sslkey.data = v->name.data;
		conf.sslkey.len = v->name.len;
		memset( str, 0, sizeof(str) );
		l_memcpy( str, conf.sslkey.data, conf.sslkey.len );
		if( OK != access( str, F_OK ) ) {
			err_log("%s --- sslkey file [%.*s] not found", __func__,
		 	conf.sslkey.len, conf.sslkey.data );
			conf.sslkey.len = 0;
		}
	}
	if( OK == json_get_obj_bool(root_obj, "log_error", l_strlen("log_error"), &v ) ) {
		conf.log_error = (v->type == JSON_TRUE) ? 1 : 0;
	}
	if( OK == json_get_obj_bool(root_obj, "log_debug", l_strlen("log_debug"), &v ) ) {
		conf.log_debug = (v->type == JSON_TRUE) ? 1 : 0;
	}
	return OK;
}
// config_parse_http -----------
static status config_parse_http( json_node_t * json )
{
	char str[1024] = {0};
	json_node_t * root_obj, *http_obj, *arr, *v;
	queue_t * q;

	json_get_child( json, 1, &root_obj );
	if( OK ==  json_get_obj_obj(root_obj, "http", l_strlen("http"), &http_obj ) ) {
		if( OK == json_get_obj_bool(http_obj, "access_log", l_strlen("access_log"), &v ) ) {
			conf.http_access_log = (v->type == JSON_TRUE) ? 1 : 0;
		}
		if( OK == json_get_obj_bool(http_obj, "keepalive", l_strlen("keepalive"), &v ) ) {
			conf.http_keepalive = (v->type == JSON_TRUE) ? 1 : 0;
		}
		if( OK == json_get_obj_str(http_obj, "home", l_strlen("home"), &v ) ) {
			conf.home.data = v->name.data;
			conf.home.len = v->name.len;
			memset( str, 0, sizeof(str) );
			l_memcpy( str, conf.home.data, conf.home.len );
			if( OK != access( str, F_OK ) ) {
				err_log("%s --- home [%.*s] not found", __func__,
			 	conf.home.len, conf.home.data );
				conf.home.len = 0;
			}
		}
		if( OK == json_get_obj_str(http_obj, "index", l_strlen("index"), &v ) ) {
			conf.index.data = v->name.data;
			conf.index.len = v->name.len;
		}
		if( OK == json_get_obj_arr(http_obj, "http_listen", l_strlen("http_listen"), &arr ) ) {
			if( !conf.home.len ) {
				err_log("%s --- http need specify a valid 'home' and 'index'", __func__ );
				return ERROR;
			}
			for( q = queue_head(&arr->child); q != queue_tail( &arr->child ); q = queue_next(q) ) {
				v = l_get_struct( q, json_node_t, queue );
				if( v->type != JSON_NUM ) {
					err_log("%s --- http listen must be number", __func__ );
					return ERROR;
				}
				conf.http[conf.http_n++] = (uint32)v->num;
			}
		}
		if( OK == json_get_obj_arr(http_obj, "https_listen", l_strlen("https_listen"), &arr ) ) {
			if( !conf.home.len ) {
				err_log("%s --- https need specify a valid 'home' and 'index'", __func__ );
				return ERROR;
			}
			if( !conf.sslcrt.len || !conf.sslkey.len ) {
				err_log("%s --- https need specify a valid 'sslcrt' and 'sslkey'", __func__ );
				return ERROR;
			}
			for( q = queue_head(&arr->child); q != queue_tail( &arr->child ); q = queue_next(q) ) {
				v = l_get_struct( q, json_node_t, queue );
				if( v->type != JSON_NUM ) {
					err_log("%s --- https listen must be number", __func__ );
					return ERROR;
				}
				conf.https[conf.https_n++] = (uint32)v->num;
			}
		}
	}
	return OK;
}
// config_parse_tunnel -------------
static status config_parse_tunnel( json_node_t * json )
{
	json_node_t * root_obj, *tunnel_obj, *v;
	status rc;

	json_get_child( json, 1, &root_obj );
	if( OK == json_get_obj_obj(root_obj, "tunnel", l_strlen("tunnel"), &tunnel_obj ) ) {
		rc = json_get_obj_str(tunnel_obj, "mode", l_strlen("mode"), &v );
		if( rc == ERROR ) {
			err_log("%s --- tunnel need specify a valid 'mode'", __func__ );
			return ERROR;
		}
		// server
		// client
		// single
		if( OK == l_strncmp_cap( v->name.data, v->name.len, "single", l_strlen("single") ) ) {
			conf.tunnel_mode = TUNNEL_SINGLE;
		} else if ( OK == l_strncmp_cap( v->name.data, v->name.len, "server", l_strlen("server") ) ) {
			conf.tunnel_mode = TUNNEL_SERVER;
		} else if ( OK == l_strncmp_cap( v->name.data, v->name.len, "client", l_strlen("client") ) ) {
			conf.tunnel_mode = TUNNEL_CLIENT;
			rc = json_get_obj_str(tunnel_obj, "serverip", l_strlen("serverip"), &v );
			if( rc == ERROR ) {
				err_log("%s --- tunnel mode client need specify a valid 'serverip'", __func__ );
				return ERROR;
			}
			conf.serverip.data = v->name.data;
			conf.serverip.len = v->name.len;
		} else {
			err_log("%s --- tunnel invalid 'mode' [%.*s]", __func__,
			v->name.len, v->name.data );
			return ERROR;
		}
	}
	return OK;
}
// config_parse_perf -------------
static status config_parse_perf( json_node_t * json )
{
	json_node_t * root_obj, *perf_obj, *v;
	status rc;

	json_get_child( json, 1, &root_obj );

	if( OK == json_get_obj_obj(root_obj, "perf", l_strlen("perf"), &perf_obj ) ) {
		rc = json_get_obj_bool(perf_obj, "switch", l_strlen("switch"), &v );
		if( rc == ERROR ) {
			err_log("%s --- tunnel need specify a valid 'switch'", __func__ );
			return ERROR;
		}
		conf.perf_switch = (v->type == JSON_TRUE) ? 1 : 0;
	}
	return OK;
}
// config_parse_lktp --------
static status config_parse_lktp( json_node_t * json )
{
	json_node_t * root_obj, *lktp_obj, *v;
	status rc;

	json_get_child( json, 1, &root_obj );

	if( OK == json_get_obj_obj(root_obj, "lktp", l_strlen("lktp"), &lktp_obj ) ) {
		rc = json_get_obj_str(lktp_obj, "mode", l_strlen("mode"), &v );
		if( rc == ERROR ) {
			err_log("%s --- lktp need specify a valid 'mode'", __func__ );
			return ERROR;
		}
		if( OK == l_strncmp_cap( v->name.data, v->name.len, "server", l_strlen("server") ) ) {
			conf.lktp_mode = LKTP_SERVER;
		} else if ( OK == l_strncmp_cap( v->name.data, v->name.len, "client", l_strlen("client") ) ) {
			conf.lktp_mode = LKTP_CLIENT;
			if( OK != json_get_obj_str(lktp_obj, "serverip", l_strlen("serverip"), &v ) ) {
				err_log("%s --- lktp client need specify server ip", __func__ );
				return ERROR;
			}
			conf.lktp_serverip.data = v->name.data;
			conf.lktp_serverip.len = v->name.len;
		} else {
			err_log("%s --- not support lktp mode, [%.*s]", __func__, v->name.len, v->name.data );
			return ERROR;
		}
	}
	return OK;
}
// config_parse_socks5 -------------
static status config_parse_socks5( json_node_t * json )
{
	json_node_t * root_obj, *socks5_obj, *v;
	status rc;

	json_get_child( json, 1, &root_obj );
	if( OK == json_get_obj_obj(root_obj, "socks5", l_strlen("socks5"), &socks5_obj ) ) {
		rc = json_get_obj_str(socks5_obj, "mode", l_strlen("mode"), &v );
		if( rc == ERROR ) {
			err_log("%s --- tunnel need specify a valid 'mode'", __func__ );
			return ERROR;
		}
		// server
		// client
		if ( OK == l_strncmp_cap( v->name.data, v->name.len, "server", l_strlen("server") ) ) {
			conf.socks5_mode = SOCKS5_SERVER;
		} else if ( OK == l_strncmp_cap( v->name.data, v->name.len, "client", l_strlen("client") ) ) {
			conf.socks5_mode = SOCKS5_CLIENT;
			rc = json_get_obj_str(socks5_obj, "serverip", l_strlen("serverip"), &v );
			if( rc == ERROR ) {
				err_log("%s --- tunnel mode client need specify a valid 'serverip'", __func__ );
				return ERROR;
			}
			conf.socks5_serverip.data = v->name.data;
			conf.socks5_serverip.len = v->name.len;
		} else {
			err_log("%s --- tunnel invalid 'mode' [%.*s]", __func__,
			v->name.len, v->name.data );
			return ERROR;
		}
	}
	return OK;
}
// config_parse -----
static status config_parse( json_node_t * json )
{
	if( OK != config_parse_global( json ) ) {
		err_log( "%s --- parse global", __func__ );
		return ERROR;
	}
	if( OK != config_parse_http( json ) ) {
		err_log("%s ---  parse http ", __func__ );
		return ERROR;
	}
	if( OK != config_parse_tunnel( json ) ) {
		err_log( "%s --- parse tunnel", __func__  );
		return ERROR;
	}
	if( OK != config_parse_perf( json ) ) {
		err_log( "%s --- parse perf", __func__ );
		return ERROR;
	}
	if( OK != config_parse_lktp( json ) ) {
		err_log( "%s --- parse lktp", __func__ );
		return ERROR;
	}
	if( OK != config_parse_socks5( json ) ) {
		err_log("%s --- parse socks5", __func__ );
		return ERROR;
	}
	return OK;
}
// config_start ----
static status config_start( void )
{
	json_ctx_t * ctx = NULL;

	if( OK != config_get( &conf.meta, L_PATH_CONFIG ) ) {
		err_log( "%s --- configuration file open", __func__  );
		goto failed;
	}
	debug_log( "%s --- configuration file:\n[%.*s]", __func__, meta_len( conf.meta->pos, conf.meta->last ), conf.meta->pos);
	if( OK != json_ctx_create( &ctx ) ) {
		err_log("%s --- json ctx create", __func__ );
		goto failed;
	}
	if( OK != json_decode( ctx, conf.meta->pos, conf.meta->last ) ) {
		err_log("%s --- configuration file decode failed", __func__ );
		goto failed;
	}
	if( OK != config_parse( &ctx->root ) ) {
		err_log("%s --- config parse failed", __func__ );
		goto failed;
	}
	json_ctx_free( ctx );
	return OK;
failed:
	if( conf.meta ) {
		meta_free( conf.meta );
	}
	conf.meta = NULL;
	if( ctx ) {
		json_ctx_free( ctx );
	}
	ctx = NULL;
	return ERROR;
}
// config_init ----
status config_init ( void )
{
	memset( &conf, 0, sizeof(config_t) );
	conf.log_debug = 1;
	conf.log_error = 1;

	if( OK != config_start(  ) ) {
		return ERROR;
	}
	return OK;
}
// config_end -----
status config_end ( void )
{
	if( conf.meta ) {
		meta_free( conf.meta );
	}
	return OK;
}
