/*
 * Copyright 2019 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#undef LOG_TAG
#define LOG_TAG "VSyncReactor"
//#define LOG_NDEBUG 0
#include "VSyncReactor.h"
#include <log/log.h>
#include "TimeKeeper.h"
#include "VSyncDispatch.h"
#include "VSyncTracker.h"

namespace android::scheduler {

Clock::~Clock() = default;
nsecs_t SystemClock::now() const {
    return systemTime(SYSTEM_TIME_MONOTONIC);
}

VSyncReactor::VSyncReactor(std::unique_ptr<Clock> clock, std::unique_ptr<VSyncDispatch> dispatch,
                           std::unique_ptr<VSyncTracker> tracker, size_t pendingFenceLimit)
      : mClock(std::move(clock)),
        mTracker(std::move(tracker)),
        mDispatch(std::move(dispatch)),
        mPendingLimit(pendingFenceLimit) {}

VSyncReactor::~VSyncReactor() = default;

// The DispSync interface has a 'repeat this callback at rate' semantic. This object adapts
// VSyncDispatch's individually-scheduled callbacks so as to meet DispSync's existing semantic
// for now.
class CallbackRepeater {
public:
    CallbackRepeater(VSyncDispatch& dispatch, DispSync::Callback* cb, const char* name,
                     nsecs_t period, nsecs_t offset, nsecs_t notBefore)
          : mCallback(cb),
            mRegistration(dispatch,
                          std::bind(&CallbackRepeater::callback, this, std::placeholders::_1,
                                    std::placeholders::_2),
                          std::string(name)),
            mPeriod(period),
            mOffset(offset),
            mLastCallTime(notBefore) {}

    ~CallbackRepeater() {
        std::lock_guard<std::mutex> lk(mMutex);
        mRegistration.cancel();
    }

    void start(nsecs_t offset) {
        std::lock_guard<std::mutex> lk(mMutex);
        mStopped = false;
        mOffset = offset;

        auto const schedule_result = mRegistration.schedule(calculateWorkload(), mLastCallTime);
        LOG_ALWAYS_FATAL_IF((schedule_result != ScheduleResult::Scheduled),
                            "Error scheduling callback: rc %X", schedule_result);
    }

    void setPeriod(nsecs_t period) {
        std::lock_guard<std::mutex> lk(mMutex);
        if (period == mPeriod) {
            return;
        }
        mPeriod = period;
    }

    void stop() {
        std::lock_guard<std::mutex> lk(mMutex);
        LOG_ALWAYS_FATAL_IF(mStopped, "DispSyncInterface misuse: callback already stopped");
        mStopped = true;
        mRegistration.cancel();
    }

private:
    void callback(nsecs_t vsynctime, nsecs_t wakeupTime) {
        {
            std::lock_guard<std::mutex> lk(mMutex);
            mLastCallTime = vsynctime;
        }

        mCallback->onDispSyncEvent(wakeupTime);

        {
            std::lock_guard<std::mutex> lk(mMutex);
            auto const schedule_result = mRegistration.schedule(calculateWorkload(), vsynctime);
            LOG_ALWAYS_FATAL_IF((schedule_result != ScheduleResult::Scheduled),
                                "Error rescheduling callback: rc %X", schedule_result);
        }
    }

    // DispSync offsets are defined as time after the vsync before presentation.
    // VSyncReactor workloads are defined as time before the intended presentation vsync.
    // Note change in sign between the two defnitions.
    nsecs_t calculateWorkload() REQUIRES(mMutex) { return mPeriod - mOffset; }

    DispSync::Callback* const mCallback;

    std::mutex mutable mMutex;
    VSyncCallbackRegistration mRegistration GUARDED_BY(mMutex);
    bool mStopped GUARDED_BY(mMutex) = false;
    nsecs_t mPeriod GUARDED_BY(mMutex);
    nsecs_t mOffset GUARDED_BY(mMutex);
    nsecs_t mLastCallTime GUARDED_BY(mMutex);
};

bool VSyncReactor::addPresentFence(const std::shared_ptr<FenceTime>& fence) {
    if (!fence) {
        return false;
    }

    nsecs_t const signalTime = fence->getCachedSignalTime();
    if (signalTime == Fence::SIGNAL_TIME_INVALID) {
        return true;
    }

    std::lock_guard<std::mutex> lk(mMutex);
    if (mIgnorePresentFences) {
        return true;
    }

    for (auto it = mUnfiredFences.begin(); it != mUnfiredFences.end();) {
        auto const time = (*it)->getCachedSignalTime();
        if (time == Fence::SIGNAL_TIME_PENDING) {
            it++;
        } else if (time == Fence::SIGNAL_TIME_INVALID) {
            it = mUnfiredFences.erase(it);
        } else {
            mTracker->addVsyncTimestamp(time);
            it = mUnfiredFences.erase(it);
        }
    }

    if (signalTime == Fence::SIGNAL_TIME_PENDING) {
        if (mPendingLimit == mUnfiredFences.size()) {
            mUnfiredFences.erase(mUnfiredFences.begin());
        }
        mUnfiredFences.push_back(fence);
    } else {
        mTracker->addVsyncTimestamp(signalTime);
    }

    return mMoreSamplesNeeded;
}

void VSyncReactor::setIgnorePresentFences(bool ignoration) {
    std::lock_guard<std::mutex> lk(mMutex);
    mIgnorePresentFences = ignoration;
    if (mIgnorePresentFences == true) {
        mUnfiredFences.clear();
    }
}

nsecs_t VSyncReactor::computeNextRefresh(int periodOffset) const {
    auto const now = mClock->now();
    auto const currentPeriod = periodOffset ? mTracker->currentPeriod() : 0;
    return mTracker->nextAnticipatedVSyncTimeFrom(now + periodOffset * currentPeriod);
}

nsecs_t VSyncReactor::expectedPresentTime() {
    return mTracker->nextAnticipatedVSyncTimeFrom(mClock->now());
}

void VSyncReactor::startPeriodTransition(nsecs_t newPeriod) {
    mPeriodTransitioningTo = newPeriod;
    mMoreSamplesNeeded = true;
}

void VSyncReactor::endPeriodTransition() {
    mPeriodTransitioningTo.reset();
    mLastHwVsync.reset();
    mMoreSamplesNeeded = false;
}

void VSyncReactor::setPeriod(nsecs_t period) {
    std::lock_guard lk(mMutex);
    mLastHwVsync.reset();
    if (period == getPeriod()) {
        endPeriodTransition();
    } else {
        startPeriodTransition(period);
    }
}

nsecs_t VSyncReactor::getPeriod() {
    return mTracker->currentPeriod();
}

void VSyncReactor::beginResync() {}

void VSyncReactor::endResync() {}

bool VSyncReactor::periodChangeDetected(nsecs_t vsync_timestamp) {
    if (!mLastHwVsync || !mPeriodTransitioningTo) {
        return false;
    }
    auto const distance = vsync_timestamp - *mLastHwVsync;
    return std::abs(distance - *mPeriodTransitioningTo) < std::abs(distance - getPeriod());
}

bool VSyncReactor::addResyncSample(nsecs_t timestamp, bool* periodFlushed) {
    assert(periodFlushed);

    std::lock_guard<std::mutex> lk(mMutex);
    if (periodChangeDetected(timestamp)) {
        mTracker->setPeriod(*mPeriodTransitioningTo);
        for (auto& entry : mCallbacks) {
            entry.second->setPeriod(*mPeriodTransitioningTo);
        }

        endPeriodTransition();
        *periodFlushed = true;
    } else if (mPeriodTransitioningTo) {
        mLastHwVsync = timestamp;
        mMoreSamplesNeeded = true;
        *periodFlushed = false;
    } else {
        mMoreSamplesNeeded = false;
        *periodFlushed = false;
    }

    mTracker->addVsyncTimestamp(timestamp);
    return mMoreSamplesNeeded;
}

status_t VSyncReactor::addEventListener(const char* name, nsecs_t phase,
                                        DispSync::Callback* callback,
                                        nsecs_t /* lastCallbackTime */) {
    std::lock_guard<std::mutex> lk(mMutex);
    auto it = mCallbacks.find(callback);
    if (it == mCallbacks.end()) {
        // TODO (b/146557561): resolve lastCallbackTime semantics in DispSync i/f.
        static auto constexpr maxListeners = 3;
        if (mCallbacks.size() >= maxListeners) {
            ALOGE("callback %s not added, exceeded callback limit of %i (currently %zu)", name,
                  maxListeners, mCallbacks.size());
            return NO_MEMORY;
        }

        auto const period = mTracker->currentPeriod();
        auto repeater = std::make_unique<CallbackRepeater>(*mDispatch, callback, name, period,
                                                           phase, mClock->now());
        it = mCallbacks.emplace(std::pair(callback, std::move(repeater))).first;
    }

    it->second->start(phase);
    return NO_ERROR;
}

status_t VSyncReactor::removeEventListener(DispSync::Callback* callback,
                                           nsecs_t* /* outLastCallback */) {
    std::lock_guard<std::mutex> lk(mMutex);
    auto const it = mCallbacks.find(callback);
    LOG_ALWAYS_FATAL_IF(it == mCallbacks.end(), "callback %p not registered", callback);

    it->second->stop();
    return NO_ERROR;
}

status_t VSyncReactor::changePhaseOffset(DispSync::Callback* callback, nsecs_t phase) {
    std::lock_guard<std::mutex> lk(mMutex);
    auto const it = mCallbacks.find(callback);
    LOG_ALWAYS_FATAL_IF(it == mCallbacks.end(), "callback was %p not registered", callback);

    it->second->start(phase);
    return NO_ERROR;
}

void VSyncReactor::dump(std::string& result) const {
    result += "VsyncReactor in use\n"; // TODO (b/144927823): add more information!
}

void VSyncReactor::reset() {}

} // namespace android::scheduler
