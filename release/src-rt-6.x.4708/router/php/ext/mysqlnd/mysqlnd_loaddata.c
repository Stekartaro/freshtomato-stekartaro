/*
  +----------------------------------------------------------------------+
  | PHP Version 7                                                        |
  +----------------------------------------------------------------------+
  | Copyright (c) 2006-2018 The PHP Group                                |
  +----------------------------------------------------------------------+
  | This source file is subject to version 3.01 of the PHP license,      |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.php.net/license/3_01.txt                                  |
  | If you did not receive a copy of the PHP license and are unable to   |
  | obtain it through the world-wide-web, please send a note to          |
  | license@php.net so we can mail you a copy immediately.               |
  +----------------------------------------------------------------------+
  | Authors: Andrey Hristov <andrey@php.net>                             |
  |          Ulf Wendel <uw@php.net>                                     |
  |          Georg Richter <georg@php.net>                               |
  +----------------------------------------------------------------------+
*/
#include "php.h"
#include "mysqlnd.h"
#include "mysqlnd_wireprotocol.h"
#include "mysqlnd_priv.h"
#include "mysqlnd_debug.h"

/* {{{ mysqlnd_local_infile_init */
static
int mysqlnd_local_infile_init(void ** ptr, const char * const filename)
{
	MYSQLND_INFILE_INFO	*info;
	php_stream_context	*context = NULL;

	DBG_ENTER("mysqlnd_local_infile_init");

	info = ((MYSQLND_INFILE_INFO *)mnd_ecalloc(1, sizeof(MYSQLND_INFILE_INFO)));
	if (!info) {
		DBG_RETURN(1);
	}

	*ptr = info;

	/* check open_basedir */
	if (PG(open_basedir)) {
		if (php_check_open_basedir_ex(filename, 0) == -1) {
			strcpy(info->error_msg, "open_basedir restriction in effect. Unable to open file");
			info->error_no = CR_UNKNOWN_ERROR;
			DBG_RETURN(1);
		}
	}

	info->filename = filename;
	info->fd = php_stream_open_wrapper_ex((char *)filename, "r", 0, NULL, context);

	if (info->fd == NULL) {
		snprintf((char *)info->error_msg, sizeof(info->error_msg), "Can't find file '%-.64s'.", filename);
		info->error_no = MYSQLND_EE_FILENOTFOUND;
		DBG_RETURN(1);
	}

	DBG_RETURN(0);
}
/* }}} */


/* {{{ mysqlnd_local_infile_read */
static
int mysqlnd_local_infile_read(void * ptr, zend_uchar * buf, unsigned int buf_len)
{
	MYSQLND_INFILE_INFO	*info = (MYSQLND_INFILE_INFO *)ptr;
	int count;

	DBG_ENTER("mysqlnd_local_infile_read");

	count = (int) php_stream_read(info->fd, (char *) buf, buf_len);

	if (count < 0) {
		strcpy(info->error_msg, "Error reading file");
		info->error_no = CR_UNKNOWN_ERROR;
	}

	DBG_RETURN(count);
}
/* }}} */


/* {{{ mysqlnd_local_infile_error */
static
int	mysqlnd_local_infile_error(void * ptr, char *error_buf, unsigned int error_buf_len)
{
	MYSQLND_INFILE_INFO	*info = (MYSQLND_INFILE_INFO *)ptr;

	DBG_ENTER("mysqlnd_local_infile_error");

	if (info) {
		strlcpy(error_buf, info->error_msg, error_buf_len);
		DBG_INF_FMT("have info, %d", info->error_no);
		DBG_RETURN(info->error_no);
	}

	strlcpy(error_buf, "Unknown error", error_buf_len);
	DBG_INF_FMT("no info, %d", CR_UNKNOWN_ERROR);
	DBG_RETURN(CR_UNKNOWN_ERROR);
}
/* }}} */


/* {{{ mysqlnd_local_infile_end */
static
void mysqlnd_local_infile_end(void * ptr)
{
	MYSQLND_INFILE_INFO	*info = (MYSQLND_INFILE_INFO *)ptr;

	if (info) {
		/* php_stream_close segfaults on NULL */
		if (info->fd) {
			php_stream_close(info->fd);
			info->fd = NULL;
		}
		mnd_efree(info);
	}
}
/* }}} */


/* {{{ mysqlnd_local_infile_default */
PHPAPI void
mysqlnd_local_infile_default(MYSQLND_CONN_DATA * conn)
{
	conn->infile.local_infile_init = mysqlnd_local_infile_init;
	conn->infile.local_infile_read = mysqlnd_local_infile_read;
	conn->infile.local_infile_error = mysqlnd_local_infile_error;
	conn->infile.local_infile_end = mysqlnd_local_infile_end;
}
/* }}} */


static const char *lost_conn = "Lost connection to MySQL server during LOAD DATA of a local file";


/* {{{ mysqlnd_handle_local_infile */
enum_func_status
mysqlnd_handle_local_infile(MYSQLND_CONN_DATA * conn, const char * const filename, zend_bool * is_warning)
{
	zend_uchar			*buf = NULL;
	zend_uchar			empty_packet[MYSQLND_HEADER_SIZE];
	enum_func_status	result = FAIL;
	unsigned int		buflen = 4096;
	void				*info = NULL;
	int					bufsize;
	size_t				ret;
	MYSQLND_INFILE		infile;
	MYSQLND_PFC			* net = conn->protocol_frame_codec;
	MYSQLND_VIO			* vio = conn->vio;

	DBG_ENTER("mysqlnd_handle_local_infile");

	if (!(conn->options->flags & CLIENT_LOCAL_FILES)) {
		php_error_docref(NULL, E_WARNING, "LOAD DATA LOCAL INFILE forbidden");
		SET_CLIENT_ERROR(conn->error_info, CR_UNKNOWN_ERROR, UNKNOWN_SQLSTATE,
						"LOAD DATA LOCAL INFILE is forbidden, check mysqli.allow_local_infile");
		/* write empty packet to server */
		ret = net->data->m.send(net, vio, empty_packet, 0, conn->stats, conn->error_info);
		*is_warning = TRUE;
		goto infile_error;
	}

	infile = conn->infile;
	/* allocate buffer for reading data */
	buf = (zend_uchar *) mnd_ecalloc(1, buflen);

	*is_warning = FALSE;

	/* init handler: allocate read buffer and open file */
	if (infile.local_infile_init(&info, (char *)filename)) {
		char tmp_buf[sizeof(conn->error_info->error)];
		int tmp_error_no;
		*is_warning = TRUE;
		/* error occurred */
		tmp_error_no = infile.local_infile_error(info, tmp_buf, sizeof(tmp_buf));
		SET_CLIENT_ERROR(conn->error_info, tmp_error_no, UNKNOWN_SQLSTATE, tmp_buf);
		/* write empty packet to server */
		ret = net->data->m.send(net, vio, empty_packet, 0, conn->stats, conn->error_info);
		goto infile_error;
	}

	/* read data */
	while ((bufsize = infile.local_infile_read (info, buf + MYSQLND_HEADER_SIZE, buflen - MYSQLND_HEADER_SIZE)) > 0) {
		if ((ret = net->data->m.send(net, vio, buf, bufsize, conn->stats, conn->error_info)) == 0) {
			DBG_ERR_FMT("Error during read : %d %s %s", CR_SERVER_LOST, UNKNOWN_SQLSTATE, lost_conn);
			SET_CLIENT_ERROR(conn->error_info, CR_SERVER_LOST, UNKNOWN_SQLSTATE, lost_conn);
			goto infile_error;
		}
	}

	/* send empty packet for eof */
	if ((ret = net->data->m.send(net, vio, empty_packet, 0, conn->stats, conn->error_info)) == 0) {
		SET_CLIENT_ERROR(conn->error_info, CR_SERVER_LOST, UNKNOWN_SQLSTATE, lost_conn);
		goto infile_error;
	}

	/* error during read occurred */
	if (bufsize < 0) {
		char tmp_buf[sizeof(conn->error_info->error)];
		int tmp_error_no;
		*is_warning = TRUE;
		DBG_ERR_FMT("Bufsize < 0, warning,  %d %s %s", CR_SERVER_LOST, UNKNOWN_SQLSTATE, lost_conn);
		tmp_error_no = infile.local_infile_error(info, tmp_buf, sizeof(tmp_buf));
		SET_CLIENT_ERROR(conn->error_info, tmp_error_no, UNKNOWN_SQLSTATE, tmp_buf);
		goto infile_error;
	}

	result = PASS;

infile_error:
	/* get response from server and update upsert values */
	if (FAIL == conn->payload_decoder_factory->m.send_command_handle_response(
											conn->payload_decoder_factory,
											PROT_OK_PACKET, FALSE, COM_QUERY, FALSE,
											conn->error_info,
											conn->upsert_status,
											&conn->last_message,
											conn->persistent)) {
		result = FAIL;
	}

	(*conn->infile.local_infile_end)(info);
	if (buf) {
		mnd_efree(buf);
	}
	DBG_INF_FMT("%s", result == PASS? "PASS":"FAIL");
	DBG_RETURN(result);
}
/* }}} */

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */
