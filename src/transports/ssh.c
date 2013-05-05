/*
 * Copyright (C) the libgit2 contributors. All rights reserved.
 *
 * This file is part of libgit2, distributed under the GNU GPL v2 with
 * a Linking Exception. For full terms see the included COPYING file.
 */

#include "git2.h"
#include "buffer.h"
#include "netops.h"
#include "smart.h"

#include <libssh2.h>

#define OWNING_SUBTRANSPORT(s) ((ssh_subtransport *)(s)->parent.subtransport)

static const char prefix_ssh[] = "ssh://";
static const char default_user[] = "git";
static const char cmd_uploadpack[] = "git-upload-pack";
static const char cmd_receivepack[] = "git-receive-pack";

typedef struct {
	git_smart_subtransport_stream parent;
	gitno_socket socket;
	LIBSSH2_SESSION *session;
	LIBSSH2_CHANNEL *channel;
	const char *cmd;
	char *url;
	unsigned sent_command : 1;
} ssh_stream;

typedef struct {
	git_smart_subtransport parent;
	transport_smart *owner;
	ssh_stream *current_stream;
	git_cred *cred;
} ssh_subtransport;

/*
 * Create a git protocol request.
 *
 * For example: git-upload-pack '/libgit2/libgit2'
 */
static int gen_proto(git_buf *request, const char *cmd, const char *url)
{
	char *repo;
	
	if (!git__prefixcmp(url, prefix_ssh)) {
		url = url + strlen(prefix_ssh);
		repo = strchr(url, '/');
	} else {
		repo = strchr(url, ':');
	}
	
	if (!repo) {
		return -1;
	}
	
	int len = strlen(cmd) + 1 /* Space */ + 1 /* Quote */ + strlen(repo) + 1 /* Quote */ + 1;
	
	git_buf_grow(request, len);
	git_buf_printf(request, "%s '%s'", cmd, repo);
	git_buf_putc(request, '\0');
	
	if (git_buf_oom(request))
		return -1;
	
	return 0;
}

static int send_command(ssh_stream *s)
{
	int error;
	git_buf request = GIT_BUF_INIT;
	
	error = gen_proto(&request, s->cmd, s->url);
	if (error < 0)
		goto cleanup;
	
	error = libssh2_channel_process_startup(
		s->channel, 
		"exec", 
		(uint32_t)sizeof("exec") - 1, 
		request.ptr,
		request.size
	);

	if (0 != error)
		goto cleanup;
	
	s->sent_command = 1;
	
cleanup:
	git_buf_free(&request);
	return error;
}

static int ssh_stream_read(
	git_smart_subtransport_stream *stream,
	char *buffer,
	size_t buf_size,
	size_t *bytes_read)
{
	ssh_stream *s = (ssh_stream *)stream;
	
	*bytes_read = 0;
	
	if (!s->sent_command && send_command(s) < 0)
		return -1;
	
	int rc = libssh2_channel_read(s->channel, buffer, buf_size);
	if (rc < 0)
		return -1;
	
	*bytes_read = rc;
	
	return 0;
}

static int ssh_stream_write(
	git_smart_subtransport_stream *stream,
	const char *buffer,
	size_t len)
{
	ssh_stream *s = (ssh_stream *)stream;
	
	if (!s->sent_command && send_command(s) < 0)
		return -1;
	
	int rc = libssh2_channel_write(s->channel, buffer, len);
	if (rc < 0) {
		return -1;
	}
	
	return rc;
}

static void ssh_stream_free(git_smart_subtransport_stream *stream)
{
	ssh_stream *s = (ssh_stream *)stream;
	ssh_subtransport *t = OWNING_SUBTRANSPORT(s);
	int ret;
	
	GIT_UNUSED(ret);
	
	t->current_stream = NULL;
	
	if (s->channel) {
		libssh2_channel_close(s->channel);
        libssh2_channel_free(s->channel);
        s->channel = NULL;
	}
	
	if (s->session) {
		libssh2_session_free(s->session), s->session = NULL;
	}
	
	if (s->socket.socket) {
		ret = gitno_close(&s->socket);
		assert(!ret);
	}
	
	git__free(s->url);
	git__free(s);
}

static int ssh_stream_alloc(
	ssh_subtransport *t,
	const char *url,
	const char *cmd,
	git_smart_subtransport_stream **stream)
{
	ssh_stream *s;
	
	if (!stream)
		return -1;
	
	s = git__calloc(sizeof(ssh_stream), 1);
	GITERR_CHECK_ALLOC(s);
	
	s->parent.subtransport = &t->parent;
	s->parent.read = ssh_stream_read;
	s->parent.write = ssh_stream_write;
	s->parent.free = ssh_stream_free;
	
	s->cmd = cmd;
	s->url = git__strdup(url);
	
	if (!s->url) {
		git__free(s);
		return -1;
	}
	
	*stream = &s->parent;
	return 0;
}

static int git_ssh_extract_url_parts(
	char **host,
	char **username,
	const char *url)
{
	char *colon, *at;
	const char *start;
    
    colon = strchr(url, ':');
	
	if (colon == NULL) {
		giterr_set(GITERR_NET, "Malformed URL: missing :");
		return -1;
	}
	
	start = url;
	at = strchr(url, '@');
	if (at) {
		start = at+1;
		*username = git__substrdup(url, at - url);
	} else {
		*username = git__strdup(default_user);
	}
	
	*host = git__substrdup(start, colon - start);
	
	return 0;
}

static int _git_ssh_authenticate_session(
	LIBSSH2_SESSION* session,
	const char *user,
	git_cred* cred
)
{
	int rc;
	do {
		switch (cred->credtype) {
			case GIT_CREDTYPE_USERPASS_PLAINTEXT: {
				git_cred_userpass_plaintext *c = (git_cred_userpass_plaintext *)cred;
				rc = libssh2_userauth_password(
					session, 
					c->username,
					c->password
				);
				break;
			}
			case GIT_CREDTYPE_SSH_KEYFILE_PASSPHRASE: {
				git_cred_ssh_keyfile_passphrase *c = (git_cred_ssh_keyfile_passphrase *)cred;
				rc = libssh2_userauth_publickey_fromfile(
					session, 
					user,
					c->publickey,
					c->privatekey,
					c->passphrase
				);
				break;
			}
			default:
				rc = -1;
		}
    } while (LIBSSH2_ERROR_EAGAIN == rc || LIBSSH2_ERROR_TIMEOUT == rc);
	
    return rc;
}

static int _git_ssh_setup_conn(
	ssh_subtransport *t,
	const char *url,
	const char *cmd,
	git_smart_subtransport_stream **stream
)
{
	char *host, *port, *user=NULL, *pass=NULL;
	const char *default_port = "22";
	ssh_stream *s;
	
	*stream = NULL;
	if (ssh_stream_alloc(t, url, cmd, stream) < 0)
		return -1;
	
	s = (ssh_stream *)*stream;
	
	if (!git__prefixcmp(url, prefix_ssh)) {
		url = url + strlen(prefix_ssh);
		if (gitno_extract_url_parts(&host, &port, &user, &pass, url, default_port) < 0)
			goto on_error;
	} else {
		if (git_ssh_extract_url_parts(&host, &user, url) < 0)
			goto on_error;
		port = git__strdup(default_port);
	}
	
	if (gitno_connect(&s->socket, host, port, 0) < 0)
		goto on_error;
	
	if (user && pass) {
		git_cred_userpass_plaintext_new(&t->cred, user, pass);
	} else {
		if (t->owner->cred_acquire_cb(&t->cred,
				t->owner->url,
				user,
				GIT_CREDTYPE_USERPASS_PLAINTEXT | GIT_CREDTYPE_SSH_KEYFILE_PASSPHRASE,
				t->owner->cred_acquire_payload) < 0)
			return -1;
	}
	assert(t->cred);
	
	if (!user) {
		user = git__strdup(default_user);
	}
	
	LIBSSH2_SESSION* session = libssh2_session_init();
    if (!session)
        goto on_error;
	
    int rc = 0;
    do {
        rc = libssh2_session_startup(session, s->socket.socket);
    } while (LIBSSH2_ERROR_EAGAIN == rc || LIBSSH2_ERROR_TIMEOUT == rc);
	
	if (0 != rc) {
        goto on_error;
    }
	
	libssh2_trace(session, 0x1FF);
	libssh2_session_set_blocking(session, 1);
	
    if (_git_ssh_authenticate_session(session, user, t->cred) < 0) {
		goto on_error;
	}
	
	LIBSSH2_CHANNEL* channel = NULL;
    do {
        channel = libssh2_channel_open_session(session);
    } while (LIBSSH2_ERROR_EAGAIN == rc || LIBSSH2_ERROR_TIMEOUT == rc);
	
	if (!channel) {
        goto on_error;
    }
	
	libssh2_channel_set_blocking(channel, 1);
	
	s->session = session;
	s->channel = channel;
	
	t->current_stream = s;
	git__free(host);
	return 0;
	
on_error:
	if (*stream)
		ssh_stream_free(*stream);
	
	git__free(host);
	return -1;
}

static int _git_uploadpack_ls(
	ssh_subtransport *t,
	const char *url,
	git_smart_subtransport_stream **stream)
{
	if (_git_ssh_setup_conn(t, url, cmd_uploadpack, stream) < 0)
		return -1;
	
	return 0;
}

static int _git_uploadpack(
	ssh_subtransport *t,
	const char *url,
	git_smart_subtransport_stream **stream)
{
	GIT_UNUSED(url);
	
	if (t->current_stream) {
		*stream = &t->current_stream->parent;
		return 0;
	}
	
	giterr_set(GITERR_NET, "Must call UPLOADPACK_LS before UPLOADPACK");
	return -1;
}

static int _git_receivepack_ls(
	ssh_subtransport *t,
	const char *url,
	git_smart_subtransport_stream **stream)
{
	if (_git_ssh_setup_conn(t, url, cmd_receivepack, stream) < 0)
		return -1;
	
	return 0;
}

static int _git_receivepack(
	ssh_subtransport *t,
	const char *url,
	git_smart_subtransport_stream **stream)
{
	GIT_UNUSED(url);
	
	if (t->current_stream) {
		*stream = &t->current_stream->parent;
		return 0;
	}
	
	giterr_set(GITERR_NET, "Must call RECEIVEPACK_LS before RECEIVEPACK");
	return -1;
}

static int _git_action(
	git_smart_subtransport_stream **stream,
	git_smart_subtransport *subtransport,
	const char *url,
	git_smart_service_t action)
{
	ssh_subtransport *t = (ssh_subtransport *) subtransport;
	
	switch (action) {
		case GIT_SERVICE_UPLOADPACK_LS:
			return _git_uploadpack_ls(t, url, stream);
			
		case GIT_SERVICE_UPLOADPACK:
			return _git_uploadpack(t, url, stream);
			
		case GIT_SERVICE_RECEIVEPACK_LS:
			return _git_receivepack_ls(t, url, stream);
			
		case GIT_SERVICE_RECEIVEPACK:
			return _git_receivepack(t, url, stream);
	}
	
	*stream = NULL;
	return -1;
}

static int _git_close(git_smart_subtransport *subtransport)
{
	ssh_subtransport *t = (ssh_subtransport *) subtransport;
	
	assert(!t->current_stream);
	
	GIT_UNUSED(t);
	
	return 0;
}

static void _git_free(git_smart_subtransport *subtransport)
{
	ssh_subtransport *t = (ssh_subtransport *) subtransport;
	
	assert(!t->current_stream);
	
	git__free(t);
}

int git_smart_subtransport_ssh(git_smart_subtransport **out, git_transport *owner)
{
	ssh_subtransport *t;
	
	if (!out)
		return -1;
	
	t = git__calloc(sizeof(ssh_subtransport), 1);
	GITERR_CHECK_ALLOC(t);
	
	t->owner = (transport_smart *)owner;
	t->parent.action = _git_action;
	t->parent.close = _git_close;
	t->parent.free = _git_free;
	
	*out = (git_smart_subtransport *) t;
	return 0;
}
