/*
 * Copyright (c) 2023 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#pragma once

#include "persistent_storage/persistent_storage_util.h"
#include "bolt_lock_manager.h"

namespace Nrf {

class LockStorageManager {
public:
	static constexpr auto kMaxIndexLength = 3;

	LockStorageManager()
		: mLock("lk", strlen("lk")),
		mUsersCount("usrs_cnt",strlen("usrs_cnt"), &mLock),
		mUser("usr", strlen("usr"), &mLock),
		mUserData("usr_data", strlen("usr_data"), &mUser),
		mUserUniqueId("usr_uid", strlen("usr_uid"), &mUser),
		mUserStatus("usr_status", strlen("usr_status"), &mUser),
		mUserType("usr_type", strlen("usr_type"), &mUser),
		mUserCreateBy("usr_createdby", strlen("usr_createdby"), &mUser),
		mUserLastModifiedBy("usr_modifiedby", strlen("usr_modifiedby"), &mUser),
		mUserCredentialRule("usr_cred_rule", strlen("usr_cred_rule"), &mUser),
		mCredentialsCount("cred_cnt", strlen("cred_cnt"), &mLock),
		mCredential("cred", strlen("cred"), &mLock),
		mCredentialStatus("cred_status", strlen("cred_status"), &mCredential),
		mCredentialType("cred_type", strlen("cred_type"), &mCredential),
		mCredentialCreatedBy("cred_createdby", strlen("cred_createdby"), &mCredential),
		mCredentialLastModifiedBy("cred_lastmodifiedby", strlen("cred_lastmodifiedby"), &mCredential),
		mCredentialSecret("cred_scr", strlen("cred_scr"), &mCredential),
		mCredentialSecretSize("cred_scr_sz", strlen("cred_scr_sz"), &mCredential)
	{
	}

	static LockStorageManager &Instance()
	{
		static LockStorageManager sInstance;
		return sInstance;
	}

	bool Init() { return PersistentStorage::Instance().Init(); }
	bool StoreUsersCount(uint8_t count);
	bool LoadUsersCount(uint8_t &count);
	bool StoreUserData(BoltLockManager::UserData userData, uint8_t index);
	bool LoadUserData(BoltLockManager::UserData &userData, uint8_t index);
	bool StoreUserUniqueID(uint32_t userUniqueId, uint8_t index);
	bool LoadUserUniqueID(uint32_t &userUniqueId, uint8_t index);
	bool StoreUserStatus(UserStatusEnum userStatus, uint8_t index);
	bool LoadUserStatus(UserStatusEnum &userStatus, uint8_t index);
	bool StoreUserType(UserTypeEnum userType, uint8_t index);
	bool LoadUserType(UserTypeEnum &userType, uint8_t index);
	bool StoreUserCreatedBy(uint8_t createdBy, uint8_t index);
	bool LoadUserCreatedBy(uint8_t &createdBy, uint8_t index);
	bool StoreUserLastModifiedBy(uint8_t lastModifiedBy, uint8_t index);
	bool LoadUserLastModifiedBy(uint8_t &lastModifiedBy, uint8_t index);

	bool StoreUserCredentialRule(CredentialRuleEnum credentialRule, uint8_t index);
	bool LoadUserCredentialRule(CredentialRuleEnum &credentialRule, uint8_t index);
	bool StoreCredentialsCount(uint8_t count);
	bool LoadCredentialsCount(uint8_t &count);
	bool StoreCredentialStatus(DlCredentialStatus credentialStatus, uint8_t index);
	bool LoadCredentialStatus(DlCredentialStatus &credentialStatus, uint8_t index);
	bool StoreCredentialType(CredentialTypeEnum credentialType, uint8_t index);
	bool LoadCredentialType(CredentialTypeEnum &credentialType, uint8_t index);
	bool StoreCredentialCreatedBy(uint8_t createdBy, uint8_t index);
	bool LoadCredentialCreatedBy(uint8_t &createdBy, uint8_t index);
	bool StoreCredentialLastModifiedBy(uint8_t lastModifiedBy, uint8_t index);
	bool LoadCredentialLastModifiedBy(uint8_t &lastModifiedBy, uint8_t index);
	bool StoreCredentialSecret(const uint8_t *secret, size_t secretSize, uint8_t index);
	bool LoadCredentialSecret(uint8_t *secret, size_t secretSize, uint8_t index);
	bool StoreCredentialSecretSize(size_t secretSize, uint8_t index);
	bool LoadCredentialSecretSize(size_t &secretSize, uint8_t index);
private:
	PersistentStorageNode mLock;
	PersistentStorageNode mUsersCount;
	PersistentStorageNode mUser;
	PersistentStorageNode mUserData;
	PersistentStorageNode mUserUniqueId;
	PersistentStorageNode mUserStatus;
	PersistentStorageNode mUserType;
	PersistentStorageNode mUserCreateBy;
	PersistentStorageNode mUserLastModifiedBy;
	PersistentStorageNode mUserCredentialRule;

	PersistentStorageNode mCredentialsCount;
	PersistentStorageNode mCredential;
	PersistentStorageNode mCredentialStatus;
	PersistentStorageNode mCredentialType;
	PersistentStorageNode mCredentialCreatedBy;
	PersistentStorageNode mCredentialLastModifiedBy;
	PersistentStorageNode mCredentialSecret;
	PersistentStorageNode mCredentialSecretSize;
};

} /* namespace Nrf */