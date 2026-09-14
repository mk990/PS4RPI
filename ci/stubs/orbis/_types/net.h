#pragma once
#include <stdint.h>
#include <stddef.h>
typedef int OrbisNetId;
typedef unsigned int OrbisNetSocklen_t;
typedef struct OrbisNetSockaddr { unsigned char sa_len, sa_family; char sa_data[14]; } OrbisNetSockaddr;
typedef struct OrbisNetDnsInfo { int d; } OrbisNetDnsInfo;
typedef struct OrbisNetEpollEvent { int e; } OrbisNetEpollEvent;
typedef struct OrbisNetIovec { void* p; size_t l; } OrbisNetIovec;
typedef struct OrbisNetMsghdr { int m; } OrbisNetMsghdr;
typedef struct OrbisNetInAddr { uint32_t s_addr; } OrbisNetInAddr;
typedef struct OrbisNetSockaddrIn { unsigned char sin_len, sin_family; uint16_t sin_port; OrbisNetInAddr sin_addr; } OrbisNetSockaddrIn;
typedef struct OrbisNetLinger { int l; } OrbisNetLinger;
typedef struct OrbisNetIpMreq { int i; } OrbisNetIpMreq;
typedef struct OrbisNetEtherAddr { unsigned char d[6]; } OrbisNetEtherAddr;
typedef struct OrbisNetEpollData { void* p; } OrbisNetEpollData;
