/*
 * Copyright (C) 2010 The Android Open Source Project
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

// TODO(b/129481165): remove the #pragma below and fix conversion issues
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wconversion"

#undef LOG_TAG
#define LOG_TAG "BufferLayerConsumer"
#define ATRACE_TAG ATRACE_TAG_GRAPHICS
//#define LOG_NDEBUG 0

#include "BufferLayerConsumer.h"
#include "Layer.h"
#include "Scheduler/VsyncController.h"

#include <inttypes.h>

#include <cutils/compiler.h>

#include <hardware/hardware.h>

#include <math/mat4.h>

#include <gui/BufferItem.h>
#include <gui/GLConsumer.h>
#include <gui/ISurfaceComposer.h>
#include <gui/SurfaceComposerClient.h>
#include <private/gui/ComposerService.h>
#include <renderengine/RenderEngine.h>
#include <renderengine/impl/ExternalTexture.h>
#include <utils/Log.h>
#include <utils/String8.h>
#include <utils/Trace.h>

namespace android {

// Macros for including the BufferLayerConsumer name in log messages
#define BLC_LOGV(x, ...) ALOGV("[%s] " x, mName.c_str(), ##__VA_ARGS__)
#define BLC_LOGD(x, ...) ALOGD("[%s] " x, mName.c_str(), ##__VA_ARGS__)
// #define BLC_LOGI(x, ...) ALOGI("[%s] " x, mName.c_str(), ##__VA_ARGS__)
#define BLC_LOGW(x, ...) ALOGW("[%s] " x, mName.c_str(), ##__VA_ARGS__)
#define BLC_LOGE(x, ...) ALOGE("[%s] " x, mName.c_str(), ##__VA_ARGS__)

static const mat4 mtxIdentity;

BufferLayerConsumer::BufferLayerConsumer(const sp<IGraphicBufferConsumer>& bq,
                                         renderengine::RenderEngine& engine, uint32_t tex,
                                         Layer* layer)
      : ConsumerBase(bq, false),
        mCurrentCrop(Rect::EMPTY_RECT),
        mCurrentTransform(0),
        mCurrentScalingMode(NATIVE_WINDOW_SCALING_MODE_FREEZE),
        mCurrentFence(Fence::NO_FENCE),
        mCurrentTimestamp(0),
        mCurrentDataSpace(ui::Dataspace::UNKNOWN),
        mCurrentFrameNumber(0),
        mCurrentTransformToDisplayInverse(false),
        mCurrentSurfaceDamage(),
        mCurrentApi(0),
        mDefaultWidth(1),
        mDefaultHeight(1),
        mFilteringEnabled(true),
        mRE(engine),
        mTexName(tex),
        mLayer(layer),
        mCurrentTexture(BufferQueue::INVALID_BUFFER_SLOT) {
    BLC_LOGV("BufferLayerConsumer");

    memcpy(mCurrentTransformMatrix, mtxIdentity.asArray(), sizeof(mCurrentTransformMatrix));

    mConsumer->setConsumerUsageBits(DEFAULT_USAGE_FLAGS);
}

status_t BufferLayerConsumer::setDefaultBufferSize(uint32_t w, uint32_t h) {
    Mutex::Autolock lock(mMutex);
    if (mAbandoned) {
        BLC_LOGE("setDefaultBufferSize: BufferLayerConsumer is abandoned!");
        return NO_INIT;
    }
    mDefaultWidth = w;
    mDefaultHeight = h;
    return mConsumer->setDefaultBufferSize(w, h);
}

void BufferLayerConsumer::setContentsChangedListener(const wp<ContentsChangedListener>& listener) {
    setFrameAvailableListener(listener);
    Mutex::Autolock lock(mMutex);
    mContentsChangedListener = listener;
}

status_t BufferLayerConsumer::updateTexImage(BufferRejecter* rejecter, nsecs_t expectedPresentTime,
                                             bool* autoRefresh, bool* queuedBuffer,
                                             uint64_t maxFrameNumber) {
    ATRACE_CALL();
    BLC_LOGV("updateTexImage");
    Mutex::Autolock lock(mMutex);

    if (mAbandoned) {
        BLC_LOGE("updateTexImage: BufferLayerConsumer is abandoned!");
        return NO_INIT;
    }

    BufferItem item;

    // Acquire the next buffer.
    // In asynchronous mode the list is guaranteed to be one buffer
    // deep, while in synchronous mode we use the oldest buffer.
    status_t err = acquireBufferLocked(&item, expectedPresentTime, maxFrameNumber);
    if (err != NO_ERROR) {
        if (err == BufferQueue::NO_BUFFER_AVAILABLE) {
            err = NO_ERROR;
        } else if (err == BufferQueue::PRESENT_LATER) {
            // return the error, without logging
        } else {
            BLC_LOGE("updateTexImage: acquire failed: %s (%d)", strerror(-err), err);
        }
        return err;
    }

    if (autoRefresh) {
        *autoRefresh = item.mAutoRefresh;
    }

    if (queuedBuffer) {
        *queuedBuffer = item.mQueuedBuffer;
    }

    // We call the rejecter here, in case the caller has a reason to
    // not accept this buffer.  This is used by SurfaceFlinger to
    // reject buffers which have the wrong size
    int slot = item.mSlot;
    if (rejecter && rejecter->reject(mSlots[slot].mGraphicBuffer, item)) {
        releaseBufferLocked(slot, mSlots[slot].mGraphicBuffer);
        return BUFFER_REJECTED;
    }

    // Release the previous buffer.
    err = updateAndReleaseLocked(item, &mPendingRelease);
    if (err != NO_ERROR) {
        return err;
    }
    return err;
}

void BufferLayerConsumer::setReleaseFence(const sp<Fence>& fence) {
    if (!fence->isValid()) {
        return;
    }

    auto slot = mPendingRelease.isPending ? mPendingRelease.currentTexture : mCurrentTexture;
    if (slot == BufferQueue::INVALID_BUFFER_SLOT) {
        return;
    }

    auto buffer = mPendingRelease.isPending ? mPendingRelease.graphicBuffer
                                            : mCurrentTextureBuffer->getBuffer();
    auto err = addReleaseFence(slot, buffer, fence);
    if (err != OK) {
        BLC_LOGE("setReleaseFence: failed to add the fence: %s (%d)", strerror(-err), err);
    }
}

bool BufferLayerConsumer::releasePendingBuffer() {
    if (!mPendingRelease.isPending) {
        BLC_LOGV("Pending buffer already released");
        return false;
    }
    BLC_LOGV("Releasing pending buffer");
    Mutex::Autolock lock(mMutex);
    status_t result =
            releaseBufferLocked(mPendingRelease.currentTexture, mPendingRelease.graphicBuffer);
    if (result < NO_ERROR) {
        BLC_LOGE("releasePendingBuffer failed: %s (%d)", strerror(-result), result);
    }
    mPendingRelease = PendingRelease();
    return true;
}

sp<Fence> BufferLayerConsumer::getPrevFinalReleaseFence() const {
    Mutex::Autolock lock(mMutex);
    return ConsumerBase::mPrevFinalReleaseFence;
}

status_t BufferLayerConsumer::acquireBufferLocked(BufferItem* item, nsecs_t presentWhen,
                                                  uint64_t maxFrameNumber) {
    status_t err = ConsumerBase::acquireBufferLocked(item, presentWhen, maxFrameNumber);
    if (err != NO_ERROR) {
        return err;
    }

    // If item->mGraphicBuffer is not null, this buffer has not been acquired
    // before, so we need to clean up old references.
    if (item->mGraphicBuffer != nullptr) {
        std::lock_guard<std::mutex> lock(mImagesMutex);
        if (mImages[item->mSlot] == nullptr || mImages[item->mSlot]->getBuffer() == nullptr ||
            mImages[item->mSlot]->getBuffer()->getId() != item->mGraphicBuffer->getId()) {
            mImages[item->mSlot] = std::make_shared<
                    renderengine::impl::ExternalTexture>(item->mGraphicBuffer, mRE,
                                                         renderengine::impl::ExternalTexture::
                                                                 Usage::READABLE);
        }
    }

    return NO_ERROR;
}

status_t BufferLayerConsumer::updateAndReleaseLocked(const BufferItem& item,
                                                     PendingRelease* pendingRelease) {
    status_t err = NO_ERROR;

    int slot = item.mSlot;

    BLC_LOGV("updateAndRelease: (slot=%d buf=%p) -> (slot=%d buf=%p)", mCurrentTexture,
             (mCurrentTextureBuffer != nullptr && mCurrentTextureBuffer->getBuffer() != nullptr)
                     ? mCurrentTextureBuffer->getBuffer()->handle
                     : 0,
             slot, mSlots[slot].mGraphicBuffer->handle);

    // Hang onto the pointer so that it isn't freed in the call to
    // releaseBufferLocked() if we're in shared buffer mode and both buffers are
    // the same.

    std::shared_ptr<renderengine::ExternalTexture> nextTextureBuffer;
    {
        std::lock_guard<std::mutex> lock(mImagesMutex);
        nextTextureBuffer = mImages[slot];
    }

    // release old buffer
    if (mCurrentTexture != BufferQueue::INVALID_BUFFER_SLOT) {
        if (pendingRelease == nullptr) {
            status_t status =
                    releaseBufferLocked(mCurrentTexture, mCurrentTextureBuffer->getBuffer());
            if (status < NO_ERROR) {
                BLC_LOGE("updateAndRelease: failed to release buffer: %s (%d)", strerror(-status),
                         status);
                err = status;
                // keep going, with error raised [?]
            }
        } else {
            pendingRelease->currentTexture = mCurrentTexture;
            pendingRelease->graphicBuffer = mCurrentTextureBuffer->getBuffer();
            pendingRelease->isPending = true;
        }
    }

    // Update the BufferLayerConsumer state.
    mCurrentTexture = slot;
    mCurrentTextureBuffer = nextTextureBuffer;
    mCurrentCrop = item.mCrop;
    mCurrentTransform = item.mTransform;
    mCurrentScalingMode = item.mScalingMode;
    mCurrentTimestamp = item.mTimestamp;
    mCurrentDataSpace = static_cast<ui::Dataspace>(item.mDataSpace);
    mCurrentHdrMetadata = item.mHdrMetadata;
    mCurrentFence = item.mFence;
    mCurrentFenceTime = item.mFenceTime;
    mCurrentFrameNumber = item.mFrameNumber;
    mCurrentTransformToDisplayInverse = item.mTransformToDisplayInverse;
    mCurrentSurfaceDamage = item.mSurfaceDamage;
    mCurrentApi = item.mApi;

    computeCurrentTransformMatrixLocked();

    return err;
}

void BufferLayerConsumer::getTransformMatrix(float mtx[16]) {
    Mutex::Autolock lock(mMutex);
    memcpy(mtx, mCurrentTransformMatrix, sizeof(mCurrentTransformMatrix));
}

void BufferLayerConsumer::setFilteringEnabled(bool enabled) {
    Mutex::Autolock lock(mMutex);
    if (mAbandoned) {
        BLC_LOGE("setFilteringEnabled: BufferLayerConsumer is abandoned!");
        return;
    }
    bool needsRecompute = mFilteringEnabled != enabled;
    mFilteringEnabled = enabled;

    if (needsRecompute && mCurrentTextureBuffer == nullptr) {
        BLC_LOGD("setFilteringEnabled called with mCurrentTextureBuffer == nullptr");
    }

    if (needsRecompute && mCurrentTextureBuffer != nullptr) {
        computeCurrentTransformMatrixLocked();
    }
}

void BufferLayerConsumer::computeCurrentTransformMatrixLocked() {
    BLC_LOGV("computeCurrentTransformMatrixLocked");
    if (mCurrentTextureBuffer == nullptr || mCurrentTextureBuffer->getBuffer() == nullptr) {
        BLC_LOGD("computeCurrentTransformMatrixLocked: "
                 "mCurrentTextureBuffer is nullptr");
    }
    GLConsumer::computeTransformMatrix(mCurrentTransformMatrix,
                                       mCurrentTextureBuffer == nullptr
                                               ? nullptr
                                               : mCurrentTextureBuffer->getBuffer(),
                                       getCurrentCropLocked(), mCurrentTransform,
                                       mFilteringEnabled);
}

nsecs_t BufferLayerConsumer::getTimestamp() {
    BLC_LOGV("getTimestamp");
    Mutex::Autolock lock(mMutex);
    return mCurrentTimestamp;
}

ui::Dataspace BufferLayerConsumer::getCurrentDataSpace() {
    BLC_LOGV("getCurrentDataSpace");
    Mutex::Autolock lock(mMutex);
    return mCurrentDataSpace;
}

const HdrMetadata& BufferLayerConsumer::getCurrentHdrMetadata() const {
    BLC_LOGV("getCurrentHdrMetadata");
    Mutex::Autolock lock(mMutex);
    return mCurrentHdrMetadata;
}

uint64_t BufferLayerConsumer::getFrameNumber() {
    BLC_LOGV("getFrameNumber");
    Mutex::Autolock lock(mMutex);
    return mCurrentFrameNumber;
}

bool BufferLayerConsumer::getTransformToDisplayInverse() const {
    Mutex::Autolock lock(mMutex);
    return mCurrentTransformToDisplayInverse;
}

const Region& BufferLayerConsumer::getSurfaceDamage() const {
    return mCurrentSurfaceDamage;
}

void BufferLayerConsumer::mergeSurfaceDamage(const Region& damage) {
    if (damage.bounds() == Rect::INVALID_RECT ||
        mCurrentSurfaceDamage.bounds() == Rect::INVALID_RECT) {
        mCurrentSurfaceDamage = Region::INVALID_REGION;
    } else {
        mCurrentSurfaceDamage |= damage;
    }
}

int BufferLayerConsumer::getCurrentApi() const {
    Mutex::Autolock lock(mMutex);
    return mCurrentApi;
}

std::shared_ptr<renderengine::ExternalTexture> BufferLayerConsumer::getCurrentBuffer(
        int* outSlot, sp<Fence>* outFence) const {
    Mutex::Autolock lock(mMutex);

    if (outSlot != nullptr) {
        *outSlot = mCurrentTexture;
    }

    if (outFence != nullptr) {
        *outFence = mCurrentFence;
    }

    return mCurrentTextureBuffer == nullptr ? nullptr : mCurrentTextureBuffer;
}

Rect BufferLayerConsumer::getCurrentCrop() const {
    Mutex::Autolock lock(mMutex);
    return getCurrentCropLocked();
}

Rect BufferLayerConsumer::getCurrentCropLocked() const {
    uint32_t width = mDefaultWidth;
    uint32_t height = mDefaultHeight;
    // If the buffer comes with a rotated bit for 90 (or 270) degrees, switch width/height in order
    // to scale and crop correctly.
    if (mCurrentTransform & NATIVE_WINDOW_TRANSFORM_ROT_90) {
        width = mDefaultHeight;
        height = mDefaultWidth;
    }

    return (mCurrentScalingMode == NATIVE_WINDOW_SCALING_MODE_SCALE_CROP)
            ? GLConsumer::scaleDownCrop(mCurrentCrop, width, height)
            : mCurrentCrop;
}

uint32_t BufferLayerConsumer::getCurrentTransform() const {
    Mutex::Autolock lock(mMutex);
    return mCurrentTransform;
}

uint32_t BufferLayerConsumer::getCurrentScalingMode() const {
    Mutex::Autolock lock(mMutex);
    return mCurrentScalingMode;
}

sp<Fence> BufferLayerConsumer::getCurrentFence() const {
    Mutex::Autolock lock(mMutex);
    return mCurrentFence;
}

std::shared_ptr<FenceTime> BufferLayerConsumer::getCurrentFenceTime() const {
    Mutex::Autolock lock(mMutex);
    return mCurrentFenceTime;
}

void BufferLayerConsumer::freeBufferLocked(int slotIndex) {
    BLC_LOGV("freeBufferLocked: slotIndex=%d", slotIndex);
    std::lock_guard<std::mutex> lock(mImagesMutex);
    if (slotIndex == mCurrentTexture) {
        mCurrentTexture = BufferQueue::INVALID_BUFFER_SLOT;
    }
    mImages[slotIndex] = nullptr;
    ConsumerBase::freeBufferLocked(slotIndex);
}

void BufferLayerConsumer::onDisconnect() {
    Mutex::Autolock lock(mMutex);

    if (mAbandoned) {
        // Nothing to do if we're already abandoned.
        return;
    }

    mLayer->onDisconnect();
}

void BufferLayerConsumer::onSidebandStreamChanged() {
    [[maybe_unused]] FrameAvailableListener* unsafeFrameAvailableListener = nullptr;
    {
        Mutex::Autolock lock(mFrameAvailableMutex);
        unsafeFrameAvailableListener = mFrameAvailableListener.unsafe_get();
    }
    sp<ContentsChangedListener> listener;
    { // scope for the lock
        Mutex::Autolock lock(mMutex);
        ALOG_ASSERT(unsafeFrameAvailableListener == mContentsChangedListener.unsafe_get());
        listener = mContentsChangedListener.promote();
    }

    if (listener != nullptr) {
        listener->onSidebandStreamChanged();
    }
}

void BufferLayerConsumer::onBufferAvailable(const BufferItem& item) {
    if (item.mGraphicBuffer != nullptr && item.mSlot != BufferQueue::INVALID_BUFFER_SLOT) {
        std::lock_guard<std::mutex> lock(mImagesMutex);
        const std::shared_ptr<renderengine::ExternalTexture>& oldImage = mImages[item.mSlot];
        if (oldImage == nullptr || oldImage->getBuffer() == nullptr ||
            oldImage->getBuffer()->getId() != item.mGraphicBuffer->getId()) {
            mImages[item.mSlot] = std::make_shared<
                    renderengine::impl::ExternalTexture>(item.mGraphicBuffer, mRE,
                                                         renderengine::impl::ExternalTexture::
                                                                 Usage::READABLE);
        }
    }
}

void BufferLayerConsumer::abandonLocked() {
    BLC_LOGV("abandonLocked");
    mCurrentTextureBuffer = nullptr;
    for (int i = 0; i < BufferQueue::NUM_BUFFER_SLOTS; i++) {
        std::lock_guard<std::mutex> lock(mImagesMutex);
        mImages[i] = nullptr;
    }
    ConsumerBase::abandonLocked();
}

status_t BufferLayerConsumer::setConsumerUsageBits(uint64_t usage) {
    return ConsumerBase::setConsumerUsageBits(usage | DEFAULT_USAGE_FLAGS);
}

void BufferLayerConsumer::dumpLocked(String8& result, const char* prefix) const {
    result.appendFormat("%smTexName=%d mCurrentTexture=%d\n"
                        "%smCurrentCrop=[%d,%d,%d,%d] mCurrentTransform=%#x\n",
                        prefix, mTexName, mCurrentTexture, prefix, mCurrentCrop.left,
                        mCurrentCrop.top, mCurrentCrop.right, mCurrentCrop.bottom,
                        mCurrentTransform);

    ConsumerBase::dumpLocked(result, prefix);
}
}; // namespace android

// TODO(b/129481165): remove the #pragma below and fix conversion issues
#pragma clang diagnostic pop // ignored "-Wconversion"
