#include "http.h"

#include <sys/stat.h>

#include <orbis/NpCommon.h>
#include <orbis/NpUtility.h>

#include "net.h"
#include "ssl.h"
#include "util.h"
#include "tiny-json.h"

#define HTTP_HEAP_SIZE (1024 * 1024)
#define SSL_HEAP_SIZE (128 * 1024)

#define DOWNLOAD_CHUNK_SIZE (16384)

#define USER_AGENT "Download/1.00"

/* A concatenated PEM bundle of public roots is well under this. */
#define CA_BUNDLE_MAX_SIZE (1024 * 1024)
#define CONFIG_MAX_SIZE (64 * 1024)

/* Every check the console can perform on a server certificate. */
#define SSL_SERVER_CHECKS ( \
	SCE_HTTPS_FLAG_SERVER_VERIFY | \
	SCE_HTTPS_FLAG_CN_CHECK | \
	SCE_HTTPS_FLAG_NOT_AFTER_CHECK | \
	SCE_HTTPS_FLAG_NOT_BEFORE_CHECK | \
	SCE_HTTPS_FLAG_KNOWN_CA_CHECK)

struct download_file_cb_args {
	uint8_t* data;
	uint64_t data_size;
	uint64_t actual_size;
	uint64_t content_length;
	uint8_t* chunk;
	size_t chunk_size;
	bool want_partial;
	bool range_ignored;
	int status_code;
};

static int s_libssl_ctx_id = -1;
static int s_libhttp_ctx_id = -1;

static bool s_http_initialized = false;

static int s_ssl_verify_mode = HTTP_SSL_VERIFY_ON;

static char s_ca_bundle_path[256] = HTTP_CA_BUNDLE_PATH;
static char* s_ca_bundle_data = NULL;
static SslPem s_ca_bundle_pem;
static bool s_ca_bundle_loaded = false;

static char s_last_error[512] = "";

typedef int request_cb_t(void* arg, int req_id, int status_code, uint64_t content_length, int content_length_type);

static int download_file_cb(void* arg, int req_id, int status_code, uint64_t content_length, int content_length_type);

static int do_request(const char* url, int method, const void* data, size_t data_size, const char** headers, size_t header_count, request_cb_t* cb, void* arg, int ssl_verify);

static inline bool is_good_status(int status_code);

static void set_last_error(const char* format, ...);
static bool apply_ssl_options(int tpl_id, const char* url, int ssl_verify);
static void unload_ca_bundle(void);

bool http_init(void)
{
	int ret;

	if (s_http_initialized) {
		goto done;
	}

	if (!net_is_initialized()) {
		goto err;
	}

	ret = sceSslInit(SSL_HEAP_SIZE);
	if (ret < 0) {
		EPRINTF("sceSslInit failed: 0x%08X\n", ret);
		goto err;
	}
	s_libssl_ctx_id = ret;

	ret = sceHttpInit(net_get_mem_id(), s_libssl_ctx_id, HTTP_HEAP_SIZE);
	if (ret < 0) {
		EPRINTF("sceHttpInit failed: 0x%08X\n", ret);
		goto err_ssl_terminate;
	}
	s_libhttp_ctx_id = ret;

	s_http_initialized = true;

	/* Settings are optional: a console without a config file keeps the
	   verifying default. */
	http_load_config(HTTP_CONFIG_PATH);

	/* The console's built-in root store predates most current issuers, so a
	   user-supplied bundle is what makes verification usable in practice.
	   Its absence is not an error - it only limits which hosts will verify. */
	if (!http_load_ca_bundle(s_ca_bundle_path)) {
		KernelPrintOut("RPI: no CA bundle at %s, relying on the built-in root store\n", s_ca_bundle_path);
		/* Startup diagnostics must not leak into the first request's error. */
		http_clear_last_error();
	}

done:
	return true;

err_ssl_terminate:
	ret = sceSslTerm(s_libssl_ctx_id);
	if (ret) {
		EPRINTF("sceSslTerm failed: 0x%08X\n", ret);
	}
	s_libssl_ctx_id = -1;

err:
	return false;
}

void http_fini(void) {
	int ret;

	if (!s_http_initialized) {
		return;
	}

	unload_ca_bundle();

	ret = sceHttpTerm(s_libhttp_ctx_id);
	if (ret) {
		EPRINTF("sceHttpTerm failed: 0x%08X\n", ret);
	}
	s_libhttp_ctx_id = -1;

	ret = sceSslTerm(s_libssl_ctx_id);
	if (ret) {
		EPRINTF("sceSslTerm failed: 0x%08X\n", ret);
	}
	s_libssl_ctx_id = -1;

	s_http_initialized = false;
}

int http_get_ssl_verify(void) {
	return s_ssl_verify_mode;
}

void http_set_ssl_verify(int mode) {
	s_ssl_verify_mode = (mode == HTTP_SSL_VERIFY_OFF) ? HTTP_SSL_VERIFY_OFF : HTTP_SSL_VERIFY_ON;
}

bool http_has_ca_bundle(void) {
	return s_ca_bundle_loaded;
}

const char* http_get_ca_bundle_path(void) {
	return s_ca_bundle_path;
}

static void unload_ca_bundle(void) {
	int ret;

	if (s_ca_bundle_loaded) {
		ret = sceHttpsUnloadCert(s_libhttp_ctx_id);
		if (ret) {
			EPRINTF("sceHttpsUnloadCert failed: 0x%08X\n", ret);
		}
		s_ca_bundle_loaded = false;
	}

	if (s_ca_bundle_data) {
		free(s_ca_bundle_data);
		s_ca_bundle_data = NULL;
	}

	memset(&s_ca_bundle_pem, 0, sizeof(s_ca_bundle_pem));
}

bool http_load_ca_bundle(const char* path) {
	SslPem* list[1];
	char* data = NULL;
	size_t size = 0;
	int ret;

	if (!s_http_initialized) {
		set_last_error("HTTP is not initialized.");
		goto err;
	}
	if (!path || strlen(path) == 0) {
		set_last_error("Empty CA bundle path.");
		goto err;
	}

	if (!read_text_file(path, &data, &size, CA_BUNDLE_MAX_SIZE)) {
		set_last_error("Unable to read CA bundle '%s'.", path);
		goto err;
	}
	if (size == 0) {
		set_last_error("CA bundle '%s' is empty.", path);
		goto err_free;
	}

	/* Drop whatever was loaded before so a reload cannot leave two copies of
	   the roots registered with libhttp. */
	unload_ca_bundle();

	s_ca_bundle_data = data;
	data = NULL;

	s_ca_bundle_pem.ptr = s_ca_bundle_data;
	s_ca_bundle_pem.size = size;
	list[0] = &s_ca_bundle_pem;

	/* libhttp keeps referencing the buffer, so it must outlive this call. */
	ret = sceHttpsLoadCert(s_libhttp_ctx_id, 1, (void*)list, NULL, NULL);
	if (ret) {
		EPRINTF("sceHttpsLoadCert failed: 0x%08X\n", ret);
		set_last_error("Unable to load CA bundle '%s': 0x%08X.", path, ret);
		free(s_ca_bundle_data);
		s_ca_bundle_data = NULL;
		memset(&s_ca_bundle_pem, 0, sizeof(s_ca_bundle_pem));
		goto err;
	}

	s_ca_bundle_loaded = true;

	if (path != s_ca_bundle_path) {
		strlcpy(s_ca_bundle_path, path, sizeof(s_ca_bundle_path));
	}

	KernelPrintOut("RPI: loaded CA bundle %s (%zu bytes)\n", s_ca_bundle_path, size);

	return true;

err_free:
	free(data);

err:
	return false;
}

bool http_load_config(const char* path) {
	json_t pool[16];
	const json_t* root;
	const json_t* field;
	char* text = NULL;
	size_t size = 0;
	bool status = false;

	if (!path) {
		goto err;
	}

	if (!read_text_file(path, &text, &size, CONFIG_MAX_SIZE)) {
		goto err;
	}

	root = json_create(text, pool, ARRAY_SIZE(pool));
	if (!root) {
		EPRINTF("Invalid JSON in '%s'.\n", path);
		goto err_free;
	}

	field = json_getProperty(root, "ssl_verify");
	if (field && json_getType(field) == JSON_BOOLEAN) {
		s_ssl_verify_mode = json_getBoolean(field) ? HTTP_SSL_VERIFY_ON : HTTP_SSL_VERIFY_OFF;
	}

	field = json_getProperty(root, "ca_bundle");
	if (field && json_getType(field) == JSON_TEXT) {
		strlcpy(s_ca_bundle_path, json_getValue(field), sizeof(s_ca_bundle_path));
	}

	status = true;

err_free:
	free(text);

err:
	return status;
}

bool http_save_config(const char* path) {
	char buf[512];
	char escaped_path[sizeof(s_ca_bundle_path) * 2 + 1];
	int len;

	if (!path) {
		return false;
	}

	if (!http_escape_json_string(escaped_path, sizeof(escaped_path), s_ca_bundle_path)) {
		return false;
	}

	len = snprintf(buf, sizeof(buf), "{ \"ssl_verify\": %s, \"ca_bundle\": \"%s\" }\n",
		s_ssl_verify_mode == HTTP_SSL_VERIFY_ON ? "true" : "false", escaped_path);
	if (len < 0 || (size_t)len >= sizeof(buf)) {
		return false;
	}

	return write_file_trunc(path, buf, (uint64_t)len, NULL, S_IRUSR | S_IWUSR);
}

const char* http_get_last_error(void) {
	return s_last_error;
}

void http_clear_last_error(void) {
	s_last_error[0] = '\0';
}

static void set_last_error(const char* format, ...) {
	va_list args;

	va_start(args, format);
	vsnprintf(s_last_error, sizeof(s_last_error), format, args);
	va_end(args);
}

bool http_get_file_size(const char* url, uint64_t* total_size, int ssl_verify) {
	struct download_file_cb_args args;
	bool status = false;
	int ret;

	if (!s_http_initialized) {
		goto err;
	}
	if (!url) {
		goto err;
	}

	memset(&args, 0, sizeof(args));
	{
		args.data_size = 0;
	}

	ret = do_request(url, ORBIS_METHOD_GET, NULL, 0, NULL, 0, &download_file_cb, &args, ssl_verify);
	if (ret) {
		goto err;
	}
	if (!is_good_status(args.status_code)) {
		set_last_error("Unexpected HTTP status %d for '%s'.", args.status_code, url);
		goto err;
	}
	/* No Content-Length, so nothing to report: the piece sizes written into the
	   reference json have to be real numbers. */
	if (args.content_length == UINT64_MAX) {
		set_last_error("Server did not report a size for '%s'.", url);
		goto err;
	}

	if (total_size) {
		*total_size = args.content_length;
	}

	status = true;

err:
	return status;
}

bool http_download_file(const char* url, uint8_t** data, uint64_t* data_size, uint64_t* total_size, uint64_t offset, int ssl_verify) {
	struct download_file_cb_args args;
	uint8_t chunk[DOWNLOAD_CHUNK_SIZE];
	const char* headers[8 * 2];
	size_t header_count = 0;
	char range_str[48];
	bool status = false;
	int ret;

	if (!s_http_initialized) {
		goto err;
	}
	if (!url) {
		goto err;
	}

	memset(&args, 0, sizeof(args));
	{
		args.data_size = data_size ? *data_size : (uint64_t)-1;
		args.chunk = chunk;
		args.chunk_size = sizeof(chunk);
	}

	memset(headers, 0, sizeof(headers));
	{
		headers[header_count * 2 + 0] = "Accept-Encoding";
		headers[header_count * 2 + 1] = "identity";
		++header_count;
	}

	if (offset > 0 && args.data_size > 0) {
		snprintf(range_str, sizeof(range_str), "bytes=%" PRIu64 "-%" PRIu64, offset, offset + args.data_size - 1);
		headers[header_count * 2 + 0] = "Range";
		headers[header_count * 2 + 1] = range_str;
		++header_count;

		args.want_partial = true;
	}

	ret = do_request(url, ORBIS_METHOD_GET, NULL, 0, headers, header_count, &download_file_cb, &args, ssl_verify);
	if (args.range_ignored) {
		set_last_error("Server answered a range request for '%s' with HTTP %d instead of 206, so it cannot serve partial content.", url, args.status_code);
		goto err_data_free;
	}
	if (ret) {
		goto err_data_free;
	}
	if (!is_good_status(args.status_code)) {
		set_last_error("Unexpected HTTP status %d for '%s'.", args.status_code, url);
		goto err_data_free;
	}

	if (data) {
		*data = args.data;
		args.data = NULL;
	}
	if (data_size) {
		*data_size = args.actual_size;
	}
	if (total_size) {
		*total_size = args.content_length;
	}

	status = true;

err_data_free:
	if (args.data) {
		free(args.data);
	}

err:
	return status;
}

bool http_unescape_uri(char** out, size_t* out_size, const char* in) {
	char* tmp = NULL;
	size_t tmp_size;
	bool status = false;
	int ret;

	if (!s_http_initialized) {
		goto err;
	}

	if (!in) {
		goto err;
	}

	ret = sceHttpUriUnescape(NULL, &tmp_size, 0, in);
	if (ret) {
		EPRINTF("sceHttpUriUnescape failed: 0x%08X\n", ret);
		goto err;
	}

	tmp = (char*)malloc(tmp_size);
	if (!tmp) {
		EPRINTF("malloc failed\n");
		goto err;
	}
	memset(tmp, 0, tmp_size);

	ret = sceHttpUriUnescape(tmp, out_size, tmp_size, in);
	if (ret) {
		EPRINTF("sceHttpUriUnescape failed: 0x%08X\n", ret);
		goto err;
	}

	if (out) {
		*out = tmp;
		tmp = NULL;
	}

	status = true;

err:
	if (tmp) {
		free(tmp);
	}

	return status;
}

bool http_escape_json_string(char* out, size_t max_out_size, const char* in) {
	bool status = false;
	int ret;

	if (!s_http_initialized) {
		goto err;
	}

	if (!in) {
		goto err;
	}

	memset(out, 0, max_out_size);

	ret = sceNpUtilJsonEscape(out, max_out_size, in, strlen(in));
	if (ret) {
		EPRINTF("sceNpUtilJsonEscape failed: 0x%08X\n", ret);
		goto err;
	}

	status = true;

err:
	return status;
}

static int download_file_cb(void* arg, int req_id, int status_code, uint64_t content_length, int content_length_type) {
	struct download_file_cb_args* args = (struct download_file_cb_args*)arg;
	uint8_t* chunk;
	size_t chunk_size;
	uint8_t* cur_data;
	uint64_t cur_size = 0;
	uint64_t total_size;
	int ret;

	assert(args != NULL);

	args->status_code = status_code;

	if (req_id < 0) {
		ret = SCE_HTTP_ERROR_INVALID_ID;
		goto err;
	}

	if (!is_good_status(status_code)) {
		ret = 404;
		goto err;
	}

	/* A server that does not implement Range answers the whole file with 200.
	   Reading that body would hand back bytes from offset 0 while the caller
	   believes it got the bytes it asked for, so refuse it instead. */
	if (args->want_partial && status_code != 206) {
		args->range_ignored = true;
		ret = SCE_HTTP_ERROR_INVALID_VALUE;
		goto err;
	}

	if (content_length_type != ORBIS_HTTP_CONTENTLEN_EXIST) {
		content_length = UINT64_MAX;
	}

	if (args->data_size == (uint64_t)-1) {
		/* XXX: if Content-Length is not specified then user must specify it by himself */
		if (content_length_type != ORBIS_HTTP_CONTENTLEN_EXIST) {
			ret = SCE_HTTP_ERROR_NO_CONTENT_LENGTH;
			goto err;
		}
		total_size = content_length;
	} else if (args->data_size > content_length) {
		total_size = content_length;
	} else {
		total_size = args->data_size;
	}
	if (total_size > 0 && !args->chunk) {
		ret = SCE_HTTP_ERROR_INVALID_VALUE;
		goto err;
	}

	cur_data = args->data = (uint8_t*)malloc(total_size + 1); /* XXX: allocate one more byte to have valid cstrings */
	if (!cur_data) {
		ret = SCE_HTTP_ERROR_OUT_OF_MEMORY;
		goto err_partial_xfer;
	}
	memset(cur_data, 0, total_size + 1);

	for (chunk = args->chunk; cur_size < total_size; ) {
		chunk_size = total_size - cur_size;
		if (chunk_size > args->chunk_size) {
			chunk_size = args->chunk_size;
		}
		ret = sceHttpReadData(req_id, chunk, chunk_size);
		if (ret < 0) {
			EPRINTF("sceHttpReadData failed: 0x%08X\n", ret);
			goto err_partial_xfer;
		} else if (ret == 0){
			break;
		}

		memcpy(cur_data, chunk, ret);

		cur_data += ret;
		cur_size += ret;
	}

	ret = 0;

err_partial_xfer:
	args->data_size = total_size;
	args->actual_size = cur_size;
	args->content_length = content_length;

err:
	return ret;
}

/* Applies the current verification policy to a freshly created template, before
   any connection is derived from it. */
static bool apply_ssl_options(int tpl_id, const char* url, int ssl_verify) {
	int ret;

	if (ssl_verify == HTTP_SSL_VERIFY_OFF) {
		ret = sceHttpsDisableOption(tpl_id, SSL_SERVER_CHECKS | SCE_HTTPS_FLAG_CLIENT_VERIFY);
		if (ret) {
			/* Not fatal: without verification there is nothing to fail closed on. */
			EPRINTF("sceHttpsDisableOption failed: 0x%08X\n", ret);
		}
		return true;
	}

	/* We never present a client certificate, so that check stays off. */
	ret = sceHttpsDisableOption(tpl_id, SCE_HTTPS_FLAG_CLIENT_VERIFY);
	if (ret) {
		EPRINTF("sceHttpsDisableOption failed: 0x%08X\n", ret);
	}

	ret = sceHttpsEnableOption(tpl_id, SSL_SERVER_CHECKS);
	if (ret) {
		/* Verification was asked for and could not be armed, so refuse the
		   request rather than silently downgrading to an unchecked one. */
		EPRINTF("sceHttpsEnableOption failed: 0x%08X\n", ret);
		set_last_error("Unable to enable TLS verification for '%s': 0x%08X.", url, ret);
		return false;
	}

	return true;
}

static int do_request(const char* url, int method, const void* data, size_t data_size, const char** headers, size_t header_count, request_cb_t* cb, void* arg, int ssl_verify) {
	int tpl_id = -1, conn_id = -1, req_id = -1;
	int status_code = 0, content_length_type = -1; /* anything but ORBIS_HTTP_CONTENTLEN_EXIST */
	uint64_t content_length = 0;
	int last_errno = 0;
	int status;
	size_t i;
	int ret;

	http_clear_last_error();

	/* Resolve "use the configured default" once, up front. */
	ssl_verify = (ssl_verify == HTTP_SSL_VERIFY_OFF) ? HTTP_SSL_VERIFY_OFF :
	             (ssl_verify == HTTP_SSL_VERIFY_ON) ? HTTP_SSL_VERIFY_ON : s_ssl_verify_mode;

	if (!url) {
		status = SCE_HTTP_ERROR_INVALID_VALUE;
		goto err;
	}
	if (!headers) {
		header_count = 0;
	}

	ret = sceHttpCreateTemplate(s_libhttp_ctx_id, USER_AGENT, ORBIS_HTTP_VERSION_1_1, 1);
	if (ret < 0) {
		EPRINTF("sceHttpCreateTemplate failed: 0x%08X\n", ret);
		status = ret;
		goto err;
	}
	tpl_id = ret;

	/* Must happen before the connection is created so it inherits the policy. */
	if (!apply_ssl_options(tpl_id, url, ssl_verify)) {
		status = SCE_HTTP_ERROR_INVALID_VALUE;
		goto err_tpl_delete;
	}

	ret = sceHttpCreateConnectionWithURL(tpl_id, url, 1);
	if (ret < 0) {
		EPRINTF("sceHttpCreateConnectionWithURL failed: 0x%08X\n", ret);
		status = ret;
		goto err_tpl_delete;
	}
	conn_id = ret;

	ret = sceHttpCreateRequestWithURL(conn_id, method, url, data ? data_size : 0);
	if (ret < 0) {
		EPRINTF("sceHttpCreateRequestWithURL failed: 0x%08X\n", ret);
		status = ret;
		goto err_conn_delete;
	}
	req_id = ret;

	for (i = 0; i < header_count; ++i) {
		ret = sceHttpAddRequestHeader(req_id, headers[i * 2 + 0], headers[i * 2 + 1], SCE_HTTP_HEADER_OVERWRITE);
		if (ret) {
			EPRINTF("sceHttpAddRequestHeader failed: 0x%08X\n", ret);
			status = ret;
			goto err_req_delete;
		}
	}

	ret = sceHttpSendRequest(req_id, data, data ? data_size : 0);
	if (ret) {
		EPRINTF("sceHttpSendRequest failed: 0x%08X\n", ret);
		if (sceHttpGetLastErrno(req_id, &last_errno)) {
			last_errno = 0;
		}
		if (ssl_verify == HTTP_SSL_VERIFY_ON && starts_with(url, "https://")) {
			/* The most common cause by far, and the one the user can act on. */
			set_last_error(
				"Request to '%s' failed (0x%08X, errno %d). With TLS verification on this is usually an "
				"untrusted or expired server certificate: add its issuer to %s, or turn ssl_verify off.",
				url, ret, last_errno, s_ca_bundle_path);
		} else {
			set_last_error("Request to '%s' failed (0x%08X, errno %d).", url, ret, last_errno);
		}
		status = ret;
		goto err_req_delete;
	}

	ret = sceHttpGetStatusCode(req_id, &status_code);
	if (ret < 0) {
		EPRINTF("sceHttpGetStatusCode failed: 0x%08X\n", ret);
		status = ret;
		goto err_req_delete;
	}

	if (is_good_status(status_code)) {
		ret = sceHttpGetResponseContentLength(req_id, &content_length_type, &content_length);
		if (ret) {
			EPRINTF("sceHttpGetResponseContentLength failed: 0x%08X\n", ret);
			status = ret;
			goto err_req_delete;
		}
	}

	status = 0;
	if (cb) {
		status = (*cb)(arg, req_id, status_code, content_length, content_length_type);
	}

err_req_delete:
	ret = sceHttpDeleteRequest(req_id);
	if (ret) {
		EPRINTF("sceHttpDeleteRequest failed: 0x%08X\n", ret);
	}

err_conn_delete:
	ret = sceHttpDeleteConnection(conn_id);
	if (ret) {
		EPRINTF("sceHttpDeleteConnection failed: 0x%08X\n", ret);
	}

err_tpl_delete:
	ret = sceHttpDeleteTemplate(tpl_id);
	if (ret) {
		EPRINTF("sceHttpDeleteTemplate failed: 0x%08X\n", ret);
	}

err:
	/* Return the failure that actually happened, not the result of tearing the
	   request down afterwards. */
	return status;
}

static inline bool is_good_status(int status_code) {
	return (status_code == 200 || status_code == 206);
}
