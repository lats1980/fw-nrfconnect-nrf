#include "lock_storage_manager.h"
#include "persistent_storage_util.h"

namespace
{
template <class T> bool LoadDataToObject(PersistentStorageNode *node, T &data)
{
	size_t readSize = 0;

	bool result = PersistentStorage::Instance().Load(node, &data, sizeof(T), readSize);

	return result;
}

PersistentStorageNode CreateIndexNode(uint8_t nodeIndex, PersistentStorageNode *parent)
{
	char index[Nrf::LockStorageManager::kMaxIndexLength + 1] = { 0 };

	snprintf(index, sizeof(index), "%d", nodeIndex);

	return PersistentStorageNode(index, strlen(index), parent);
}

} /* namespace */

namespace Nrf {
bool LockStorageManager::FactoryReset(void)
{
	uint8_t totalUsersCount, totalCredentialsCount;

	LoadDataToObject(&mUsersCount, totalUsersCount);
	/* userIndex is guaranteed by the caller to be between 1 and CONFIG_LOCK_NUM_USERS */
	for (size_t userIndex = 1; userIndex <= totalUsersCount; userIndex++)
	{
		PersistentStorageNode id = CreateIndexNode(userIndex, &mUserData);
		PersistentStorage::Instance().Remove(&id);
		id = CreateIndexNode(userIndex, &mUserUniqueId);
		PersistentStorage::Instance().Remove(&id);
		id = CreateIndexNode(userIndex, &mUserType);
		PersistentStorage::Instance().Remove(&id);
		id = CreateIndexNode(userIndex, &mUserStatus);
		PersistentStorage::Instance().Remove(&id);
		id = CreateIndexNode(userIndex, &mUserCreateBy);
		PersistentStorage::Instance().Remove(&id);
		id = CreateIndexNode(userIndex, &mUserLastModifiedBy);
		PersistentStorage::Instance().Remove(&id);
		id = CreateIndexNode(userIndex, &mUserCredentialRule);
		PersistentStorage::Instance().Remove(&id);
	}
	PersistentStorage::Instance().Remove(&mUsersCount);
	LoadDataToObject(&mCredentialsCount, totalCredentialsCount);
	for (size_t credentialIndex = 1; credentialIndex <= totalCredentialsCount; credentialIndex++)
	{
		PersistentStorageNode id = CreateIndexNode(credentialIndex, &mCredentialStatus);
		PersistentStorage::Instance().Remove(&id);
		id = CreateIndexNode(credentialIndex, &mCredentialType);
		PersistentStorage::Instance().Remove(&id);
		id = CreateIndexNode(credentialIndex, &mCredentialCreatedBy);
		PersistentStorage::Instance().Remove(&id);
		id = CreateIndexNode(credentialIndex, &mCredentialLastModifiedBy);
		PersistentStorage::Instance().Remove(&id);
		id = CreateIndexNode(credentialIndex, &mCredentialSecret);
		PersistentStorage::Instance().Remove(&id);
		id = CreateIndexNode(credentialIndex, &mCredentialSecretSize);
		PersistentStorage::Instance().Remove(&id);
	}
	PersistentStorage::Instance().Remove(&mCredentialsCount);
	return true;
}

bool LockStorageManager::StoreUsersCount(uint8_t count)
{
	return PersistentStorage::Instance().Store(&mUsersCount, &count, sizeof(count));
}

bool LockStorageManager::LoadUsersCount(uint8_t &count)
{
	return LoadDataToObject(&mUsersCount, count);
}

bool LockStorageManager::StoreUserData(BoltLockManager::UserData userData, uint8_t index)
{
	PersistentStorageNode id = CreateIndexNode(index, &mUserData);

	return PersistentStorage::Instance().Store(&id, &userData, sizeof(userData));
}

bool LockStorageManager::LoadUserData(BoltLockManager::UserData &userData, uint8_t index)
{
	PersistentStorageNode id = CreateIndexNode(index, &mUserData);

	return LoadDataToObject(&id, userData);
}

bool LockStorageManager::StoreUserUniqueID(uint32_t userUniqueId, uint8_t index)
{
	PersistentStorageNode id = CreateIndexNode(index, &mUserUniqueId);

	return PersistentStorage::Instance().Store(&id, &userUniqueId, sizeof(userUniqueId));
}

bool LockStorageManager::LoadUserUniqueID(uint32_t &userUniqueID, uint8_t index)
{
	PersistentStorageNode id = CreateIndexNode(index, &mUserUniqueId);

	return LoadDataToObject(&id, userUniqueID);
}

bool LockStorageManager::StoreUserStatus(UserStatusEnum userStatus, uint8_t index)
{
	PersistentStorageNode id = CreateIndexNode(index, &mUserStatus);

	return PersistentStorage::Instance().Store(&id, &userStatus, sizeof(userStatus));
}

bool LockStorageManager::LoadUserStatus(UserStatusEnum &userStatus, uint8_t index)
{
	PersistentStorageNode id = CreateIndexNode(index, &mUserStatus);

	return LoadDataToObject(&id, userStatus);
}

bool LockStorageManager::StoreUserType(UserTypeEnum userType, uint8_t index)
{
	PersistentStorageNode id = CreateIndexNode(index, &mUserType);

	return PersistentStorage::Instance().Store(&id, &userType, sizeof(userType));
}

bool LockStorageManager::LoadUserType(UserTypeEnum &userType, uint8_t index)
{
	PersistentStorageNode id = CreateIndexNode(index, &mUserType);

	return LoadDataToObject(&id, userType);
}

bool LockStorageManager::StoreUserCreatedBy(uint8_t createdBy, uint8_t index)
{
	PersistentStorageNode id = CreateIndexNode(index, &mUserCreateBy);

	return PersistentStorage::Instance().Store(&id, &createdBy, sizeof(createdBy));
}

bool LockStorageManager::LoadUserCreatedBy(uint8_t &createdBy, uint8_t index)
{
	PersistentStorageNode id = CreateIndexNode(index, &mUserCreateBy);

	return LoadDataToObject(&id, createdBy);
}

bool LockStorageManager::StoreUserLastModifiedBy(uint8_t lastModifiedBy, uint8_t index)
{
	PersistentStorageNode id = CreateIndexNode(index, &mUserLastModifiedBy);

	return PersistentStorage::Instance().Store(&id, &lastModifiedBy, sizeof(lastModifiedBy));
}

bool LockStorageManager::LoadUserLastModifiedBy(uint8_t &lastModifiedBy, uint8_t index)
{
	PersistentStorageNode id = CreateIndexNode(index, &mUserLastModifiedBy);

	return LoadDataToObject(&id, lastModifiedBy);
}

bool LockStorageManager::StoreUserCredentialRule(CredentialRuleEnum credentialRule, uint8_t index)
{
	PersistentStorageNode id = CreateIndexNode(index, &mUserCredentialRule);

	return PersistentStorage::Instance().Store(&id, &credentialRule, sizeof(credentialRule));
}

bool LockStorageManager::LoadUserCredentialRule(CredentialRuleEnum &credentialRule, uint8_t index)
{
	PersistentStorageNode id = CreateIndexNode(index, &mUserCredentialRule);

	return LoadDataToObject(&id, credentialRule);
}

bool LockStorageManager::StoreCredentialsCount(uint8_t count)
{
	return PersistentStorage::Instance().Store(&mCredentialsCount, &count, sizeof(count));
}

bool LockStorageManager::LoadCredentialsCount(uint8_t &count)
{
	return LoadDataToObject(&mCredentialsCount, count);
}

bool LockStorageManager::StoreCredentialStatus(DlCredentialStatus credentialStatus, uint8_t index)
{
	PersistentStorageNode id = CreateIndexNode(index, &mCredentialStatus);

	return PersistentStorage::Instance().Store(&id, &credentialStatus, sizeof(credentialStatus));
}

bool LockStorageManager::LoadCredentialStatus(DlCredentialStatus &credentialStatus, uint8_t index)
{
	PersistentStorageNode id = CreateIndexNode(index, &mCredentialStatus);

	return LoadDataToObject(&id, credentialStatus);
}

bool LockStorageManager::StoreCredentialType(CredentialTypeEnum credentialType, uint8_t index)
{
	PersistentStorageNode id = CreateIndexNode(index, &mCredentialType);

	return PersistentStorage::Instance().Store(&id, &credentialType, sizeof(credentialType));
}

bool LockStorageManager::LoadCredentialType(CredentialTypeEnum &credentialType, uint8_t index)
{
	PersistentStorageNode id = CreateIndexNode(index, &mCredentialType);

	return LoadDataToObject(&id, credentialType);
}

bool LockStorageManager::StoreCredentialCreatedBy(uint8_t createdBy, uint8_t index)
{
	PersistentStorageNode id = CreateIndexNode(index, &mCredentialCreatedBy);

	return PersistentStorage::Instance().Store(&id, &createdBy, sizeof(createdBy));
}

bool LockStorageManager::LoadCredentialCreatedBy(uint8_t &createdBy, uint8_t index)
{
	PersistentStorageNode id = CreateIndexNode(index, &mCredentialCreatedBy);

	return LoadDataToObject(&id, createdBy);
}

bool LockStorageManager::StoreCredentialLastModifiedBy(uint8_t lastModifiedBy, uint8_t index)
{
	PersistentStorageNode id = CreateIndexNode(index, &mCredentialLastModifiedBy);

	return PersistentStorage::Instance().Store(&id, &lastModifiedBy, sizeof(lastModifiedBy));
}

bool LockStorageManager::LoadCredentialLastModifiedBy(uint8_t &lastModifiedBy, uint8_t index)
{
	PersistentStorageNode id = CreateIndexNode(index, &mCredentialLastModifiedBy);

	return LoadDataToObject(&id, lastModifiedBy);
}

bool LockStorageManager::StoreCredentialSecret(const uint8_t *secret, size_t secretSize, uint8_t index)
{
	PersistentStorageNode id = CreateIndexNode(index, &mCredentialSecret);

	if (secretSize != 0) {
		return PersistentStorage::Instance().Store(&id, secret, secretSize);
	}
	else {
		return PersistentStorage::Instance().Remove(&id);
	}
}

bool LockStorageManager::LoadCredentialSecret(uint8_t *secret, size_t secretSize, uint8_t index)
{
	PersistentStorageNode id = CreateIndexNode(index, &mCredentialSecret);
	size_t readSize = 0;

	PersistentStorage::Instance().Load(&id, secret, secretSize, readSize);
	bool result = readSize == secretSize?true:false;
	return result;
}

bool LockStorageManager::StoreCredentialSecretSize(size_t secretSize, uint8_t index)
{
	PersistentStorageNode id = CreateIndexNode(index, &mCredentialSecretSize);

	return PersistentStorage::Instance().Store(&id, &secretSize, sizeof(secretSize));
}

bool LockStorageManager::LoadCredentialSecretSize(size_t &secretSize, uint8_t index)
{
	PersistentStorageNode id = CreateIndexNode(index, &mCredentialSecretSize);

	return LoadDataToObject(&id, secretSize);
}

} /* namespace Nrf */

