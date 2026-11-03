// Actual app capability policy, no GUI, subprocess or save-card access.
#include "fukami_app.h"
#include "fukami_app_common.h"
#include <cstdio>
#if defined(_WIN32)
#define setenv(n,v,o) _putenv_s(n,v)
#define unsetenv(n) _putenv_s(n,"")
#endif
int main() {
    using namespace fukami::app;
    unsigned checks=0;
    auto check=[&](bool ok){ ++checks; if(!ok){std::fprintf(stderr,"FAIL restart capability %u\n",checks);std::exit(1);} };
    unsetenv(detail::kChild);unsetenv(detail::kTerminalRestart);
    check(!canRestart());
    setenv(detail::kTerminalRestart,"0",1);check(!canRestart());
    setenv(detail::kTerminalRestart,"yes",1);check(!canRestart());
    setenv(detail::kTerminalRestart,"1",1);check(canRestart() && !inApp());
    unsetenv(detail::kTerminalRestart);setenv(detail::kChild,"1",1);check(canRestart() && inApp());
    check(detail::kTerminalRestartExit==75);
    std::printf("PASS actual restart capability: %u checks\n",checks);
}
