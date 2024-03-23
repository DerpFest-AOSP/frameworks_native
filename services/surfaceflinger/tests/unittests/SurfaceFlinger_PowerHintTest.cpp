/*
 * Copyright 2022 The Android Open Source Project
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
#define LOG_TAG "SurfaceFlingerPowerHintTest"

#include <compositionengine/Display.h>
#include <compositionengine/mock/DisplaySurface.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <renderengine/mock/RenderEngine.h>
#include <algorithm>
#include <chrono>
#include <memory>
#include "TestableSurfaceFlinger.h"
#include "mock/DisplayHardware/MockComposer.h"
#include "mock/DisplayHardware/MockPowerAdvisor.h"
#include "mock/MockTimeStats.h"
#include "mock/system/window/MockNativeWindow.h"

using namespace android;
using namespace android::Hwc2::mock;
using namespace android::hardware::power;
using namespace std::chrono_literals;
using namespace testing;

namespace android {
namespace {
using FakeHwcDisplayInjector = TestableSurfaceFlinger::FakeHwcDisplayInjector;
using FakeDisplayDeviceInjector = TestableSurfaceFlinger::FakeDisplayDeviceInjector;

constexpr hal::HWDisplayId HWC_DISPLAY = FakeHwcDisplayInjector::DEFAULT_HWC_DISPLAY_ID;
constexpr PhysicalDisplayId DEFAULT_DISPLAY_ID = PhysicalDisplayId::fromPort(42u);
constexpr int DEFAULT_DISPLAY_WIDTH = 1920;
constexpr int DEFAULT_DISPLAY_HEIGHT = 1024;

class SurfaceFlingerPowerHintTest : public Test {
public:
    void SetUp() override;

protected:
    TestableSurfaceFlinger mFlinger;
    renderengine::mock::RenderEngine* mRenderEngine = new renderengine::mock::RenderEngine();
    sp<DisplayDevice> mDisplay;
    sp<compositionengine::mock::DisplaySurface> mDisplaySurface =
            sp<compositionengine::mock::DisplaySurface>::make();
    sp<mock::NativeWindow> mNativeWindow = sp<mock::NativeWindow>::make();
    mock::TimeStats* mTimeStats = new mock::TimeStats();
    Hwc2::mock::PowerAdvisor* mPowerAdvisor = nullptr;
    Hwc2::mock::Composer* mComposer = nullptr;
};

void SurfaceFlingerPowerHintTest::SetUp() {
    mFlinger.setupMockScheduler({.displayId = DEFAULT_DISPLAY_ID});
    mComposer = new Hwc2::mock::Composer();
    mPowerAdvisor = new Hwc2::mock::PowerAdvisor();
    mFlinger.setupRenderEngine(std::unique_ptr<renderengine::RenderEngine>(mRenderEngine));
    mFlinger.setupTimeStats(std::shared_ptr<TimeStats>(mTimeStats));
    mFlinger.setupComposer(std::unique_ptr<Hwc2::Composer>(mComposer));
    mFlinger.setupPowerAdvisor(std::unique_ptr<Hwc2::PowerAdvisor>(mPowerAdvisor));
    static constexpr bool kIsPrimary = true;
    FakeHwcDisplayInjector(DEFAULT_DISPLAY_ID, hal::DisplayType::PHYSICAL, kIsPrimary)
            .setPowerMode(hal::PowerMode::ON)
            .inject(&mFlinger, mComposer);
    auto compostionEngineDisplayArgs =
            compositionengine::DisplayCreationArgsBuilder()
                    .setId(DEFAULT_DISPLAY_ID)
                    .setPixels({DEFAULT_DISPLAY_WIDTH, DEFAULT_DISPLAY_HEIGHT})
                    .setPowerAdvisor(mPowerAdvisor)
                    .setName("injected display")
                    .build();
    auto compositionDisplay =
            compositionengine::impl::createDisplay(mFlinger.getCompositionEngine(),
                                                   std::move(compostionEngineDisplayArgs));
    mDisplay =
            FakeDisplayDeviceInjector(mFlinger, compositionDisplay,
                                      ui::DisplayConnectionType::Internal, HWC_DISPLAY, kIsPrimary)
                    .setDisplaySurface(mDisplaySurface)
                    .setNativeWindow(mNativeWindow)
                    .setPowerMode(hal::PowerMode::ON)
                    .setRefreshRateSelector(mFlinger.scheduler()->refreshRateSelector())
                    .skipRegisterDisplay()
                    .inject();
}

TEST_F(SurfaceFlingerPowerHintTest, sendDurationsIncludingHwcWaitTime) {
    ON_CALL(*mPowerAdvisor, usePowerHintSession()).WillByDefault(Return(true));

    EXPECT_CALL(*mPowerAdvisor, updateTargetWorkDuration(_)).Times(1);
    EXPECT_CALL(*mDisplaySurface,
                prepareFrame(compositionengine::DisplaySurface::CompositionType::Hwc))
            .Times(1);
    EXPECT_CALL(*mComposer, presentOrValidateDisplay(HWC_DISPLAY, _, _, _, _, _, _)).WillOnce([] {
        constexpr Duration kMockHwcRunTime = 20ms;
        std::this_thread::sleep_for(kMockHwcRunTime);
        return hardware::graphics::composer::V2_1::Error::NONE;
    });
    EXPECT_CALL(*mPowerAdvisor, reportActualWorkDuration()).Times(1);

    const TimePoint frameTime = scheduler::SchedulerClock::now();
    constexpr Period kMockVsyncPeriod = 15ms;
    mFlinger.commitAndComposite(frameTime, VsyncId{123}, frameTime + kMockVsyncPeriod);
}

TEST_F(SurfaceFlingerPowerHintTest, inactiveOnDisplayDoze) {
    ON_CALL(*mPowerAdvisor, usePowerHintSession()).WillByDefault(Return(true));

    mDisplay->setPowerMode(hal::PowerMode::DOZE);

    EXPECT_CALL(*mPowerAdvisor, updateTargetWorkDuration(_)).Times(0);
    EXPECT_CALL(*mDisplaySurface,
                prepareFrame(compositionengine::DisplaySurface::CompositionType::Hwc))
            .Times(1);
    EXPECT_CALL(*mComposer, presentOrValidateDisplay(HWC_DISPLAY, _, _, _, _, _, _)).WillOnce([] {
        constexpr Duration kMockHwcRunTime = 20ms;
        std::this_thread::sleep_for(kMockHwcRunTime);
        return hardware::graphics::composer::V2_1::Error::NONE;
    });
    EXPECT_CALL(*mPowerAdvisor, reportActualWorkDuration()).Times(0);

    const TimePoint frameTime = scheduler::SchedulerClock::now();
    constexpr Period kMockVsyncPeriod = 15ms;
    mFlinger.commitAndComposite(frameTime, VsyncId{123}, frameTime + kMockVsyncPeriod);
}

} // namespace
} // namespace android
