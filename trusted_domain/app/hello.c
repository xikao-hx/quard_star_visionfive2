#define LOG_TAG "HELLO"
#include "elog.h"
#include "shell.h"

/**
 * @brief 测试命令
 * 
 */
int hello_world(int argc, char *argv[]) {
    LOG_I("Hello, RISC-V Shell!");
    if (argc > 1) {
        LOG_I("Arg 1: %s", argv[1]);
    }

    return 0;
}

SHELL_EXPORT_CMD(SHELL_CMD_PERMISSION(0)|SHELL_CMD_TYPE(SHELL_TYPE_CMD_MAIN)|SHELL_CMD_DISABLE_RETURN, 
                hello, hello_world, test command);

