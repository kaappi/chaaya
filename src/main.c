#include "chaaya/cli.h"

#include <stdio.h>

int main(int argc, char **argv) {
    ChCliOptions opts;
    int prc = ch_cli_parse(argc, argv, &opts);
    if (prc != CH_EXIT_OK) {
        return prc;
    }
    return ch_cli_dispatch(&opts, argc, argv);
}
