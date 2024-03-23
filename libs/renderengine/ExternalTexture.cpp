/*
 * Copyright 2021 The Android Open Source Project
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

#include <log/log.h>
#include <renderengine/RenderEngine.h>
#include <renderengine/impl/ExternalTexture.h>
#include <ui/GraphicBuffer.h>
#include <utils/Trace.h>

namespace android::renderengine::impl {

ExternalTexture::ExternalTexture(const sp<GraphicBuffer>& buffer,
                                 renderengine::RenderEngine& renderEngine, uint32_t usage)
      : mBuffer(buffer), mRenderEngine(renderEngine), mWritable(usage & WRITEABLE) {
    LOG_ALWAYS_FATAL_IF(buffer == nullptr,
                        "Attempted to bind a null buffer to an external texture!");
    mRenderEngine.mapExternalTextureBuffer(mBuffer, mWritable);
}

ExternalTexture::~ExternalTexture() {
    mRenderEngine.unmapExternalTextureBuffer(std::move(mBuffer));
}

void ExternalTexture::remapBuffer() {
    ATRACE_CALL();
    {
        auto buf = mBuffer;
        mRenderEngine.unmapExternalTextureBuffer(std::move(buf));
    }
    mRenderEngine.mapExternalTextureBuffer(mBuffer, mWritable);
}

} // namespace android::renderengine::impl
