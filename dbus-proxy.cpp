#include <vector>
#include <fstream>
#include <cstdlib>
#include <iostream>

#include "flatpak-proxy.h"


static const char *argv0;

static void
usage(int ecode, std::ostream *out)
{
    *out<<"usage: "<<argv0<<" [OPTIONS...] [ADDRESS PATH [OPTIONS...] ...]\n\n";
    *out<< "Options:\n"
           "    --help                       Print this help\n"
           "    --version                    Print version\n"
           "    --fd=FD                      Stop when FD is closed\n"
           "    --args=FD                    Read arguments from FD\n\n"
           "Proxy Options:\n"
           "    --filter                     Enable filtering\n"
           "    --log                        Turn on logging\n"
           "    --sloppy-names               Report name changes for unique names\n"
           "    --see=NAME                   Set 'see' policy for NAME\n"
           "    --talk=NAME                  Set 'talk' policy for NAME\n"
           "    --own=NAME                   Set 'own' policy for NAME\n"
           "    --call=NAME=RULE             Set RULE for calls on NAME\n"
            "    --broadcast=NAME=RULE        Set RULE for broadcasts from NAME\n";
    exit(ecode);
}


int main(int argc, char *argv[]) {
    std::vector <std::string> args(argv+1,argv+argc);
    int args_i;
    argv0=argv[0];
    setlocale(LC_ALL, "");

    if (argc==1) usage(EXIT_FAILURE, &std::cerr);
}