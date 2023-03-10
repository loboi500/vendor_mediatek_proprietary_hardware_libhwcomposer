#define DEBUG_LOG_TAG "FPSCOUNTER"

#include <stdint.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <utils/String8.h>
#include <utils/Errors.h>
#include <log/log.h>
#include <utils/threads.h>

#include <cutils/compiler.h>
#include <cutils/properties.h>

#include "debug.h"
#include "fpscounter.h"


namespace android {

//--------------------------------------------------------------------------------------------------
bool FpsCounter::reset() {
    mFps = 0.0;

    mMaxDuration = -1;
    mMinDuration = -1;
    mMaxDurationCounting = -1;
    mMinDurationCounting = -1;

    mFrames = 0;
    mLastLogTime = -1;
    mLastLogDuration = -1;

    mLastTime = -1;
    mLastDuration = -1;

    // read property as default log interval setting
    char value[PROPERTY_VALUE_MAX] = {0};
    property_get("vendor.debug.hwc.stc_interval", value, "1000");
    mCountInterval = ms2ns(atoi(value));

    return true;
}

bool FpsCounter::update() {
    return update(systemTime(SYSTEM_TIME_MONOTONIC));
}

bool FpsCounter::update(const nsecs_t& timestamp) {
    if ((-1 == mLastLogTime) || (-1 == mLastTime) || (mLastTime >= timestamp)) {
        mLastLogTime = mLastTime = timestamp;
        return false;
    }

    mFrames++;

    // count duration from last time update
    mLastDuration = timestamp - mLastTime;
    mLastTime = timestamp;
    if ((-1 == mMaxDurationCounting) || (mLastDuration > mMaxDurationCounting)) {
        mMaxDurationCounting = mLastDuration;
    }
    if ((-1 == mMinDurationCounting) || (mLastDuration < mMinDurationCounting)) {
        mMinDurationCounting = mLastDuration;
    }

    // check if reach statistics interval, print result and reset for next
    nsecs_t duration = timestamp - mLastLogTime;
    if (duration > mCountInterval) {

        // update data for FPS result
        mFps = static_cast<float>(mFrames * 1e9 / duration);
        mLastLogDuration = duration;
        mMaxDuration = mMaxDurationCounting;
        mMinDuration = mMinDurationCounting;

        // write to ring buffer
        mRingBuffer.WriteToBuffer(*this);

        // reset counting data for next
        mFrames = 0;
        mLastLogTime = timestamp;
        mMaxDurationCounting = -1;
        mMinDurationCounting = -1;

        return true;
    }

    return false;
}

void FpsCounter::RingBufferFps::WriteToBuffer(const FpsCounter& obj) {
    BufContent now;
    gettimeofday(&now.mTv, NULL);
    now.mFps             = obj.getFps();
    now.mMaxDuration     = obj.getMaxDuration();
    now.mMinDuration     = obj.getMinDuration();
    now.mLastLogDuration = obj.getLastLogDuration();
    if (mBufSize >= mIdx) mIdx = mIdx % mBufSize;

    if (mIdx < mBufSize) {
        mBuf[mIdx] = now;
        mIdx++;
    }
}

void FpsCounter::dump(String8* result, const char* prefix) {
    std::vector<RingBufferFps::BufContent>& RingBuf = mRingBuffer.getBuf();
    result->appendFormat("%s FPS ring buffer:\n", prefix);

    for (size_t i = 0; i < RingBuf.size(); i++) {
        if (0 != RingBuf[i].mFps && 0 != RingBuf[i].mLastLogDuration) {
        const size_t buf_size = 30;
        char buffer[buf_size] = {0};
        tm* time = localtime(&(RingBuf[i].mTv.tv_sec));
        if (time != nullptr) {
            size_t ret = strftime(buffer, buf_size - 1, "%T.", time);
            if (CC_UNLIKELY(ret == 0))
            {
                HWC_LOGE("%s(), strftime fail", __FUNCTION__);
            }
            result->appendFormat("%s (%zu) %-9s%-3d fps=%-5.2f dur=%-13.2f max=%-13.2f min=%-5.2f\n",
                prefix, i, buffer, static_cast<int>(RingBuf[i].mTv.tv_usec/1e3),
                static_cast<double>(RingBuf[i].mFps),
                RingBuf[i].mLastLogDuration / 1e6,
                RingBuf[i].mMaxDuration / 1e6,
                RingBuf[i].mMinDuration / 1e6);
            }
        }
        else
        {
            HWC_LOGE("localtime() returns a null object");
        }
    }

}

// ----------------------------------------------------------------------------
}; // namespace android
