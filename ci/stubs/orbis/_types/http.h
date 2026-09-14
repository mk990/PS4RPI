#pragma once
#define ORBIS_METHOD_GET 0
#define ORBIS_HTTP_VERSION_1_1 1
#define ORBIS_HTTP_CONTENTLEN_EXIST 1
typedef int (*OrbisHttpsCallback)(int, void* const[], int, void*);
