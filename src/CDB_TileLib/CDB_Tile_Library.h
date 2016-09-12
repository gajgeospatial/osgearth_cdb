#pragma once
#ifdef CDB_TILELIB_EXPORTS
#define CDBTILELIBRARYAPI __declspec(dllexport)
#else
#define CDBTILELIBRARYAPI __declspec(dllimport)
#endif
