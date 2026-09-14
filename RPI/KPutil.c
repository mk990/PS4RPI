#include <orbis/libkernel.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* The original code passed a hardcoded 3120 as the request size. Assert that the
   SDK struct really is that size so a toolchain change fails the build loudly
   instead of silently sending a truncated (or over-read) request. */
_Static_assert(sizeof(OrbisNotificationRequest) == 3120, "unexpected OrbisNotificationRequest size");

void Notify(const char* fmt, ...)
{
	OrbisNotificationRequest req;

	va_list args;

	memset(&req, 0, sizeof(req));

	va_start(args, fmt);
	vsnprintf(req.message, sizeof(req.message), fmt, args);
	va_end(args);

	req.type = NotificationRequest;
	req.unk3 = 0;
	req.useIconImageUri = 1;
	req.targetId = -1;
	strlcpy(req.iconUri, "cxml://psnotification/tex_icon_system", sizeof(req.iconUri));

	sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}

void KernelPrintOut(const char* fmt, ...)
{
	char buf[1024];
	va_list args;

	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);

	sceKernelDebugOutText(0, buf);
}
