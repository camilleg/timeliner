#include <iostream>
#include <cstdlib>
#include "timeliner_diagnostics.h"

std::string appname = "app";

#define VERBOSE
#ifdef VERBOSE
void quit(const std::string& _) { std::cout << appname + " ABORT: " + _ +"\n\n"; exit(1); }
void warn(const std::string& _) { std::cout << appname + " WARN: " + _ +"\n"; }
void info(const std::string& _) { std::cout << appname + " info: " + _ +"\n"; }
#else
// Get user's attention: report only fatality.
// They'll ignore more than one diagnostic.
void quit(const std::string& _) { std::cout << appname + " ABORT: " + _ + "\n\n"; exit(1); }
void warn(const std::string& _) { quit(_); }
void info(const std::string& _) {}
#endif
