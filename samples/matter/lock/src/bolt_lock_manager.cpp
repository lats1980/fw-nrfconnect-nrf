/*
 * Copyright (c) 2021 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "bolt_lock_manager.h"

#include "app_event.h"
#include "app_task.h"

using namespace chip;

BoltLockManager BoltLockManager::sLock;

void BoltLockManager::Init(StateChangeCallback callback)
{
	mStateChangeCallback = callback;

	k_timer_init(&mActuatorTimer, &BoltLockManager::ActuatorTimerEventHandler, nullptr);
	k_timer_user_data_set(&mActuatorTimer, this);

	AccessMgr::Instance().Init();
}

bool BoltLockManager::GetUser(uint16_t userIndex, EmberAfPluginDoorLockUserInfo &user)
{
	return AccessMgr::Instance().GetUserInfo(userIndex, user);
}

bool BoltLockManager::SetUser(uint16_t userIndex, FabricIndex creator, FabricIndex modifier, const CharSpan &userName,
			      uint32_t uniqueId, UserStatusEnum userStatus, UserTypeEnum userType,
			      CredentialRuleEnum credentialRule, const CredentialStruct *credentials,
			      size_t totalCredentials)
{
	return AccessMgr::Instance().SetUser(userIndex, creator, modifier, userName, uniqueId, userStatus, userType,
					      credentialRule, credentials, totalCredentials);
}

bool BoltLockManager::GetCredential(uint16_t credentialIndex, CredentialTypeEnum credentialType,
				    EmberAfPluginDoorLockCredentialInfo &credential)
{
	return AccessMgr::Instance().GetCredential(credentialIndex, credentialType, credential);
}

bool BoltLockManager::SetCredential(uint16_t credentialIndex, FabricIndex creator, FabricIndex modifier,
				    DlCredentialStatus credentialStatus, CredentialTypeEnum credentialType,
				    const ByteSpan &secret)
{
	return AccessMgr::Instance().SetCredential(credentialIndex, creator, modifier, credentialStatus,
						    credentialType, secret);
}

bool BoltLockManager::ClearAllUserCredential()
{
	bool ret;
	CredentialStruct credential{ (CredentialTypeEnum)0, 0 };

	for (size_t idxUsr = 0; idxUsr < CONFIG_LOCK_MAX_NUM_USERS; ++idxUsr) {
		ret = AccessMgr::Instance().SetUser(idxUsr + 1, 0, 0, chip::CharSpan(), 0, UserStatusEnum::kAvailable, UserTypeEnum::kUnrestrictedUser,
											CredentialRuleEnum::kSingle, &credential, 0);
		if (!ret) {
			return false;
		}
	}
	for (size_t idxCred = 0; idxCred < CONFIG_LOCK_MAX_NUM_CREDENTIALS_PER_TYPE; ++idxCred) {
		ret = AccessMgr::Instance().SetCredential(idxCred + 1, 0, 0, DlCredentialStatus::kAvailable,
												  CredentialTypeEnum::kPin, chip::ByteSpan());
		if (!ret) {
			return false;
		}
	}

	return true;
}

#ifdef CONFIG_LOCK_SCHEDULES

DlStatus BoltLockManager::GetWeekDaySchedule(uint8_t weekdayIndex, uint16_t userIndex,
					     EmberAfPluginDoorLockWeekDaySchedule &schedule)
{
	return AccessMgr::Instance().GetWeekDaySchedule(weekdayIndex, userIndex, schedule);
}

DlStatus BoltLockManager::SetWeekDaySchedule(uint8_t weekdayIndex, uint16_t userIndex, DlScheduleStatus status,
					     DaysMaskMap daysMask, uint8_t startHour, uint8_t startMinute,
					     uint8_t endHour, uint8_t endMinute)
{
	return AccessMgr::Instance().SetWeekDaySchedule(weekdayIndex, userIndex, status, daysMask, startHour,
							 startMinute, endHour, endMinute);
}

DlStatus BoltLockManager::GetYearDaySchedule(uint8_t yearDayIndex, uint16_t userIndex,
					     EmberAfPluginDoorLockYearDaySchedule &schedule)
{
	return AccessMgr::Instance().GetYearDaySchedule(yearDayIndex, userIndex, schedule);
}

DlStatus BoltLockManager::SetYearDaySchedule(uint8_t yeardayIndex, uint16_t userIndex, DlScheduleStatus status,
					     uint32_t localStartTime, uint32_t localEndTime)
{
	return AccessMgr::Instance().SetYearDaySchedule(yeardayIndex, userIndex, status, localStartTime, localEndTime);
}

DlStatus BoltLockManager::GetHolidaySchedule(uint8_t holidayIndex, EmberAfPluginDoorLockHolidaySchedule &schedule)
{
	return AccessMgr::Instance().GetHolidaySchedule(holidayIndex, schedule);
}

DlStatus BoltLockManager::SetHolidaySchedule(uint8_t holidayIndex, DlScheduleStatus status, uint32_t localStartTime,
					     uint32_t localEndTime, OperatingModeEnum operatingMode)
{
	return AccessMgr::Instance().SetHolidaySchedule(holidayIndex, status, localStartTime, localEndTime,
							 operatingMode);
}

#endif /* CONFIG_LOCK_SCHEDULES */


bool BoltLockManager::ValidatePIN(const Optional<ByteSpan> &pinCode, OperationErrorEnum &err)
{
	return AccessMgr::Instance().ValidatePIN(pinCode, err);
}

void BoltLockManager::SetRequirePIN(bool require)
{
	return AccessMgr::Instance().SetRequirePIN(require);
}
bool BoltLockManager::GetRequirePIN()
{
	return AccessMgr::Instance().GetRequirePIN();
}

#ifdef CONFIG_LOCK_ENABLE_DEBUG
bool BoltLockManager::PrintUserdata(uint8_t userIndex)
{
	/* userIndex is guaranteed by the caller to be between 1 and CONFIG_LOCK_MAX_NUM_USERS */
	VerifyOrReturnError(userIndex > 0 && userIndex <= CONFIG_LOCK_MAX_NUM_USERS, false);
	AccessMgr::Instance().PrintUser(userIndex);
	return true;
}

bool BoltLockManager::PrintCredential(CredentialTypeEnum type, uint16_t credentialIndex)
{
	/* credentialIndex is guaranteed by the caller to be between 1 and CONFIG_LOCK_MAX_NUM_CREDENTIALS_PER_TYPE */
	VerifyOrReturnError(credentialIndex > 0 && credentialIndex <= CONFIG_LOCK_MAX_NUM_CREDENTIALS_PER_TYPE, false);
	AccessMgr::Instance().PrintCredential(type, credentialIndex);
	return true;
}
#endif//CONFIG_LOCK_ENABLE_DEBUG

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
