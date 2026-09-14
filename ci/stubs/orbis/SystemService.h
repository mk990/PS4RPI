#pragma once
#define ORBIS_SYSTEM_SERVICE_PARAM_ID_LANG 0
int sceSystemServiceParamGetInt(int id, int* value);
int sceSystemServiceLoadExec(char* path, void* args);
