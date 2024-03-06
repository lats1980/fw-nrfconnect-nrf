/*
 * Copyright (c) 2021 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "bolt_lock_manager.h"

#include "app_event.h"
#include "app_task.h"
#include "lock_storage_manager.h"

using namespace chip;

BoltLockManager BoltLockManager::sLock;

void BoltLockManager::Init(StateChangeCallback callback)
{
	mStateChangeCallback = callback;

	k_timer_init(&mActuatorTimer, &BoltLockManager::ActuatorTimerEventHandler, nullptr);
	k_timer_user_data_set(&mActuatorTimer, this);
	bool result;
	result = Nrf::LockStorageManager::Instance().Init();
	ChipLogProgress(Zcl, "Initialize persistent storage: %s", result == true ? "OK" : "Fail");
	/* Restore users and credentials */
	result = BoltLockManager::RestoreUsers();
	ChipLogProgress(Zcl, "Restore users: %s", result == true ? "OK" : "Fail");
	result = BoltLockManager::RestoreCredentials();
	ChipLogProgress(Zcl, "Restore credential: %s", result == true ? "OK" : "Fail");
}

bool BoltLockManager::GetUser(uint16_t userIndex, EmberAfPluginDoorLockUserInfo &user) const
{
	/* userIndex is guaranteed by the caller to be between 1 and CONFIG_LOCK_NUM_USERS */
	user = mUsers[userIndex - 1];

	ChipLogProgress(Zcl, "Getting lock user %u: %s", static_cast<unsigned>(userIndex),
			user.userStatus == UserStatusEnum::kAvailable ? "available" : "occupied");

	return true;
}

bool BoltLockManager::SetUser(uint16_t userIndex, FabricIndex creator, FabricIndex modifier, const CharSpan &userName,
				  uint32_t uniqueId, UserStatusEnum userStatus, UserTypeEnum userType,
				  CredentialRuleEnum credentialRule, const CredentialStruct *credentials,
				  size_t totalCredentials)
{
	/* userIndex is guaranteed by the caller to be between 1 and CONFIG_LOCK_NUM_USERS */
	UserData &userData = mUserData[userIndex - 1];
	auto &user = mUsers[userIndex - 1];

	VerifyOrReturnError(userName.size() <= DOOR_LOCK_MAX_USER_NAME_SIZE, false);
	VerifyOrReturnError(totalCredentials <= CONFIG_LOCK_NUM_CREDENTIALS_PER_USER, false);

	Platform::CopyString(userData.mName, userName);
	memcpy(userData.mCredentials, credentials, totalCredentials * sizeof(CredentialStruct));

	user.userName = CharSpan(userData.mName, userName.size());
	user.credentials = Span<const CredentialStruct>(userData.mCredentials, totalCredentials);
	user.userUniqueId = uniqueId;
	user.userStatus = userStatus;
	user.userType = userType;
	user.credentialRule = credentialRule;
	user.creationSource = DlAssetSource::kMatterIM;
	user.createdBy = creator;
	user.modificationSource = DlAssetSource::kMatterIM;
	user.lastModifiedBy = modifier;

	ChipLogProgress(Zcl, "Setting lock user %u: %s", static_cast<unsigned>(userIndex),
			userStatus == UserStatusEnum::kAvailable ? "available" : "occupied");

	ChipLogProgress(Zcl,
					"Lock App: LockEndpoint::SetUser "
					"[userIndex=%u,creator=%d,modifier=%d,userName=\"%.*s\",uniqueId=%" PRIx32
					",userStatus=%u,userType=%u,"
					"credentialRule=%u,credentials=%p,totalCredentials=%u]",
					userIndex, creator, modifier, static_cast<int>(userName.size()), userName.data(), uniqueId,
					to_underlying(userStatus), to_underlying(userType), to_underlying(credentialRule), credentials,
					static_cast<unsigned int>(totalCredentials));
	if (totalCredentials == 0) {
		ChipLogProgress(Zcl, "Setting lock user without credential");
		return true;
	}
	mTotalUsersCount++;
	VerifyOrReturnError(Nrf::LockStorageManager::Instance().StoreUsersCount(mTotalUsersCount), false);
	VerifyOrReturnError(Nrf::LockStorageManager::Instance().StoreUserData(mUserData[userIndex - 1], userIndex), false);
	VerifyOrReturnError(Nrf::LockStorageManager::Instance().StoreUserUniqueID(user.userUniqueId, userIndex), false);
	VerifyOrReturnError(Nrf::LockStorageManager::Instance().StoreUserType(user.userType, userIndex), false);
	VerifyOrReturnError(Nrf::LockStorageManager::Instance().StoreUserStatus(user.userStatus, userIndex), false);
	VerifyOrReturnError(Nrf::LockStorageManager::Instance().StoreUserCreatedBy(user.createdBy, userIndex), false);
	VerifyOrReturnError(Nrf::LockStorageManager::Instance().StoreUserLastModifiedBy(user.lastModifiedBy, userIndex), false);
	VerifyOrReturnError(Nrf::LockStorageManager::Instance().StoreUserCredentialRule(user.credentialRule, userIndex), false);

	return true;
}

bool BoltLockManager::GetCredential(uint16_t credentialIndex, CredentialTypeEnum credentialType,
					EmberAfPluginDoorLockCredentialInfo &credential) const
{
	VerifyOrReturnError(credentialIndex > 0 && credentialIndex <= CONFIG_LOCK_NUM_CREDENTIALS, false);

	credential = mCredentials[credentialIndex - 1];

	ChipLogProgress(Zcl, "Getting lock credential %u: %s", static_cast<unsigned>(credentialIndex),
			credential.status == DlCredentialStatus::kAvailable ? "available" : "occupied");

	return true;
}

bool BoltLockManager::SetCredential(uint16_t credentialIndex, FabricIndex creator, FabricIndex modifier,
					DlCredentialStatus credentialStatus, CredentialTypeEnum credentialType,
					const ByteSpan &secret)
{
	VerifyOrReturnError(credentialIndex > 0 && credentialIndex <= CONFIG_LOCK_NUM_CREDENTIALS, false);
	VerifyOrReturnError(secret.size() <= kMaxCredentialLength, false);

	CredentialData &credentialData = mCredentialData[credentialIndex - 1];
	auto &credential = mCredentials[credentialIndex - 1];

	if (!secret.empty()) {
		memcpy(credentialData.mSecret.Alloc(secret.size()).Get(), secret.data(), secret.size());
	}

	credential.status = credentialStatus;
	credential.credentialType = credentialType;
	credential.credentialData = ByteSpan(credentialData.mSecret.Get(), secret.size());
	credential.creationSource = DlAssetSource::kMatterIM;
	credential.createdBy = creator;
	credential.modificationSource = DlAssetSource::kMatterIM;
	credential.lastModifiedBy = modifier;

	ChipLogProgress(Zcl, "Setting lock credential %u: %s", static_cast<unsigned>(credentialIndex),
			credential.status == DlCredentialStatus::kAvailable ? "available" : "occupied");

	ChipLogProgress(
		Zcl,
		"Lock App: LockEndpoint::SetCredential "
		"[credentialIndex=%u,credentialStatus=%u,credentialType=%u,credentialDataSize=%u,creator=%u,modifier=%u]",
		credentialIndex, to_underlying(credentialStatus), to_underlying(credentialType),
		static_cast<unsigned int>(secret.size()), creator, modifier);

		// Leaving this logging code for debug, but this cannot be enabled at runtime
		// since it leaks private security material.
#if 0
	ChipLogByteSpan(Zcl, secret);
#endif
	mTotalCredentialsCount++;
	VerifyOrReturnError(Nrf::LockStorageManager::Instance().StoreCredentialsCount(mTotalCredentialsCount), false);
	VerifyOrReturnError(Nrf::LockStorageManager::Instance().StoreCredentialStatus(credentialStatus, credentialIndex), false);
	VerifyOrReturnError(Nrf::LockStorageManager::Instance().StoreCredentialType(credentialType, credentialIndex), false);
	VerifyOrReturnError(Nrf::LockStorageManager::Instance().StoreCredentialCreatedBy(creator, credentialIndex), false);
	VerifyOrReturnError(Nrf::LockStorageManager::Instance().StoreCredentialLastModifiedBy(modifier, credentialIndex), false);
	VerifyOrReturnError(Nrf::LockStorageManager::Instance().StoreCredentialSecretSize(secret.size(), credentialIndex), false);
	VerifyOrReturnError(Nrf::LockStorageManager::Instance().StoreCredentialSecret(secret.data(), secret.size(), credentialIndex), false);

	return true;
}

bool BoltLockManager::ValidatePIN(const Optional<ByteSpan> &pinCode, OperationErrorEnum &err) const
{
	/* Optionality of the PIN code is validated by the caller, so assume it is OK not to provide the PIN code. */
	if (!pinCode.HasValue()) {
		return true;
	}

	/* Check the PIN code */
	for (const auto &credential : mCredentials) {
		if (credential.status == DlCredentialStatus::kAvailable ||
			credential.credentialType != CredentialTypeEnum::kPin) {
			continue;
		}

		if (credential.credentialData.data_equal(pinCode.Value())) {
			ChipLogDetail(Zcl, "Valid lock PIN code provided");
			return true;
		}
	}

	ChipLogDetail(Zcl, "Invalid lock PIN code provided");
	err = OperationErrorEnum::kInvalidCredential;

	return false;
}

void BoltLockManager::Lock(OperationSource source)
{
	VerifyOrReturn(mState != State::kLockingCompleted);
	SetState(State::kLockingInitiated, source);

	mActuatorOperationSource = source;
	k_timer_start(&mActuatorTimer, K_MSEC(kActuatorMovementTimeMs), K_NO_WAIT);
}

void BoltLockManager::Unlock(OperationSource source)
{
	VerifyOrReturn(mState != State::kUnlockingCompleted);
	SetState(State::kUnlockingInitiated, source);

	mActuatorOperationSource = source;
	k_timer_start(&mActuatorTimer, K_MSEC(kActuatorMovementTimeMs), K_NO_WAIT);
}

void BoltLockManager::ActuatorTimerEventHandler(k_timer *timer)
{
	/*
	 * The timer event handler is called in the context of the system clock ISR.
	 * Post an event to the application task queue to process the event in the
	 * context of the application thread.
	 */

	AppEvent event;
	event.Type = AppEventType::Timer;
	event.TimerEvent.Context = static_cast<BoltLockManager *>(k_timer_user_data_get(timer));
	event.Handler = BoltLockManager::ActuatorAppEventHandler;
	AppTask::Instance().PostEvent(event);
}

void BoltLockManager::ActuatorAppEventHandler(const AppEvent &event)
{
	BoltLockManager *lock = static_cast<BoltLockManager *>(event.TimerEvent.Context);

	if (!lock) {
		return;
	}

	switch (lock->mState) {
	case State::kLockingInitiated:
		lock->SetState(State::kLockingCompleted, lock->mActuatorOperationSource);
		break;
	case State::kUnlockingInitiated:
		lock->SetState(State::kUnlockingCompleted, lock->mActuatorOperationSource);
		break;
	default:
		break;
	}
}

void BoltLockManager::SetState(State state, OperationSource source)
{
	mState = state;

	if (mStateChangeCallback != nullptr) {
		mStateChangeCallback(state, source);
	}
}

bool BoltLockManager::RestoreUsers(void)
{
	if (!Nrf::LockStorageManager::Instance().LoadUsersCount(mTotalUsersCount)) {
		ChipLogProgress(Zcl, "No users to load from the storage.");
		return Nrf::LockStorageManager::Instance().StoreUsersCount(mTotalUsersCount);
	}
	ChipLogProgress(Zcl, "Users restored. Total users: %u", mTotalUsersCount);
	/* userIndex is guaranteed by the caller to be between 1 and CONFIG_LOCK_NUM_USERS */
	for (size_t userIndex = 1; userIndex <= mTotalUsersCount; userIndex++)
	{
		UserData &userData = mUserData[userIndex - 1];
		auto &user = mUsers[userIndex];
		Nrf::LockStorageManager::Instance().LoadUserData(userData, userIndex);
		for (auto &userCredential : userData.mCredentials) {
			ChipLogProgress(Zcl, "Credential type: %u index: %u", (uint8_t)userCredential.credentialType, userCredential.credentialIndex);
		}
		VerifyOrReturnError(Nrf::LockStorageManager::Instance().LoadUserUniqueID(user.userUniqueId, userIndex), false);
		ChipLogProgress(Zcl, "Users UID loaded: 0x%X", user.userUniqueId);
		VerifyOrReturnError(Nrf::LockStorageManager::Instance().LoadUserType(user.userType, userIndex), false);
		ChipLogProgress(Zcl, "Users type: %u", (uint8_t)user.userType);
		VerifyOrReturnError(Nrf::LockStorageManager::Instance().LoadUserStatus(user.userStatus, userIndex), false);
		ChipLogProgress(Zcl, "Users status: %u", (uint8_t)user.userStatus);
		VerifyOrReturnError(Nrf::LockStorageManager::Instance().LoadUserCreatedBy(user.createdBy, userIndex), false);
		ChipLogProgress(Zcl, "Credential created by: %u", user.createdBy);
		VerifyOrReturnError(Nrf::LockStorageManager::Instance().LoadUserLastModifiedBy(user.lastModifiedBy, userIndex), false);
		ChipLogProgress(Zcl, "Credential last modified by: %u", (uint8_t)user.lastModifiedBy);
		VerifyOrReturnError(Nrf::LockStorageManager::Instance().LoadUserCredentialRule(user.credentialRule, userIndex), false);
		ChipLogProgress(Zcl, "Credential rule: %u", (uint8_t)user.credentialRule);
		// So far there's no way to actually create the credential outside the matter, so here we always set the creation/modification
		// source to Matter
		user.creationSource = DlAssetSource::kMatterIM;
		user.modificationSource = DlAssetSource::kMatterIM;
		user.userName = CharSpan(userData.mName, DOOR_LOCK_USER_NAME_BUFFER_SIZE);
		user.credentials = Span<const CredentialStruct>(userData.mCredentials, sizeof(userData.mCredentials));
		user.createdBy = user.createdBy;
		user.lastModifiedBy = user.lastModifiedBy;
	}

	return true;
}

bool BoltLockManager::RestoreCredentials(void)
{
	if (!Nrf::LockStorageManager::Instance().LoadCredentialsCount(mTotalCredentialsCount)) {
		ChipLogProgress(Zcl, "No credentials devices to load from the storage.");
		return Nrf::LockStorageManager::Instance().StoreCredentialsCount(mTotalCredentialsCount);
	}
	ChipLogProgress(Zcl, "Crendential restored. Total credentials: %u", mTotalCredentialsCount);

	for (size_t credentialIndex = 1; credentialIndex <= mTotalCredentialsCount; credentialIndex++)
	{
		size_t secretSize;
		CredentialData &credentialData = mCredentialData[credentialIndex - 1];
		auto &credential = mCredentials[credentialIndex - 1];
		VerifyOrReturnError(Nrf::LockStorageManager::Instance().LoadCredentialStatus(credential.status, credentialIndex), false);
		ChipLogProgress(Zcl, "Cred status: %u", (uint8_t)credential.status);
		VerifyOrReturnError(Nrf::LockStorageManager::Instance().LoadCredentialType(credential.credentialType, credentialIndex), false);
		ChipLogProgress(Zcl, "Cred type: %u", (uint8_t)credential.credentialType);
		VerifyOrReturnError(Nrf::LockStorageManager::Instance().LoadCredentialCreatedBy(credential.createdBy, credentialIndex), false);
		ChipLogProgress(Zcl, "Cred created by: %u", credential.createdBy);
		VerifyOrReturnError(Nrf::LockStorageManager::Instance().LoadCredentialLastModifiedBy(credential.lastModifiedBy, credentialIndex), false);
		ChipLogProgress(Zcl, "Cred last modified by: %u", credential.lastModifiedBy);
		VerifyOrReturnError(Nrf::LockStorageManager::Instance().LoadCredentialSecretSize(secretSize, credentialIndex), false);
		if (secretSize > 0 && secretSize <= kMaxCredentialLength) {
			credentialData.mSecret.Alloc(secretSize);
			VerifyOrReturnError(Nrf::LockStorageManager::Instance().LoadCredentialSecret(credentialData.mSecret.Get(), secretSize, credentialIndex) , false);
			credential.credentialData = ByteSpan(credentialData.mSecret.Get(), secretSize);
			// Leaving this logging code for debug, but this cannot be enabled at runtime
			// since it leaks private security material.
#if 0
			ChipLogProgress(Zcl, "Credential secret:");
			ChipLogByteSpan(Zcl, credential.credentialData);
#endif
		} else {
			ChipLogProgress(Zcl, "Invalid credential size");
			return false;
		}
		credential.creationSource = DlAssetSource::kMatterIM;
		credential.modificationSource = DlAssetSource::kMatterIM;
	}

	return true;
}
