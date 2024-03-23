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

#pragma once

#include <gmock/gmock.h>
#include <optional>

namespace android::mock {

class MockLayer : public Layer {
public:
    MockLayer(SurfaceFlinger* flinger, std::string name)
          : Layer(LayerCreationArgs(flinger, nullptr, std::move(name), 0, {})) {
        EXPECT_CALL(*this, getDefaultFrameRateCompatibility())
                .WillOnce(testing::Return(scheduler::FrameRateCompatibility::Default));
    }

    MockLayer(SurfaceFlinger* flinger, std::string name, std::optional<uint32_t> uid)
          : Layer(LayerCreationArgs(flinger, nullptr, std::move(name), 0, {}, uid)) {
        EXPECT_CALL(*this, getDefaultFrameRateCompatibility())
                .WillOnce(testing::Return(scheduler::FrameRateCompatibility::Default));
    }

    explicit MockLayer(SurfaceFlinger* flinger) : MockLayer(flinger, "TestLayer") {}

    MOCK_CONST_METHOD0(getType, const char*());
    MOCK_METHOD0(getFrameSelectionPriority, int32_t());
    MOCK_CONST_METHOD0(isVisible, bool());
    MOCK_METHOD1(createClone, sp<Layer>(uint32_t));
    MOCK_CONST_METHOD0(getFrameRateForLayerTree, FrameRate());
    MOCK_CONST_METHOD0(getDefaultFrameRateCompatibility, scheduler::FrameRateCompatibility());
    MOCK_CONST_METHOD0(getOwnerUid, uid_t());
    MOCK_CONST_METHOD0(getDataSpace, ui::Dataspace());
    MOCK_METHOD(bool, isFrontBuffered, (), (const, override));
};

} // namespace android::mock
