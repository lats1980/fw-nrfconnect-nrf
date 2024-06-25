#include "bolt_lock_manager.h"

#include <zephyr/shell/shell.h>

#include <cstdio>
#include <cstdlib>

#include <string>

#include <lib/core/DataModelTypes.h>
#include <lib/support/CodeUtils.h>

using std::string;
using namespace ::chip;

static int CreateUserCredentialHandler(const struct shell *shell, size_t argc, char **argv)
{
	bool ret;
	uint8_t userIndex = strtoul(argv[1], nullptr, 0);
	uint32_t userUniqueID = strtoul(argv[2], nullptr, 0);
	uint8_t credentialIndex = strtoul(argv[3], nullptr, 0);
	char *credentialData = nullptr;
	if (argv[4]) {
		credentialData = argv[4];
	}

	CredentialStruct credential{ CredentialTypeEnum::kPin, credentialIndex };
	ret = BoltLockMgr().SetUser(userIndex, kMaxValidFabricIndex, kMaxValidFabricIndex,
					chip::CharSpan(),
					userUniqueID, UserStatusEnum::kOccupiedEnabled, UserTypeEnum::kUnrestrictedUser,
					CredentialRuleEnum::kSingle, &credential, 1);
	if (!ret) {
		goto exit;
	}
	BoltLockMgr().SetCredential(credentialIndex, kMaxValidFabricIndex, kMaxValidFabricIndex, DlCredentialStatus::kOccupied, CredentialTypeEnum::kPin, ByteSpan(Uint8::from_const_char(credentialData), strlen(credentialData)));
	if (!ret) {
		goto exit;
	}
	shell_fprintf(shell, SHELL_INFO, "Done\n");
	return ret;
exit:
	shell_fprintf(shell, SHELL_INFO, "Fail\n");
	return ret;
}

static int ClearUserCredentialHandler(const struct shell *shell, size_t argc, char **argv)
{
	bool ret;
	uint8_t userIndex = strtoul(argv[1], nullptr, 0);
	uint8_t credentialIndex = strtoul(argv[2], nullptr, 0);

	CredentialStruct credential{ CredentialTypeEnum::kPin, 0 };
	ret = BoltLockMgr().SetUser(userIndex, 0, 0,
					chip::CharSpan(),
					0, UserStatusEnum::kAvailable, UserTypeEnum::kUnrestrictedUser,
					CredentialRuleEnum::kSingle, &credential, 0);
	if (!ret) {
		goto exit;
	}
	BoltLockMgr().SetCredential(credentialIndex, 0, 0, DlCredentialStatus::kAvailable, CredentialTypeEnum::kPin, chip::ByteSpan());
	if (!ret) {
		goto exit;
	}
	shell_fprintf(shell, SHELL_INFO, "Done\n");
	return ret;
exit:
	shell_fprintf(shell, SHELL_INFO, "Fail\n");
	return ret;
}

#ifdef CONFIG_LOCK_ENABLE_DEBUG
static void PrintUsersHandler(const struct shell *shell, size_t argc, char **argv)
{
	for (size_t idxUsr = 0; idxUsr < CONFIG_LOCK_MAX_NUM_USERS; ++idxUsr) {
		BoltLockMgr().PrintUserdata(idxUsr + 1);
	}
	shell_fprintf(shell, SHELL_INFO, "Done\n");
}

static void PrintCredentialsHandler(const struct shell *shell, size_t argc, char **argv)
{
	for (size_t idxCred = 0; idxCred < CONFIG_LOCK_MAX_NUM_CREDENTIALS_PER_TYPE; ++idxCred) {
		BoltLockMgr().PrintCredential(CredentialTypeEnum::kPin, idxCred + 1);
	}
	shell_fprintf(shell, SHELL_INFO, "Done\n");
}
#endif//CONFIG_LOCK_ENABLE_DEBUG

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_matter_lock,
	SHELL_CMD_ARG(
		createusercredential, NULL,
		"Create a new credential and a new user record. \n"
		"Usage: createusercredential <user_index> <user_unique_id> <credential_index> <credential_data> \n",
		CreateUserCredentialHandler, 5, 0),
	SHELL_CMD_ARG(
		clearusercredential, NULL,
		"Clear a credential and a user record. \n"
		"Usage: clearusercredential <user_index> <credential_index> \n",
		ClearUserCredentialHandler, 3, 0),
#ifdef CONFIG_LOCK_ENABLE_DEBUG
	SHELL_CMD_ARG(
		printusers, NULL,
		"Print all user data. \n"
		"Usage: printusers \n",
		PrintUsersHandler, 1, 0),
	SHELL_CMD_ARG(
		printcredentials, NULL,
		"Print all credential data. \n"
		"Usage: printcredentials \n",
		PrintCredentialsHandler, 1, 0),
#endif//CONFIG_LOCK_ENABLE_DEBUG
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(matter_lock, &sub_matter_lock, "matter_lock commands", NULL);