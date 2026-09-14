#pragma once
typedef struct OrbisNetCtlInfo { char ip_address[16]; } OrbisNetCtlInfo;
#define ORBIS_NET_CTL_INFO_IP_ADDRESS 0
int sceNetCtlInit(void);
void sceNetCtlTerm(void);
int sceNetCtlGetInfo(int code, OrbisNetCtlInfo* info);
