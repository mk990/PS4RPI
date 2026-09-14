#include "net.h"

#include <orbis/libkernel.h>
#include <orbis/NetCtl.h>

#define NET_HEAP_SIZE (1 * 1024 * 1024)

static int s_libnet_mem_id = -1;

static bool s_net_initialized = false;

/* sceNetErrnoLoc() returns a pointer to this thread's libnet errno. */
static inline int net_errno(void) {
	int* loc = sceNetErrnoLoc();
	return loc ? *loc : 0;
}

bool net_init(void) {
	int ret;

	if (s_net_initialized) {
		goto done;
	}

	ret = sceNetCtlInit();
	if (ret) {
		EPRINTF("sceNetCtlInit failed: 0x%08X\n", ret);
		goto err;
	}

	ret = sceNetInit();
	if (ret) {
		EPRINTF("sceNetInit failed: 0x%08X\n", net_errno());
		goto err_netctl_terminate;
	}

	ret = sceNetPoolCreate("remote_pkg_inst_net_pool", NET_HEAP_SIZE, 0);
	if (ret < 0) {
		EPRINTF("sceNetPoolCreate failed: 0x%08X\n", net_errno());
		goto err_net_terminate;
	}
	s_libnet_mem_id = ret;

	s_net_initialized = true;

done:
	return true;

err_net_terminate:
	ret = sceNetTerm();
	if (ret) {
		EPRINTF("sceNetTerm failed: 0x%08X\n", net_errno());
	}

err_netctl_terminate:
	sceNetCtlTerm();

err:
	return false;
}

bool net_is_initialized(void) {
	return s_net_initialized;
}

int net_get_mem_id(void) {
	if (!s_net_initialized) {
		return -1;
	}

	return s_libnet_mem_id;
}

void net_fini(void) {
	int ret;

	if (!s_net_initialized) {
		return;
	}

	ret = sceNetPoolDestroy(s_libnet_mem_id);
	if (ret < 0) {
		EPRINTF("sceNetPoolDestroy failed: 0x%08X\n", net_errno());
	}
	s_libnet_mem_id = -1;

	ret = sceNetTerm();
	if (ret < 0) {
		EPRINTF("sceNetTerm failed: 0x%08X\n", net_errno());
	}

	sceNetCtlTerm();

	s_net_initialized = false;
}

int net_get_ipv4(char* buf, size_t buf_size) {
	OrbisNetCtlInfo info;
	int ret;

	if (!buf || buf_size < sizeof(info.ip_address)) {
		ret = 0x80020016;
		goto err;
	}

	memset(&info, 0, sizeof(info));
	ret = sceNetCtlGetInfo(ORBIS_NET_CTL_INFO_IP_ADDRESS, &info);
	if (ret) {
		EPRINTF("sceNetCtlGetInfo failed: 0x%08X\n", ret);
		goto err;
	}

	strlcpy(buf, info.ip_address, buf_size);

err:
	return ret;
}
