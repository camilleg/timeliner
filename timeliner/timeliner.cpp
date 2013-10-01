// Entry point for the console application.
#include "stdafx.h"
extern int mainCore(int argc, char** const argv);

int _tmain(int argc, _TCHAR* argv[])
{
    // For this to run, disable unicode: config properties, general, project defaults, character set, "not set".
	return mainCore(argc, argv);
}

