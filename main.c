#include <stdio.h>

#include "player.h"

int main(int argc, char *argv[])
{
    if (argc != 2)
    {
        av_log(NULL, AV_LOG_FATAL, "Please provide a movie file, usage: \n./simpleFFplay test.mp4\n");
        return -1;
    }
    player_running(argv[1]);

    return 0;
}
