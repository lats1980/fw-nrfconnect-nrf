#include <app/server/Server.h>

#include <zephyr/shell/shell.h>

static int ResetDeviceHandler(const struct shell *shell, size_t argc, char **argv)
{
    shell_fprintf(shell, SHELL_INFO, "Done\n");
    chip::Server::GetInstance().ScheduleFactoryReset();

    return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_test,
	SHELL_CMD_ARG(
		reset, NULL,
		"Factory reset device. \n",
		ResetDeviceHandler, 1, 0),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(test, &sub_test, "test commands", NULL);