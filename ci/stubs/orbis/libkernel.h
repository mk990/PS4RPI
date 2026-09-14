#pragma once
#include <stdint.h>
#include <stddef.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#undef st_atime
typedef struct { int st_dev; struct timespec st_atime_ts; } OrbisKernelStat_real;
typedef struct OrbisKernelStat { unsigned int st_mode; long st_size; struct timespec st_atime; } OrbisKernelStat;
typedef struct OrbisNotificationRequest { int type; int unk3; int useIconImageUri; int targetId; char message[1024]; char iconUri[1024]; char pad[3120-16-2048]; } OrbisNotificationRequest;
#define NotificationRequest 0
int sceKernelOpen(const char* path, int flags, int mode);
int sceKernelClose(int fd);
int sceKernelStat(const char* path, OrbisKernelStat* st);
int sceKernelGetdents(int fd, char* buf, size_t size);
int sceKernelSendNotificationRequest(int a, OrbisNotificationRequest* r, size_t n, int b);
int sceKernelDebugOutText(int a, const char* s);
int sceKernelLoadStartModule(const char*, void*, void*, void*, void*, void*);
int sceKernelDlsym(int, const char*, void**);
#define ORBIS_KERNEL_ERROR_EINVAL 0x16
#define ORBIS_KERNEL_ERROR_ENOSPC 0x1c
