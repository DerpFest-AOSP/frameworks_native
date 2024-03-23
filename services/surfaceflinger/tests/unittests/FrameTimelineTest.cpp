/*
 * Copyright 2020 The Android Open Source Project
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

#include <common/test/FlagUtils.h>
#include "com_android_graphics_surfaceflinger_flags.h"
#include "gmock/gmock-spec-builders.h"
#include "mock/MockTimeStats.h"
#undef LOG_TAG
#define LOG_TAG "LibSurfaceFlingerUnittests"

#include <FrameTimeline/FrameTimeline.h>
#include <gtest/gtest.h>
#include <log/log.h>
#include <perfetto/trace/trace.pb.h>
#include <cinttypes>

using namespace std::chrono_literals;
using testing::_;
using testing::AtLeast;
using testing::Contains;
using FrameTimelineEvent = perfetto::protos::FrameTimelineEvent;
using ProtoExpectedDisplayFrameStart =
        perfetto::protos::FrameTimelineEvent_ExpectedDisplayFrameStart;
using ProtoExpectedSurfaceFrameStart =
        perfetto::protos::FrameTimelineEvent_ExpectedSurfaceFrameStart;
using ProtoActualDisplayFrameStart = perfetto::protos::FrameTimelineEvent_ActualDisplayFrameStart;
using ProtoActualSurfaceFrameStart = perfetto::protos::FrameTimelineEvent_ActualSurfaceFrameStart;
using ProtoFrameEnd = perfetto::protos::FrameTimelineEvent_FrameEnd;
using ProtoPresentType = perfetto::protos::FrameTimelineEvent_PresentType;
using ProtoJankType = perfetto::protos::FrameTimelineEvent_JankType;
using ProtoJankSeverityType = perfetto::protos::FrameTimelineEvent_JankSeverityType;
using ProtoPredictionType = perfetto::protos::FrameTimelineEvent_PredictionType;

namespace android::frametimeline {

static const std::string sLayerNameOne = "layer1";
static const std::string sLayerNameTwo = "layer2";

constexpr const uid_t sUidOne = 0;
constexpr pid_t sPidOne = 10;
constexpr pid_t sPidTwo = 20;
constexpr int32_t sInputEventId = 5;
constexpr int32_t sLayerIdOne = 1;
constexpr int32_t sLayerIdTwo = 2;
constexpr GameMode sGameMode = GameMode::Unsupported;
constexpr Fps RR_11 = Fps::fromPeriodNsecs(11);
constexpr Fps RR_30 = Fps::fromPeriodNsecs(30);

class FrameTimelineTest : public testing::Test {
public:
    FrameTimelineTest() {
        const ::testing::TestInfo* const test_info =
                ::testing::UnitTest::GetInstance()->current_test_info();
        ALOGD("**** Setting up for %s.%s\n", test_info->test_case_name(), test_info->name());
    }

    ~FrameTimelineTest() {
        const ::testing::TestInfo* const test_info =
                ::testing::UnitTest::GetInstance()->current_test_info();
        ALOGD("**** Tearing down after %s.%s\n", test_info->test_case_name(), test_info->name());
    }

    static void SetUpTestSuite() {
        // Need to initialize tracing in process for testing, and only once per test suite.
        perfetto::TracingInitArgs args;
        args.backends = perfetto::kInProcessBackend;
        perfetto::Tracing::Initialize(args);
    }

    void SetUp() override {
        constexpr bool kUseBootTimeClock = true;
        mTimeStats = std::make_shared<mock::TimeStats>();
        mFrameTimeline = std::make_unique<impl::FrameTimeline>(mTimeStats, kSurfaceFlingerPid,
                                                               kTestThresholds, !kUseBootTimeClock);
        mFrameTimeline->registerDataSource();
        mTokenManager = &mFrameTimeline->mTokenManager;
        mTraceCookieCounter = &mFrameTimeline->mTraceCookieCounter;
        maxDisplayFrames = &mFrameTimeline->mMaxDisplayFrames;
        maxTokens = mTokenManager->kMaxTokens;
    }

    // Each tracing session can be used for a single block of Start -> Stop.
    static std::unique_ptr<perfetto::TracingSession> getTracingSessionForTest() {
        perfetto::TraceConfig cfg;
        cfg.set_duration_ms(500);
        cfg.add_buffers()->set_size_kb(1024);
        auto* ds_cfg = cfg.add_data_sources()->mutable_config();
        ds_cfg->set_name(impl::FrameTimeline::kFrameTimelineDataSource);

        auto tracingSession = perfetto::Tracing::NewTrace(perfetto::kInProcessBackend);
        tracingSession->Setup(cfg);
        return tracingSession;
    }

    std::vector<perfetto::protos::TracePacket> readFrameTimelinePacketsBlocking(
            perfetto::TracingSession* tracingSession) {
        std::vector<char> raw_trace = tracingSession->ReadTraceBlocking();
        perfetto::protos::Trace trace;
        EXPECT_TRUE(trace.ParseFromArray(raw_trace.data(), int(raw_trace.size())));

        std::vector<perfetto::protos::TracePacket> packets;
        for (const auto& packet : trace.packet()) {
            if (!packet.has_frame_timeline_event()) {
                continue;
            }
            packets.emplace_back(packet);
        }
        return packets;
    }

    void addEmptySurfaceFrame() {
        auto surfaceFrame =
                mFrameTimeline->createSurfaceFrameForToken({}, sPidOne, sUidOne, sLayerIdOne,
                                                           sLayerNameOne, sLayerNameOne,
                                                           /*isBuffer*/ false, sGameMode);
        mFrameTimeline->addSurfaceFrame(std::move(surfaceFrame));
    }

    void addEmptyDisplayFrame() {
        auto presentFence1 = fenceFactory.createFenceTimeForTest(Fence::NO_FENCE);
        // Trigger a flushPresentFence by calling setSfPresent for the next frame
        mFrameTimeline->setSfPresent(2500, presentFence1);
    }

    void flushTokens() {
        for (size_t i = 0; i < maxTokens; i++) {
            mTokenManager->generateTokenForPredictions({});
        }
        EXPECT_EQ(getPredictions().size(), maxTokens);
    }

    SurfaceFrame& getSurfaceFrame(size_t displayFrameIdx, size_t surfaceFrameIdx) {
        std::lock_guard<std::mutex> lock(mFrameTimeline->mMutex);
        return *(mFrameTimeline->mDisplayFrames[displayFrameIdx]
                         ->getSurfaceFrames()[surfaceFrameIdx]);
    }

    std::shared_ptr<impl::FrameTimeline::DisplayFrame> getDisplayFrame(size_t idx) {
        std::lock_guard<std::mutex> lock(mFrameTimeline->mMutex);
        return mFrameTimeline->mDisplayFrames[idx];
    }

    static bool compareTimelineItems(const TimelineItem& a, const TimelineItem& b) {
        return a.startTime == b.startTime && a.endTime == b.endTime &&
                a.presentTime == b.presentTime;
    }

    const std::map<int64_t, TimelineItem>& getPredictions() const {
        return mTokenManager->mPredictions;
    }

    uint32_t getNumberOfDisplayFrames() const {
        std::lock_guard<std::mutex> lock(mFrameTimeline->mMutex);
        return static_cast<uint32_t>(mFrameTimeline->mDisplayFrames.size());
    }

    int64_t snoopCurrentTraceCookie() const { return mTraceCookieCounter->mTraceCookie; }

    void flushTrace() {
        using FrameTimelineDataSource = impl::FrameTimeline::FrameTimelineDataSource;
        FrameTimelineDataSource::Trace(
                [&](FrameTimelineDataSource::TraceContext ctx) { ctx.Flush(); });
    }

    std::shared_ptr<mock::TimeStats> mTimeStats;
    std::unique_ptr<impl::FrameTimeline> mFrameTimeline;
    impl::TokenManager* mTokenManager;
    TraceCookieCounter* mTraceCookieCounter;
    FenceToFenceTimeMap fenceFactory;
    uint32_t* maxDisplayFrames;
    size_t maxTokens;
    static constexpr pid_t kSurfaceFlingerPid = 666;
    static constexpr nsecs_t kPresentThreshold = std::chrono::nanoseconds(2ns).count();
    static constexpr nsecs_t kDeadlineThreshold = std::chrono::nanoseconds(0ns).count();
    static constexpr nsecs_t kStartThreshold = std::chrono::nanoseconds(2ns).count();
    static constexpr JankClassificationThresholds kTestThresholds{kPresentThreshold,
                                                                  kDeadlineThreshold,
                                                                  kStartThreshold};
};

TEST_F(FrameTimelineTest, tokenManagerRemovesStalePredictions) {
    int64_t token1 = mTokenManager->generateTokenForPredictions({0, 0, 0});
    EXPECT_EQ(getPredictions().size(), 1u);
    flushTokens();
    int64_t token2 = mTokenManager->generateTokenForPredictions({10, 20, 30});
    std::optional<TimelineItem> predictions = mTokenManager->getPredictionsForToken(token1);

    // token1 should have expired
    EXPECT_EQ(predictions.has_value(), false);

    predictions = mTokenManager->getPredictionsForToken(token2);
    EXPECT_EQ(compareTimelineItems(*predictions, TimelineItem(10, 20, 30)), true);
}

TEST_F(FrameTimelineTest, createSurfaceFrameForToken_getOwnerPidReturnsCorrectPid) {
    auto surfaceFrame1 =
            mFrameTimeline->createSurfaceFrameForToken({}, sPidOne, sUidOne, sLayerIdOne,
                                                       sLayerNameOne, sLayerNameOne,
                                                       /*isBuffer*/ true, sGameMode);
    auto surfaceFrame2 =
            mFrameTimeline->createSurfaceFrameForToken({}, sPidTwo, sUidOne, sLayerIdOne,
                                                       sLayerNameOne, sLayerNameOne,
                                                       /*isBuffer*/ true, sGameMode);
    EXPECT_EQ(surfaceFrame1->getOwnerPid(), sPidOne);
    EXPECT_EQ(surfaceFrame2->getOwnerPid(), sPidTwo);
}

TEST_F(FrameTimelineTest, createSurfaceFrameForToken_noToken) {
    auto surfaceFrame =
            mFrameTimeline->createSurfaceFrameForToken({}, sPidOne, sUidOne, sLayerIdOne,
                                                       sLayerNameOne, sLayerNameOne,
                                                       /*isBuffer*/ true, sGameMode);
    EXPECT_EQ(surfaceFrame->getPredictionState(), PredictionState::None);
}

TEST_F(FrameTimelineTest, createSurfaceFrameForToken_expiredToken) {
    int64_t token1 = mTokenManager->generateTokenForPredictions({0, 0, 0});
    flushTokens();
    FrameTimelineInfo ftInfo;
    ftInfo.vsyncId = token1;
    ftInfo.inputEventId = sInputEventId;
    auto surfaceFrame =
            mFrameTimeline->createSurfaceFrameForToken(ftInfo, sPidOne, sUidOne, sLayerIdOne,
                                                       sLayerNameOne, sLayerNameOne,
                                                       /*isBuffer*/ true, sGameMode);

    EXPECT_EQ(surfaceFrame->getPredictionState(), PredictionState::Expired);
}

TEST_F(FrameTimelineTest, createSurfaceFrameForToken_validToken) {
    int64_t token1 = mTokenManager->generateTokenForPredictions({10, 20, 30});
    FrameTimelineInfo ftInfo;
    ftInfo.vsyncId = token1;
    ftInfo.inputEventId = sInputEventId;
    auto surfaceFrame =
            mFrameTimeline->createSurfaceFrameForToken(ftInfo, sPidOne, sUidOne, sLayerIdOne,
                                                       sLayerNameOne, sLayerNameOne,
                                                       /*isBuffer*/ true, sGameMode);

    EXPECT_EQ(surfaceFrame->getPredictionState(), PredictionState::Valid);
    EXPECT_EQ(compareTimelineItems(surfaceFrame->getPredictions(), TimelineItem(10, 20, 30)), true);
}

TEST_F(FrameTimelineTest, createSurfaceFrameForToken_validInputEventId) {
    int64_t token1 = mTokenManager->generateTokenForPredictions({10, 20, 30});
    constexpr int32_t inputEventId = 1;
    FrameTimelineInfo ftInfo;
    ftInfo.vsyncId = token1;
    ftInfo.inputEventId = inputEventId;
    auto surfaceFrame =
            mFrameTimeline->createSurfaceFrameForToken(ftInfo, sPidOne, sUidOne, sLayerIdOne,
                                                       sLayerNameOne, sLayerNameOne,
                                                       /*isBuffer*/ true, sGameMode);

    EXPECT_EQ(inputEventId, surfaceFrame->getInputEventId());
}

TEST_F(FrameTimelineTest, presentFenceSignaled_droppedFramesNotUpdated) {
    auto presentFence1 = fenceFactory.createFenceTimeForTest(Fence::NO_FENCE);
    int64_t token1 = mTokenManager->generateTokenForPredictions({10, 20, 30});
    FrameTimelineInfo ftInfo;
    ftInfo.vsyncId = token1;
    ftInfo.inputEventId = sInputEventId;
    auto surfaceFrame1 =
            mFrameTimeline->createSurfaceFrameForToken(ftInfo, sPidOne, sUidOne, sLayerIdOne,
                                                       sLayerNameOne, sLayerNameOne,
                                                       /*isBuffer*/ true, sGameMode);

    // Set up the display frame
    mFrameTimeline->setSfWakeUp(token1, 20, RR_11, RR_11);
    surfaceFrame1->setDropTime(12);
    surfaceFrame1->setPresentState(SurfaceFrame::PresentState::Dropped);
    mFrameTimeline->addSurfaceFrame(surfaceFrame1);
    mFrameTimeline->setSfPresent(25, presentFence1);
    presentFence1->signalForTest(30);

    addEmptyDisplayFrame();

    auto& droppedSurfaceFrame = getSurfaceFrame(0, 0);
    EXPECT_EQ(droppedSurfaceFrame.getPresentState(), SurfaceFrame::PresentState::Dropped);
    EXPECT_EQ(0u, droppedSurfaceFrame.getActuals().endTime);
    EXPECT_EQ(12u, droppedSurfaceFrame.getDropTime());
    EXPECT_EQ(droppedSurfaceFrame.getActuals().presentTime, 0);
}

TEST_F(FrameTimelineTest, presentFenceSignaled_presentedFramesUpdated) {
    // Layer specific increment
    EXPECT_CALL(*mTimeStats, incrementJankyFrames(_)).Times(2);
    auto presentFence1 = fenceFactory.createFenceTimeForTest(Fence::NO_FENCE);
    int64_t surfaceFrameToken1 = mTokenManager->generateTokenForPredictions({10, 20, 30});
    int64_t sfToken1 = mTokenManager->generateTokenForPredictions({22, 26, 30});
    FrameTimelineInfo ftInfo;
    ftInfo.vsyncId = surfaceFrameToken1;
    ftInfo.inputEventId = sInputEventId;
    auto surfaceFrame1 =
            mFrameTimeline->createSurfaceFrameForToken(ftInfo, sPidOne, sUidOne, sLayerIdOne,
                                                       sLayerNameOne, sLayerNameOne,
                                                       /*isBuffer*/ true, sGameMode);
    auto surfaceFrame2 =
            mFrameTimeline->createSurfaceFrameForToken(ftInfo, sPidOne, sUidOne, sLayerIdTwo,
                                                       sLayerNameTwo, sLayerNameTwo,
                                                       /*isBuffer*/ true, sGameMode);
    mFrameTimeline->setSfWakeUp(sfToken1, 22, RR_11, RR_11);
    surfaceFrame1->setPresentState(SurfaceFrame::PresentState::Presented);
    mFrameTimeline->addSurfaceFrame(surfaceFrame1);
    surfaceFrame2->setPresentState(SurfaceFrame::PresentState::Presented);
    mFrameTimeline->addSurfaceFrame(surfaceFrame2);
    mFrameTimeline->setSfPresent(26, presentFence1);
    auto displayFrame = getDisplayFrame(0);
    auto& presentedSurfaceFrame1 = getSurfaceFrame(0, 0);
    auto& presentedSurfaceFrame2 = getSurfaceFrame(0, 1);
    presentFence1->signalForTest(42);

    // Fences haven't been flushed yet, so it should be 0
    EXPECT_EQ(displayFrame->getActuals().presentTime, 0);
    EXPECT_EQ(presentedSurfaceFrame1.getActuals().presentTime, 0);
    EXPECT_EQ(presentedSurfaceFrame2.getActuals().presentTime, 0);

    addEmptyDisplayFrame();

    // Fences have flushed, so the present timestamps should be updated
    EXPECT_EQ(displayFrame->getActuals().presentTime, 42);
    EXPECT_EQ(presentedSurfaceFrame1.getActuals().presentTime, 42);
    EXPECT_EQ(presentedSurfaceFrame2.getActuals().presentTime, 42);
    EXPECT_NE(surfaceFrame1->getJankType(), std::nullopt);
    EXPECT_NE(surfaceFrame1->getJankSeverityType(), std::nullopt);
    EXPECT_NE(surfaceFrame2->getJankType(), std::nullopt);
    EXPECT_NE(surfaceFrame2->getJankSeverityType(), std::nullopt);
}

TEST_F(FrameTimelineTest, displayFramesSlidingWindowMovesAfterLimit) {
    // Insert kMaxDisplayFrames' count of DisplayFrames to fill the deque
    int frameTimeFactor = 0;
    // Layer specific increment
    EXPECT_CALL(*mTimeStats, incrementJankyFrames(_))
            .Times(static_cast<int32_t>(*maxDisplayFrames));
    for (size_t i = 0; i < *maxDisplayFrames; i++) {
        auto presentFence = fenceFactory.createFenceTimeForTest(Fence::NO_FENCE);
        int64_t surfaceFrameToken = mTokenManager->generateTokenForPredictions(
                {10 + frameTimeFactor, 20 + frameTimeFactor, 30 + frameTimeFactor});
        int64_t sfToken = mTokenManager->generateTokenForPredictions(
                {22 + frameTimeFactor, 26 + frameTimeFactor, 30 + frameTimeFactor});
        FrameTimelineInfo ftInfo;
        ftInfo.vsyncId = surfaceFrameToken;
        ftInfo.inputEventId = sInputEventId;
        auto surfaceFrame =
                mFrameTimeline->createSurfaceFrameForToken(ftInfo, sPidOne, sUidOne, sLayerIdOne,
                                                           sLayerNameOne, sLayerNameOne,
                                                           /*isBuffer*/ true, sGameMode);
        mFrameTimeline->setSfWakeUp(sfToken, 22 + frameTimeFactor, RR_11, RR_11);
        surfaceFrame->setPresentState(SurfaceFrame::PresentState::Presented);
        mFrameTimeline->addSurfaceFrame(surfaceFrame);
        mFrameTimeline->setSfPresent(27 + frameTimeFactor, presentFence);
        presentFence->signalForTest(32 + frameTimeFactor);
        frameTimeFactor += 30;
    }
    auto displayFrame0 = getDisplayFrame(0);

    // The 0th Display Frame should have actuals 22, 27, 32
    EXPECT_EQ(compareTimelineItems(displayFrame0->getActuals(), TimelineItem(22, 27, 32)), true);

    // Add one more display frame
    auto presentFence = fenceFactory.createFenceTimeForTest(Fence::NO_FENCE);
    int64_t surfaceFrameToken = mTokenManager->generateTokenForPredictions(
            {10 + frameTimeFactor, 20 + frameTimeFactor, 30 + frameTimeFactor});
    int64_t sfToken = mTokenManager->generateTokenForPredictions(
            {22 + frameTimeFactor, 26 + frameTimeFactor, 30 + frameTimeFactor});
    FrameTimelineInfo ftInfo;
    ftInfo.vsyncId = surfaceFrameToken;
    ftInfo.inputEventId = sInputEventId;
    auto surfaceFrame =
            mFrameTimeline->createSurfaceFrameForToken(ftInfo, sPidOne, sUidOne, sLayerIdOne,
                                                       sLayerNameOne, sLayerNameOne,
                                                       /*isBuffer*/ true, sGameMode);
    mFrameTimeline->setSfWakeUp(sfToken, 22 + frameTimeFactor, RR_11, RR_11);
    surfaceFrame->setPresentState(SurfaceFrame::PresentState::Presented);
    mFrameTimeline->addSurfaceFrame(surfaceFrame);
    mFrameTimeline->setSfPresent(27 + frameTimeFactor, presentFence);
    presentFence->signalForTest(32 + frameTimeFactor);
    displayFrame0 = getDisplayFrame(0);

    // The window should have slided by 1 now and the previous 0th display frame
    // should have been removed from the deque
    EXPECT_EQ(compareTimelineItems(displayFrame0->getActuals(), TimelineItem(52, 57, 62)), true);
}

TEST_F(FrameTimelineTest, surfaceFrameEndTimeAcquireFenceAfterQueue) {
    auto surfaceFrame = mFrameTimeline->createSurfaceFrameForToken({}, sPidOne, 0, sLayerIdOne,
                                                                   "acquireFenceAfterQueue",
                                                                   "acquireFenceAfterQueue",
                                                                   /*isBuffer*/ true, sGameMode);
    surfaceFrame->setActualQueueTime(123);
    surfaceFrame->setAcquireFenceTime(456);
    EXPECT_EQ(surfaceFrame->getActuals().endTime, 456);
}

TEST_F(FrameTimelineTest, surfaceFrameEndTimeAcquireFenceBeforeQueue) {
    auto surfaceFrame = mFrameTimeline->createSurfaceFrameForToken({}, sPidOne, 0, sLayerIdOne,
                                                                   "acquireFenceAfterQueue",
                                                                   "acquireFenceAfterQueue",
                                                                   /*isBuffer*/ true, sGameMode);
    surfaceFrame->setActualQueueTime(456);
    surfaceFrame->setAcquireFenceTime(123);
    EXPECT_EQ(surfaceFrame->getActuals().endTime, 456);
}

TEST_F(FrameTimelineTest, setMaxDisplayFramesSetsSizeProperly) {
    auto presentFence = fenceFactory.createFenceTimeForTest(Fence::NO_FENCE);
    presentFence->signalForTest(2);

    // Size shouldn't exceed maxDisplayFrames - 64
    for (size_t i = 0; i < *maxDisplayFrames + 10; i++) {
        auto surfaceFrame =
                mFrameTimeline->createSurfaceFrameForToken({}, sPidOne, sUidOne, sLayerIdOne,
                                                           sLayerNameOne, sLayerNameOne,
                                                           /*isBuffer*/ true, sGameMode);
        int64_t sfToken = mTokenManager->generateTokenForPredictions({22, 26, 30});
        mFrameTimeline->setSfWakeUp(sfToken, 22, RR_11, RR_11);
        surfaceFrame->setPresentState(SurfaceFrame::PresentState::Presented);
        mFrameTimeline->addSurfaceFrame(surfaceFrame);
        mFrameTimeline->setSfPresent(27, presentFence);
    }
    EXPECT_EQ(getNumberOfDisplayFrames(), *maxDisplayFrames);

    // Increase the size to 256
    mFrameTimeline->setMaxDisplayFrames(256);
    EXPECT_EQ(*maxDisplayFrames, 256u);

    for (size_t i = 0; i < *maxDisplayFrames + 10; i++) {
        auto surfaceFrame =
                mFrameTimeline->createSurfaceFrameForToken({}, sPidOne, sUidOne, sLayerIdOne,
                                                           sLayerNameOne, sLayerNameOne,
                                                           /*isBuffer*/ true, sGameMode);
        int64_t sfToken = mTokenManager->generateTokenForPredictions({22, 26, 30});
        mFrameTimeline->setSfWakeUp(sfToken, 22, RR_11, RR_11);
        surfaceFrame->setPresentState(SurfaceFrame::PresentState::Presented);
        mFrameTimeline->addSurfaceFrame(surfaceFrame);
        mFrameTimeline->setSfPresent(27, presentFence);
    }
    EXPECT_EQ(getNumberOfDisplayFrames(), *maxDisplayFrames);

    // Shrink the size to 128
    mFrameTimeline->setMaxDisplayFrames(128);
    EXPECT_EQ(*maxDisplayFrames, 128u);

    for (size_t i = 0; i < *maxDisplayFrames + 10; i++) {
        auto surfaceFrame =
                mFrameTimeline->createSurfaceFrameForToken({}, sPidOne, sUidOne, sLayerIdOne,
                                                           sLayerNameOne, sLayerNameOne,
                                                           /*isBuffer*/ true, sGameMode);
        int64_t sfToken = mTokenManager->generateTokenForPredictions({22, 26, 30});
        mFrameTimeline->setSfWakeUp(sfToken, 22, RR_11, RR_11);
        surfaceFrame->setPresentState(SurfaceFrame::PresentState::Presented);
        mFrameTimeline->addSurfaceFrame(surfaceFrame);
        mFrameTimeline->setSfPresent(27, presentFence);
    }
    EXPECT_EQ(getNumberOfDisplayFrames(), *maxDisplayFrames);
}

TEST_F(FrameTimelineTest, presentFenceSignaled_invalidSignalTime) {
    Fps refreshRate = RR_11;

    auto presentFence1 = fenceFactory.createFenceTimeForTest(Fence::NO_FENCE);
    int64_t surfaceFrameToken1 = mTokenManager->generateTokenForPredictions({10, 20, 60});
    int64_t sfToken1 = mTokenManager->generateTokenForPredictions({52, 60, 60});
    FrameTimelineInfo ftInfo;
    ftInfo.vsyncId = surfaceFrameToken1;
    ftInfo.inputEventId = sInputEventId;

    auto surfaceFrame1 =
            mFrameTimeline->createSurfaceFrameForToken(ftInfo, sPidOne, sUidOne, sLayerIdOne,
                                                       sLayerNameOne, sLayerNameOne,
                                                       /*isBuffer*/ true, sGameMode);
    mFrameTimeline->setSfWakeUp(sfToken1, 52, refreshRate, refreshRate);
    surfaceFrame1->setAcquireFenceTime(20);
    surfaceFrame1->setPresentState(SurfaceFrame::PresentState::Presented);
    mFrameTimeline->addSurfaceFrame(surfaceFrame1);

    mFrameTimeline->setSfPresent(59, presentFence1);
    presentFence1->signalForTest(-1);
    addEmptyDisplayFrame();

    auto displayFrame0 = getDisplayFrame(0);
    EXPECT_EQ(displayFrame0->getActuals().presentTime, 59);
    EXPECT_EQ(displayFrame0->getJankType(), JankType::Unknown | JankType::DisplayHAL);
    EXPECT_EQ(displayFrame0->getJankSeverityType(), JankSeverityType::Unknown);
    EXPECT_EQ(surfaceFrame1->getActuals().presentTime, -1);
    EXPECT_EQ(surfaceFrame1->getJankType(), JankType::Unknown);
    EXPECT_EQ(surfaceFrame1->getJankSeverityType(), JankSeverityType::Unknown);
}

// Tests related to TimeStats
TEST_F(FrameTimelineTest, presentFenceSignaled_doesNotReportForInvalidTokens) {
    Fps refreshRate = RR_11;
    EXPECT_CALL(*mTimeStats, incrementJankyFrames(_)).Times(0);
    auto presentFence1 = fenceFactory.createFenceTimeForTest(Fence::NO_FENCE);
    int64_t surfaceFrameToken1 = -1;
    int64_t sfToken1 = -1;
    FrameTimelineInfo ftInfo;
    ftInfo.vsyncId = surfaceFrameToken1;
    ftInfo.inputEventId = sInputEventId;

    auto surfaceFrame1 =
            mFrameTimeline->createSurfaceFrameForToken(ftInfo, sPidOne, sUidOne, sLayerIdOne,
                                                       sLayerNameOne, sLayerNameOne,
                                                       /*isBuffer*/ true, sGameMode);
    mFrameTimeline->setSfWakeUp(sfToken1, 52, refreshRate, refreshRate);
    surfaceFrame1->setAcquireFenceTime(20);
    surfaceFrame1->setPresentState(SurfaceFrame::PresentState::Presented);
    mFrameTimeline->addSurfaceFrame(surfaceFrame1);
    presentFence1->signalForTest(70);

    mFrameTimeline->setSfPresent(59, presentFence1);
}

TEST_F(FrameTimelineTest, presentFenceSignaled_reportsLongSfCpu) {
    Fps refreshRate = RR_11;
    EXPECT_CALL(*mTimeStats,
                incrementJankyFrames(
                        TimeStats::JankyFramesInfo{refreshRate, std::nullopt, sUidOne,
                                                   sLayerNameOne, sGameMode,
                                                   JankType::SurfaceFlingerCpuDeadlineMissed, 2, 10,
                                                   0}));
    auto presentFence1 = fenceFactory.createFenceTimeForTest(Fence::NO_FENCE);
    int64_t surfaceFrameToken1 = mTokenManager->generateTokenForPredictions({10, 20, 60});
    int64_t sfToken1 = mTokenManager->generateTokenForPredictions({52, 60, 60});
    FrameTimelineInfo ftInfo;
    ftInfo.vsyncId = surfaceFrameToken1;
    ftInfo.inputEventId = sInputEventId;

    auto surfaceFrame1 =
            mFrameTimeline->createSurfaceFrameForToken(ftInfo, sPidOne, sUidOne, sLayerIdOne,
                                                       sLayerNameOne, sLayerNameOne,
                                                       /*isBuffer*/ true, sGameMode);
    mFrameTimeline->setSfWakeUp(sfToken1, 52, refreshRate, refreshRate);
    surfaceFrame1->setAcquireFenceTime(20);
    surfaceFrame1->setPresentState(SurfaceFrame::PresentState::Presented);
    mFrameTimeline->addSurfaceFrame(surfaceFrame1);
    presentFence1->signalForTest(70);

    mFrameTimeline->setSfPresent(62, presentFence1);
}

TEST_F(FrameTimelineTest, presentFenceSignaled_reportsLongSfGpu) {
    Fps refreshRate = RR_11;
    EXPECT_CALL(*mTimeStats,
                incrementJankyFrames(
                        TimeStats::JankyFramesInfo{refreshRate, std::nullopt, sUidOne,
                                                   sLayerNameOne, sGameMode,
                                                   JankType::SurfaceFlingerGpuDeadlineMissed, 4, 10,
                                                   0}));
    auto presentFence1 = fenceFactory.createFenceTimeForTest(Fence::NO_FENCE);
    auto gpuFence1 = fenceFactory.createFenceTimeForTest(Fence::NO_FENCE);
    int64_t surfaceFrameToken1 = mTokenManager->generateTokenForPredictions({10, 20, 60});
    int64_t sfToken1 = mTokenManager->generateTokenForPredictions({52, 60, 60});
    FrameTimelineInfo ftInfo;
    ftInfo.vsyncId = surfaceFrameToken1;
    ftInfo.inputEventId = sInputEventId;

    auto surfaceFrame1 =
            mFrameTimeline->createSurfaceFrameForToken(ftInfo, sPidOne, sUidOne, sLayerIdOne,
                                                       sLayerNameOne, sLayerNameOne,
                                                       /*isBuffer*/ true, sGameMode);
    mFrameTimeline->setSfWakeUp(sfToken1, 52, refreshRate, refreshRate);
    surfaceFrame1->setAcquireFenceTime(20);
    surfaceFrame1->setPresentState(SurfaceFrame::PresentState::Presented);
    mFrameTimeline->addSurfaceFrame(surfaceFrame1);
    gpuFence1->signalForTest(64);
    presentFence1->signalForTest(70);

    mFrameTimeline->setSfPresent(59, presentFence1, gpuFence1);
}

TEST_F(FrameTimelineTest, presentFenceSignaled_reportsDisplayMiss) {
    Fps refreshRate = RR_30;
    EXPECT_CALL(*mTimeStats,
                incrementJankyFrames(TimeStats::JankyFramesInfo{refreshRate, std::nullopt, sUidOne,
                                                                sLayerNameOne, sGameMode,
                                                                JankType::DisplayHAL, -4, 0, 0}));

    auto presentFence1 = fenceFactory.createFenceTimeForTest(Fence::NO_FENCE);
    int64_t surfaceFrameToken1 = mTokenManager->generateTokenForPredictions({10, 20, 60});
    int64_t sfToken1 = mTokenManager->generateTokenForPredictions({52, 60, 60});
    FrameTimelineInfo ftInfo;
    ftInfo.vsyncId = surfaceFrameToken1;
    ftInfo.inputEventId = sInputEventId;

    auto surfaceFrame1 =
            mFrameTimeline->createSurfaceFrameForToken(ftInfo, sPidOne, sUidOne, sLayerIdOne,
                                                       sLayerNameOne, sLayerNameOne,
                                                       /*isBuffer*/ true, sGameMode);
    mFrameTimeline->setSfWakeUp(sfToken1, 52, refreshRate, refreshRate);
    surfaceFrame1->setPresentState(SurfaceFrame::PresentState::Presented);
    surfaceFrame1->setAcquireFenceTime(20);
    mFrameTimeline->addSurfaceFrame(surfaceFrame1);
    presentFence1->signalForTest(90);
    mFrameTimeline->setSfPresent(56, presentFence1);
    EXPECT_EQ(surfaceFrame1->getJankType(), JankType::DisplayHAL);
    EXPECT_EQ(surfaceFrame1->getJankSeverityType(), JankSeverityType::Full);
}

TEST_F(FrameTimelineTest, presentFenceSignaled_reportsAppMiss) {
    Fps refreshRate = 11_Hz;
    EXPECT_CALL(*mTimeStats,
                incrementJankyFrames(TimeStats::JankyFramesInfo{refreshRate, std::nullopt, sUidOne,
                                                                sLayerNameOne, sGameMode,
                                                                JankType::AppDeadlineMissed, -4, 0,
                                                                25}));
    auto presentFence1 = fenceFactory.createFenceTimeForTest(Fence::NO_FENCE);
    int64_t surfaceFrameToken1 = mTokenManager->generateTokenForPredictions({10, 20, 60});
    int64_t sfToken1 = mTokenManager->generateTokenForPredictions({82, 90, 90});
    FrameTimelineInfo ftInfo;
    ftInfo.vsyncId = surfaceFrameToken1;
    ftInfo.inputEventId = sInputEventId;

    auto surfaceFrame1 =
            mFrameTimeline->createSurfaceFrameForToken(ftInfo, sPidOne, sUidOne, sLayerIdOne,
                                                       sLayerNameOne, sLayerNameOne,
                                                       /*isBuffer*/ true, sGameMode);
    surfaceFrame1->setAcquireFenceTime(45);
    mFrameTimeline->setSfWakeUp(sfToken1, 52, refreshRate, refreshRate);

    surfaceFrame1->setPresentState(SurfaceFrame::PresentState::Presented);
    mFrameTimeline->addSurfaceFrame(surfaceFrame1);
    presentFence1->signalForTest(90);
    mFrameTimeline->setSfPresent(86, presentFence1);

    EXPECT_EQ(surfaceFrame1->getJankType(), JankType::AppDeadlineMissed);
    EXPECT_EQ(surfaceFrame1->getJankSeverityType(), JankSeverityType::Partial);
}

TEST_F(FrameTimelineTest, presentFenceSignaled_reportsSfScheduling) {
    Fps refreshRate = Fps::fromPeriodNsecs(32);
    EXPECT_CALL(*mTimeStats,
                incrementJankyFrames(TimeStats::JankyFramesInfo{refreshRate, std::nullopt, sUidOne,
                                                                sLayerNameOne, sGameMode,
                                                                JankType::SurfaceFlingerScheduling,
                                                                -4, 0, -10}));
    auto presentFence1 = fenceFactory.createFenceTimeForTest(Fence::NO_FENCE);
    int64_t surfaceFrameToken1 = mTokenManager->generateTokenForPredictions({40, 60, 92});
    int64_t sfToken1 = mTokenManager->generateTokenForPredictions({52, 60, 60});
    FrameTimelineInfo ftInfo;
    ftInfo.vsyncId = surfaceFrameToken1;
    ftInfo.inputEventId = sInputEventId;

    auto surfaceFrame1 =
            mFrameTimeline->createSurfaceFrameForToken(ftInfo, sPidOne, sUidOne, sLayerIdOne,
                                                       sLayerNameOne, sLayerNameOne,
                                                       /*isBuffer*/ true, sGameMode);
    surfaceFrame1->setAcquireFenceTime(50);
    mFrameTimeline->setSfWakeUp(sfToken1, 52, refreshRate, refreshRate);

    surfaceFrame1->setPresentState(SurfaceFrame::PresentState::Presented);
    mFrameTimeline->addSurfaceFrame(surfaceFrame1);
    presentFence1->signalForTest(60);
    mFrameTimeline->setSfPresent(56, presentFence1);

    EXPECT_EQ(surfaceFrame1->getJankType(), JankType::SurfaceFlingerScheduling);
    EXPECT_EQ(surfaceFrame1->getJankSeverityType(), JankSeverityType::Full);
}

TEST_F(FrameTimelineTest, presentFenceSignaled_reportsSfPredictionError) {
    Fps refreshRate = Fps::fromPeriodNsecs(16);
    EXPECT_CALL(*mTimeStats,
                incrementJankyFrames(TimeStats::JankyFramesInfo{refreshRate, std::nullopt, sUidOne,
                                                                sLayerNameOne, sGameMode,
                                                                JankType::PredictionError, -4, 5,
                                                                0}));
    auto presentFence1 = fenceFactory.createFenceTimeForTest(Fence::NO_FENCE);
    int64_t surfaceFrameToken1 = mTokenManager->generateTokenForPredictions({30, 40, 60});
    int64_t sfToken1 = mTokenManager->generateTokenForPredictions({52, 60, 60});
    FrameTimelineInfo ftInfo;
    ftInfo.vsyncId = surfaceFrameToken1;
    ftInfo.inputEventId = sInputEventId;

    auto surfaceFrame1 =
            mFrameTimeline->createSurfaceFrameForToken(ftInfo, sPidOne, sUidOne, sLayerIdOne,
                                                       sLayerNameOne, sLayerNameOne,
                                                       /*isBuffer*/ true, sGameMode);
    surfaceFrame1->setAcquireFenceTime(40);
    mFrameTimeline->setSfWakeUp(sfToken1, 52, refreshRate, refreshRate);

    surfaceFrame1->setPresentState(SurfaceFrame::PresentState::Presented);
    mFrameTimeline->addSurfaceFrame(surfaceFrame1);
    presentFence1->signalForTest(65);
    mFrameTimeline->setSfPresent(56, presentFence1);

    EXPECT_EQ(surfaceFrame1->getJankType(), JankType::PredictionError);
    EXPECT_EQ(surfaceFrame1->getJankSeverityType(), JankSeverityType::Partial);
}

TEST_F(FrameTimelineTest, presentFenceSignaled_reportsAppBufferStuffing) {
    Fps refreshRate = Fps::fromPeriodNsecs(32);
    EXPECT_CALL(*mTimeStats,
                incrementJankyFrames(TimeStats::JankyFramesInfo{refreshRate, std::nullopt, sUidOne,
                                                                sLayerNameOne, sGameMode,
                                                                JankType::BufferStuffing, -4, 0,
                                                                0}));
    auto presentFence1 = fenceFactory.createFenceTimeForTest(Fence::NO_FENCE);
    int64_t surfaceFrameToken1 = mTokenManager->generateTokenForPredictions({30, 40, 58});
    int64_t sfToken1 = mTokenManager->generateTokenForPredictions({82, 90, 90});
    FrameTimelineInfo ftInfo;
    ftInfo.vsyncId = surfaceFrameToken1;
    ftInfo.inputEventId = sInputEventId;

    auto surfaceFrame1 =
            mFrameTimeline->createSurfaceFrameForToken(ftInfo, sPidOne, sUidOne, sLayerIdOne,
                                                       sLayerNameOne, sLayerNameOne,
                                                       /*isBuffer*/ true, sGameMode);
    surfaceFrame1->setAcquireFenceTime(40);
    mFrameTimeline->setSfWakeUp(sfToken1, 82, refreshRate, refreshRate);

    surfaceFrame1->setPresentState(SurfaceFrame::PresentState::Presented,
                                   /*previousLatchTime*/ 56);
    mFrameTimeline->addSurfaceFrame(surfaceFrame1);
    presentFence1->signalForTest(90);
    mFrameTimeline->setSfPresent(86, presentFence1);

    EXPECT_EQ(surfaceFrame1->getJankType(), JankType::BufferStuffing);
    EXPECT_EQ(surfaceFrame1->getJankSeverityType(), JankSeverityType::Full);
}

TEST_F(FrameTimelineTest, presentFenceSignaled_reportsAppMissWithRenderRate) {
    Fps refreshRate = RR_11;
    Fps renderRate = RR_30;
    EXPECT_CALL(*mTimeStats,
                incrementJankyFrames(TimeStats::JankyFramesInfo{refreshRate, renderRate, sUidOne,
                                                                sLayerNameOne, sGameMode,
                                                                JankType::AppDeadlineMissed, -4, 0,
                                                                25}));
    auto presentFence1 = fenceFactory.createFenceTimeForTest(Fence::NO_FENCE);
    int64_t surfaceFrameToken1 = mTokenManager->generateTokenForPredictions({10, 20, 60});
    int64_t sfToken1 = mTokenManager->generateTokenForPredictions({82, 90, 90});
    FrameTimelineInfo ftInfo;
    ftInfo.vsyncId = surfaceFrameToken1;
    ftInfo.inputEventId = sInputEventId;

    auto surfaceFrame1 =
            mFrameTimeline->createSurfaceFrameForToken(ftInfo, sPidOne, sUidOne, sLayerIdOne,
                                                       sLayerNameOne, sLayerNameOne,
                                                       /*isBuffer*/ true, sGameMode);
    surfaceFrame1->setAcquireFenceTime(45);
    mFrameTimeline->setSfWakeUp(sfToken1, 52, refreshRate, refreshRate);

    surfaceFrame1->setPresentState(SurfaceFrame::PresentState::Presented);
    surfaceFrame1->setRenderRate(renderRate);
    mFrameTimeline->addSurfaceFrame(surfaceFrame1);
    presentFence1->signalForTest(90);
    mFrameTimeline->setSfPresent(86, presentFence1);

    EXPECT_EQ(surfaceFrame1->getJankType(), JankType::AppDeadlineMissed);
    EXPECT_EQ(surfaceFrame1->getJankSeverityType(), JankSeverityType::Full);
}

TEST_F(FrameTimelineTest, presentFenceSignaled_displayFramePredictionExpiredPresentsSurfaceFrame) {
    Fps refreshRate = RR_11;
    Fps renderRate = RR_30;

    EXPECT_CALL(*mTimeStats,
                incrementJankyFrames(
                        TimeStats::JankyFramesInfo{refreshRate, renderRate, sUidOne, sLayerNameOne,
                                                   sGameMode,
                                                   JankType::Unknown | JankType::AppDeadlineMissed,
                                                   0, 0, 25}));
    auto presentFence1 = fenceFactory.createFenceTimeForTest(Fence::NO_FENCE);
    int64_t surfaceFrameToken1 = mTokenManager->generateTokenForPredictions({10, 20, 60});
    int64_t sfToken1 = mTokenManager->generateTokenForPredictions({82, 90, 90});
    FrameTimelineInfo ftInfo;
    ftInfo.vsyncId = surfaceFrameToken1;
    ftInfo.inputEventId = sInputEventId;

    auto surfaceFrame1 =
            mFrameTimeline->createSurfaceFrameForToken(ftInfo, sPidOne, sUidOne, sLayerIdOne,
                                                       sLayerNameOne, sLayerNameOne,
                                                       /*isBuffer*/ true, sGameMode);
    surfaceFrame1->setAcquireFenceTime(45);
    // Trigger a prediction expiry
    flushTokens();
    mFrameTimeline->setSfWakeUp(sfToken1, 52, refreshRate, refreshRate);

    surfaceFrame1->setPresentState(SurfaceFrame::PresentState::Presented);
    surfaceFrame1->setRenderRate(renderRate);
    mFrameTimeline->addSurfaceFrame(surfaceFrame1);
    presentFence1->signalForTest(90);
    mFrameTimeline->setSfPresent(86, presentFence1);

    auto displayFrame = getDisplayFrame(0);
    EXPECT_EQ(displayFrame->getJankType(), JankType::Unknown);
    EXPECT_EQ(displayFrame->getJankSeverityType(), JankSeverityType::Unknown);
    EXPECT_EQ(displayFrame->getFrameStartMetadata(), FrameStartMetadata::UnknownStart);
    EXPECT_EQ(displayFrame->getFrameReadyMetadata(), FrameReadyMetadata::UnknownFinish);
    EXPECT_EQ(displayFrame->getFramePresentMetadata(), FramePresentMetadata::UnknownPresent);

    EXPECT_EQ(surfaceFrame1->getActuals().presentTime, 90);
    EXPECT_EQ(surfaceFrame1->getJankType(), JankType::Unknown | JankType::AppDeadlineMissed);
    EXPECT_EQ(surfaceFrame1->getJankSeverityType(), JankSeverityType::Full);
}

/*
 * Tracing Tests
 *
 * Trace packets are flushed all the way only when the next packet is traced.
 * For example: trace<Display/Surface>Frame() will create a TracePacket but not flush it. Only when
 * another TracePacket is created, the previous one is guaranteed to be flushed. The following tests
 * will have additional empty frames created for this reason.
 */
TEST_F(FrameTimelineTest, tracing_noPacketsSentWithoutTraceStart) {
    auto tracingSession = getTracingSessionForTest();
    auto presentFence1 = fenceFactory.createFenceTimeForTest(Fence::NO_FENCE);
    int64_t token1 = mTokenManager->generateTokenForPredictions({10, 20, 30});
    FrameTimelineInfo ftInfo;
    ftInfo.vsyncId = token1;
    ftInfo.inputEventId = sInputEventId;
    auto surfaceFrame1 =
            mFrameTimeline->createSurfaceFrameForToken(ftInfo, sPidOne, sUidOne, sLayerIdOne,
                                                       sLayerNameOne, sLayerNameOne,
                                                       /*isBuffer*/ true, sGameMode);

    // Set up the display frame
    mFrameTimeline->setSfWakeUp(token1, 20, RR_11, RR_11);
    surfaceFrame1->setPresentState(SurfaceFrame::PresentState::Dropped);
    mFrameTimeline->addSurfaceFrame(surfaceFrame1);
    mFrameTimeline->setSfPresent(25, presentFence1);
    presentFence1->signalForTest(30);

    addEmptyDisplayFrame();

    auto packets = readFrameTimelinePacketsBlocking(tracingSession.get());
    EXPECT_EQ(packets.size(), 0u);
}

TEST_F(FrameTimelineTest, tracing_sanityTest) {
    auto tracingSession = getTracingSessionForTest();
    // Layer specific increment
    EXPECT_CALL(*mTimeStats, incrementJankyFrames(_));
    auto presentFence1 = fenceFactory.createFenceTimeForTest(Fence::NO_FENCE);

    tracingSession->StartBlocking();
    int64_t token1 = mTokenManager->generateTokenForPredictions({10, 20, 30});
    int64_t token2 = mTokenManager->generateTokenForPredictions({40, 50, 60});
    FrameTimelineInfo ftInfo;
    ftInfo.vsyncId = token1;
    ftInfo.inputEventId = sInputEventId;
    auto surfaceFrame1 =
            mFrameTimeline->createSurfaceFrameForToken(ftInfo, sPidOne, sUidOne, sLayerIdOne,
                                                       sLayerNameOne, sLayerNameOne,
                                                       /*isBuffer*/ true, sGameMode);

    // Set up the display frame
    mFrameTimeline->setSfWakeUp(token2, 20, RR_11, RR_11);
    surfaceFrame1->setPresentState(SurfaceFrame::PresentState::Presented);
    mFrameTimeline->addSurfaceFrame(surfaceFrame1);
    mFrameTimeline->setSfPresent(25, presentFence1);
    presentFence1->signalForTest(30);

    addEmptyDisplayFrame();
    flushTrace();
    tracingSession->StopBlocking();

    auto packets = readFrameTimelinePacketsBlocking(tracingSession.get());
    // Display Frame 1 has 8 packets - 4 from DisplayFrame and 4 from SurfaceFrame.
    EXPECT_EQ(packets.size(), 8u);
}

TEST_F(FrameTimelineTest, traceDisplayFrame_invalidTokenDoesNotEmitTracePacket) {
    auto tracingSession = getTracingSessionForTest();
    auto presentFence1 = fenceFactory.createFenceTimeForTest(Fence::NO_FENCE);

    tracingSession->StartBlocking();

    // Set up the display frame
    mFrameTimeline->setSfWakeUp(-1, 20, RR_11, RR_11);
    mFrameTimeline->setSfPresent(25, presentFence1);
    presentFence1->signalForTest(30);

    addEmptyDisplayFrame();
    flushTrace();
    tracingSession->StopBlocking();

    auto packets = readFrameTimelinePacketsBlocking(tracingSession.get());
    EXPECT_EQ(packets.size(), 0u);
}

TEST_F(FrameTimelineTest, traceSurfaceFrame_invalidTokenDoesNotEmitTracePacket) {
    auto tracingSession = getTracingSessionForTest();
    auto presentFence1 = fenceFactory.createFenceTimeForTest(Fence::NO_FENCE);

    tracingSession->StartBlocking();
    int64_t token1 = mTokenManager->generateTokenForPredictions({10, 20, 30});
    auto surfaceFrame1 =
            mFrameTimeline->createSurfaceFrameForToken({}, sPidOne, sUidOne, sLayerIdOne,
                                                       sLayerNameOne, sLayerNameOne,
                                                       /*isBuffer*/ true, sGameMode);

    // Set up the display frame
    mFrameTimeline->setSfWakeUp(token1, 20, RR_11, RR_11);
    surfaceFrame1->setPresentState(SurfaceFrame::PresentState::Dropped);
    mFrameTimeline->addSurfaceFrame(surfaceFrame1);
    mFrameTimeline->setSfPresent(25, presentFence1);
    presentFence1->signalForTest(30);

    addEmptyDisplayFrame();
    flushTrace();
    tracingSession->StopBlocking();

    auto packets = readFrameTimelinePacketsBlocking(tracingSession.get());
    // Display Frame 1 has 4 packets (SurfaceFrame shouldn't be traced since it has an invalid
    // token).
    EXPECT_EQ(packets.size(), 4u);
}

ProtoExpectedDisplayFrameStart createProtoExpectedDisplayFrameStart(int64_t cookie, int64_t token,
                                                                    pid_t pid) {
    ProtoExpectedDisplayFrameStart proto;
    proto.set_cookie(cookie);
    proto.set_token(token);
    proto.set_pid(pid);
    return proto;
}

ProtoActualDisplayFrameStart createProtoActualDisplayFrameStart(
        int64_t cookie, int64_t token, pid_t pid, ProtoPresentType presentType, bool onTimeFinish,
        bool gpuComposition, ProtoJankType jankType, ProtoJankSeverityType jankSeverityType,
        ProtoPredictionType predictionType) {
    ProtoActualDisplayFrameStart proto;
    proto.set_cookie(cookie);
    proto.set_token(token);
    proto.set_pid(pid);
    proto.set_present_type(presentType);
    proto.set_on_time_finish(onTimeFinish);
    proto.set_gpu_composition(gpuComposition);
    proto.set_jank_type(jankType);
    proto.set_jank_severity_type(jankSeverityType);
    proto.set_prediction_type(predictionType);
    return proto;
}

ProtoExpectedSurfaceFrameStart createProtoExpectedSurfaceFrameStart(int64_t cookie, int64_t token,
                                                                    int64_t displayFrameToken,
                                                                    pid_t pid,
                                                                    std::string layerName) {
    ProtoExpectedSurfaceFrameStart proto;
    proto.set_cookie(cookie);
    proto.set_token(token);
    proto.set_display_frame_token(displayFrameToken);
    proto.set_pid(pid);
    proto.set_layer_name(layerName);
    return proto;
}

ProtoActualSurfaceFrameStart createProtoActualSurfaceFrameStart(
        int64_t cookie, int64_t token, int64_t displayFrameToken, pid_t pid, std::string layerName,
        ProtoPresentType presentType, bool onTimeFinish, bool gpuComposition,
        ProtoJankType jankType, ProtoJankSeverityType jankSeverityType,
        ProtoPredictionType predictionType, bool isBuffer) {
    ProtoActualSurfaceFrameStart proto;
    proto.set_cookie(cookie);
    proto.set_token(token);
    proto.set_display_frame_token(displayFrameToken);
    proto.set_pid(pid);
    proto.set_layer_name(layerName);
    proto.set_present_type(presentType);
    proto.set_on_time_finish(onTimeFinish);
    proto.set_gpu_composition(gpuComposition);
    proto.set_jank_type(jankType);
    proto.set_jank_severity_type(jankSeverityType);
    proto.set_prediction_type(predictionType);
    proto.set_is_buffer(isBuffer);
    return proto;
}

ProtoFrameEnd createProtoFrameEnd(int64_t cookie) {
    ProtoFrameEnd proto;
    proto.set_cookie(cookie);
    return proto;
}

void validateTraceEvent(const ProtoExpectedDisplayFrameStart& received,
                        const ProtoExpectedDisplayFrameStart& source) {
    ASSERT_TRUE(received.has_cookie());
    EXPECT_EQ(received.cookie(), source.cookie());

    ASSERT_TRUE(received.has_token());
    EXPECT_EQ(received.token(), source.token());

    ASSERT_TRUE(received.has_pid());
    EXPECT_EQ(received.pid(), source.pid());
}

void validateTraceEvent(const ProtoActualDisplayFrameStart& received,
                        const ProtoActualDisplayFrameStart& source) {
    ASSERT_TRUE(received.has_cookie());
    EXPECT_EQ(received.cookie(), source.cookie());

    ASSERT_TRUE(received.has_token());
    EXPECT_EQ(received.token(), source.token());

    ASSERT_TRUE(received.has_pid());
    EXPECT_EQ(received.pid(), source.pid());

    ASSERT_TRUE(received.has_present_type());
    EXPECT_EQ(received.present_type(), source.present_type());
    ASSERT_TRUE(received.has_on_time_finish());
    EXPECT_EQ(received.on_time_finish(), source.on_time_finish());
    ASSERT_TRUE(received.has_gpu_composition());
    EXPECT_EQ(received.gpu_composition(), source.gpu_composition());
    ASSERT_TRUE(received.has_jank_type());
    EXPECT_EQ(received.jank_type(), source.jank_type());
    ASSERT_TRUE(received.has_jank_severity_type());
    EXPECT_EQ(received.jank_severity_type(), source.jank_severity_type());
    ASSERT_TRUE(received.has_prediction_type());
    EXPECT_EQ(received.prediction_type(), source.prediction_type());
}

void validateTraceEvent(const ProtoExpectedSurfaceFrameStart& received,
                        const ProtoExpectedSurfaceFrameStart& source) {
    ASSERT_TRUE(received.has_cookie());
    EXPECT_EQ(received.cookie(), source.cookie());

    ASSERT_TRUE(received.has_token());
    EXPECT_EQ(received.token(), source.token());

    ASSERT_TRUE(received.has_display_frame_token());
    EXPECT_EQ(received.display_frame_token(), source.display_frame_token());

    ASSERT_TRUE(received.has_pid());
    EXPECT_EQ(received.pid(), source.pid());

    ASSERT_TRUE(received.has_layer_name());
    EXPECT_EQ(received.layer_name(), source.layer_name());
}

void validateTraceEvent(const ProtoActualSurfaceFrameStart& received,
                        const ProtoActualSurfaceFrameStart& source) {
    ASSERT_TRUE(received.has_cookie());
    EXPECT_EQ(received.cookie(), source.cookie());

    ASSERT_TRUE(received.has_token());
    EXPECT_EQ(received.token(), source.token());

    ASSERT_TRUE(received.has_display_frame_token());
    EXPECT_EQ(received.display_frame_token(), source.display_frame_token());

    ASSERT_TRUE(received.has_pid());
    EXPECT_EQ(received.pid(), source.pid());

    ASSERT_TRUE(received.has_layer_name());
    EXPECT_EQ(received.layer_name(), source.layer_name());

    ASSERT_TRUE(received.has_present_type());
    EXPECT_EQ(received.present_type(), source.present_type());
    ASSERT_TRUE(received.has_on_time_finish());
    EXPECT_EQ(received.on_time_finish(), source.on_time_finish());
    ASSERT_TRUE(received.has_gpu_composition());
    EXPECT_EQ(received.gpu_composition(), source.gpu_composition());
    ASSERT_TRUE(received.has_jank_type());
    EXPECT_EQ(received.jank_type(), source.jank_type());
    ASSERT_TRUE(received.has_jank_severity_type());
    EXPECT_EQ(received.jank_severity_type(), source.jank_severity_type());
    ASSERT_TRUE(received.has_prediction_type());
    EXPECT_EQ(received.prediction_type(), source.prediction_type());
    ASSERT_TRUE(received.has_is_buffer());
    EXPECT_EQ(received.is_buffer(), source.is_buffer());
}

void validateTraceEvent(const ProtoFrameEnd& received, const ProtoFrameEnd& source) {
    ASSERT_TRUE(received.has_cookie());
    EXPECT_EQ(received.cookie(), source.cookie());
}

TEST_F(FrameTimelineTest, traceDisplayFrameSkipped) {
    SET_FLAG_FOR_TEST(com::android::graphics::surfaceflinger::flags::add_sf_skipped_frames_to_trace,
                      true);

    // setup 2 display frames
    // DF 1: [22,40] -> [5, 40]
    // DF  : [36, 70] (Skipped one, added by the trace)
    // DF 2: [82, 100] -> SF [25, 70]
    auto tracingSession = getTracingSessionForTest();
    tracingSession->StartBlocking();
    int64_t sfToken1 = mTokenManager->generateTokenForPredictions({22, 30, 40});
    int64_t sfToken2 = mTokenManager->generateTokenForPredictions({82, 90, 100});
    int64_t surfaceFrameToken1 = mTokenManager->generateTokenForPredictions({5, 16, 40});
    int64_t surfaceFrameToken2 = mTokenManager->generateTokenForPredictions({25, 36, 70});
    auto presentFence1 = fenceFactory.createFenceTimeForTest(Fence::NO_FENCE);

    int64_t traceCookie = snoopCurrentTraceCookie();

    // set up 1st display frame
    FrameTimelineInfo ftInfo1;
    ftInfo1.vsyncId = surfaceFrameToken1;
    ftInfo1.inputEventId = sInputEventId;
    auto surfaceFrame1 =
            mFrameTimeline->createSurfaceFrameForToken(ftInfo1, sPidOne, sUidOne, sLayerIdOne,
                                                       sLayerNameOne, sLayerNameOne,
                                                       /*isBuffer*/ true, sGameMode);
    surfaceFrame1->setAcquireFenceTime(16);
    mFrameTimeline->setSfWakeUp(sfToken1, 22, RR_11, RR_30);
    surfaceFrame1->setPresentState(SurfaceFrame::PresentState::Presented);
    mFrameTimeline->addSurfaceFrame(surfaceFrame1);
    mFrameTimeline->setSfPresent(30, presentFence1);
    presentFence1->signalForTest(40);

    // Trigger a flush by finalizing the next DisplayFrame
    auto presentFence2 = fenceFactory.createFenceTimeForTest(Fence::NO_FENCE);
    FrameTimelineInfo ftInfo2;
    ftInfo2.vsyncId = surfaceFrameToken2;
    ftInfo2.inputEventId = sInputEventId;
    auto surfaceFrame2 =
            mFrameTimeline->createSurfaceFrameForToken(ftInfo2, sPidOne, sUidOne, sLayerIdOne,
                                                       sLayerNameOne, sLayerNameOne,
                                                       /*isBuffer*/ true, sGameMode);

    // set up 2nd display frame
    surfaceFrame2->setAcquireFenceTime(36);
    mFrameTimeline->setSfWakeUp(sfToken2, 82, RR_11, RR_30);
    surfaceFrame2->setPresentState(SurfaceFrame::PresentState::Presented);
    mFrameTimeline->addSurfaceFrame(surfaceFrame2);
    mFrameTimeline->setSfPresent(90, presentFence2);
    presentFence2->signalForTest(100);

    // the token of skipped Display Frame
    auto protoSkippedActualDisplayFrameStart =
            createProtoActualDisplayFrameStart(traceCookie + 9, 0, kSurfaceFlingerPid,
                                               FrameTimelineEvent::PRESENT_DROPPED, true, false,
                                               FrameTimelineEvent::JANK_DROPPED,
                                               FrameTimelineEvent::SEVERITY_NONE,
                                               FrameTimelineEvent::PREDICTION_VALID);
    auto protoSkippedActualDisplayFrameEnd = createProtoFrameEnd(traceCookie + 9);

    // Trigger a flush by finalizing the next DisplayFrame
    addEmptyDisplayFrame();
    flushTrace();
    tracingSession->StopBlocking();

    auto packets = readFrameTimelinePacketsBlocking(tracingSession.get());
    // 8 Valid Display Frames + 8 Valid Surface Frames + 2 Skipped Display Frames
    EXPECT_EQ(packets.size(), 18u);

    // Packet - 16: Actual skipped Display Frame Start
    // the timestamp should be equal to the 2nd expected surface frame's end time
    const auto& packet16 = packets[16];
    ASSERT_TRUE(packet16.has_timestamp());
    EXPECT_EQ(packet16.timestamp(), 36u);
    ASSERT_TRUE(packet16.has_frame_timeline_event());

    const auto& event16 = packet16.frame_timeline_event();
    const auto& actualSkippedDisplayFrameStart = event16.actual_display_frame_start();
    validateTraceEvent(actualSkippedDisplayFrameStart, protoSkippedActualDisplayFrameStart);

    // Packet - 17: Actual skipped Display Frame End
    // the timestamp should be equal to the 2nd expected surface frame's present time
    const auto& packet17 = packets[17];
    ASSERT_TRUE(packet17.has_timestamp());
    EXPECT_EQ(packet17.timestamp(), 70u);
    ASSERT_TRUE(packet17.has_frame_timeline_event());

    const auto& event17 = packet17.frame_timeline_event();
    const auto& actualSkippedDisplayFrameEnd = event17.frame_end();
    validateTraceEvent(actualSkippedDisplayFrameEnd, protoSkippedActualDisplayFrameEnd);
}

TEST_F(FrameTimelineTest, traceDisplayFrame_emitsValidTracePacket) {
    auto tracingSession = getTracingSessionForTest();
    auto presentFence1 = fenceFactory.createFenceTimeForTest(Fence::NO_FENCE);

    tracingSession->StartBlocking();

    // Add an empty surface frame so that display frame would get traced.
    addEmptySurfaceFrame();
    int64_t displayFrameToken1 = mTokenManager->generateTokenForPredictions({10, 30, 30});

    // Set up the display frame
    mFrameTimeline->setSfWakeUp(displayFrameToken1, 20, RR_11, RR_11);
    mFrameTimeline->setSfPresent(26, presentFence1);
    presentFence1->signalForTest(31);

    int64_t traceCookie = snoopCurrentTraceCookie();
    auto protoExpectedDisplayFrameStart =
            createProtoExpectedDisplayFrameStart(traceCookie + 1, displayFrameToken1,
                                                 kSurfaceFlingerPid);
    auto protoExpectedDisplayFrameEnd = createProtoFrameEnd(traceCookie + 1);
    auto protoActualDisplayFrameStart =
            createProtoActualDisplayFrameStart(traceCookie + 2, displayFrameToken1,
                                               kSurfaceFlingerPid,
                                               FrameTimelineEvent::PRESENT_ON_TIME, true, false,
                                               FrameTimelineEvent::JANK_NONE,
                                               FrameTimelineEvent::SEVERITY_NONE,
                                               FrameTimelineEvent::PREDICTION_VALID);
    auto protoActualDisplayFrameEnd = createProtoFrameEnd(traceCookie + 2);

    addEmptyDisplayFrame();
    flushTrace();
    tracingSession->StopBlocking();

    auto packets = readFrameTimelinePacketsBlocking(tracingSession.get());
    EXPECT_EQ(packets.size(), 4u);

    // Packet - 0 : ExpectedDisplayFrameStart
    const auto& packet0 = packets[0];
    ASSERT_TRUE(packet0.has_timestamp());
    EXPECT_EQ(packet0.timestamp(), 10u);
    ASSERT_TRUE(packet0.has_frame_timeline_event());

    const auto& event0 = packet0.frame_timeline_event();
    ASSERT_TRUE(event0.has_expected_display_frame_start());
    const auto& expectedDisplayFrameStart = event0.expected_display_frame_start();
    validateTraceEvent(expectedDisplayFrameStart, protoExpectedDisplayFrameStart);

    // Packet - 1 : FrameEnd (ExpectedDisplayFrame)
    const auto& packet1 = packets[1];
    ASSERT_TRUE(packet1.has_timestamp());
    EXPECT_EQ(packet1.timestamp(), 30u);
    ASSERT_TRUE(packet1.has_frame_timeline_event());

    const auto& event1 = packet1.frame_timeline_event();
    ASSERT_TRUE(event1.has_frame_end());
    const auto& expectedDisplayFrameEnd = event1.frame_end();
    validateTraceEvent(expectedDisplayFrameEnd, protoExpectedDisplayFrameEnd);

    // Packet - 2 : ActualDisplayFrameStart
    const auto& packet2 = packets[2];
    ASSERT_TRUE(packet2.has_timestamp());
    EXPECT_EQ(packet2.timestamp(), 20u);
    ASSERT_TRUE(packet2.has_frame_timeline_event());

    const auto& event2 = packet2.frame_timeline_event();
    ASSERT_TRUE(event2.has_actual_display_frame_start());
    const auto& actualDisplayFrameStart = event2.actual_display_frame_start();
    validateTraceEvent(actualDisplayFrameStart, protoActualDisplayFrameStart);

    // Packet - 3 : FrameEnd (ActualDisplayFrame)
    const auto& packet3 = packets[3];
    ASSERT_TRUE(packet3.has_timestamp());
    EXPECT_EQ(packet3.timestamp(), 31u);
    ASSERT_TRUE(packet3.has_frame_timeline_event());

    const auto& event3 = packet3.frame_timeline_event();
    ASSERT_TRUE(event3.has_frame_end());
    const auto& actualDisplayFrameEnd = event3.frame_end();
    validateTraceEvent(actualDisplayFrameEnd, protoActualDisplayFrameEnd);
}

TEST_F(FrameTimelineTest, traceDisplayFrame_predictionExpiredDoesNotTraceExpectedTimeline) {
    auto tracingSession = getTracingSessionForTest();
    auto presentFence1 = fenceFactory.createFenceTimeForTest(Fence::NO_FENCE);

    tracingSession->StartBlocking();
    int64_t displayFrameToken1 = mTokenManager->generateTokenForPredictions({10, 25, 30});
    // Flush the token so that it would expire
    flushTokens();

    // Add an empty surface frame so that display frame would get traced.
    addEmptySurfaceFrame();

    // Set up the display frame
    mFrameTimeline->setSfWakeUp(displayFrameToken1, 20, RR_11, RR_11);
    mFrameTimeline->setSfPresent(26, presentFence1);
    presentFence1->signalForTest(31);

    int64_t traceCookie = snoopCurrentTraceCookie();

    auto protoActualDisplayFrameStart =
            createProtoActualDisplayFrameStart(traceCookie + 1, displayFrameToken1,
                                               kSurfaceFlingerPid,
                                               FrameTimelineEvent::PRESENT_UNSPECIFIED, false,
                                               false, FrameTimelineEvent::JANK_UNKNOWN,
                                               FrameTimelineEvent::SEVERITY_UNKNOWN,
                                               FrameTimelineEvent::PREDICTION_EXPIRED);
    auto protoActualDisplayFrameEnd = createProtoFrameEnd(traceCookie + 1);

    addEmptyDisplayFrame();
    flushTrace();
    tracingSession->StopBlocking();

    auto packets = readFrameTimelinePacketsBlocking(tracingSession.get());
    // Only actual timeline packets should be in the trace
    EXPECT_EQ(packets.size(), 2u);

    // Packet - 0 : ActualDisplayFrameStart
    const auto& packet0 = packets[0];
    ASSERT_TRUE(packet0.has_timestamp());
    EXPECT_EQ(packet0.timestamp(), 20u);
    ASSERT_TRUE(packet0.has_frame_timeline_event());

    const auto& event0 = packet0.frame_timeline_event();
    ASSERT_TRUE(event0.has_actual_display_frame_start());
    const auto& actualDisplayFrameStart = event0.actual_display_frame_start();
    validateTraceEvent(actualDisplayFrameStart, protoActualDisplayFrameStart);

    // Packet - 1 : FrameEnd (ActualDisplayFrame)
    const auto& packet1 = packets[1];
    ASSERT_TRUE(packet1.has_timestamp());
    EXPECT_EQ(packet1.timestamp(), 31u);
    ASSERT_TRUE(packet1.has_frame_timeline_event());

    const auto& event1 = packet1.frame_timeline_event();
    ASSERT_TRUE(event1.has_frame_end());
    const auto& actualDisplayFrameEnd = event1.frame_end();
    validateTraceEvent(actualDisplayFrameEnd, protoActualDisplayFrameEnd);
}

TEST_F(FrameTimelineTest, traceSurfaceFrame_emitsValidTracePacket) {
    auto tracingSession = getTracingSessionForTest();
    // Layer specific increment
    EXPECT_CALL(*mTimeStats, incrementJankyFrames(_)).Times(2);
    auto presentFence1 = fenceFactory.createFenceTimeForTest(Fence::NO_FENCE);
    auto presentFence2 = fenceFactory.createFenceTimeForTest(Fence::NO_FENCE);

    tracingSession->StartBlocking();
    int64_t surfaceFrameToken = mTokenManager->generateTokenForPredictions({10, 25, 40});
    int64_t displayFrameToken1 = mTokenManager->generateTokenForPredictions({30, 35, 40});

    FrameTimelineInfo ftInfo;
    ftInfo.vsyncId = surfaceFrameToken;
    ftInfo.inputEventId = sInputEventId;

    auto surfaceFrame1 =
            mFrameTimeline->createSurfaceFrameForToken(ftInfo, sPidOne, sUidOne, sLayerIdOne,
                                                       sLayerNameOne, sLayerNameOne,
                                                       /*isBuffer*/ true, sGameMode);
    auto surfaceFrame2 =
            mFrameTimeline->createSurfaceFrameForToken(ftInfo, sPidOne, sUidOne, sLayerIdOne,
                                                       sLayerNameOne, sLayerNameOne,
                                                       /*isBuffer*/ true, sGameMode);
    surfaceFrame1->setActualQueueTime(10);
    surfaceFrame1->setDropTime(15);

    surfaceFrame2->setActualQueueTime(15);
    surfaceFrame2->setAcquireFenceTime(20);

    // First 2 cookies will be used by the DisplayFrame
    int64_t traceCookie = snoopCurrentTraceCookie() + 2;

    auto protoDroppedSurfaceFrameExpectedStart =
            createProtoExpectedSurfaceFrameStart(traceCookie + 1, surfaceFrameToken,
                                                 displayFrameToken1, sPidOne, sLayerNameOne);
    auto protoDroppedSurfaceFrameExpectedEnd = createProtoFrameEnd(traceCookie + 1);
    auto protoDroppedSurfaceFrameActualStart =
            createProtoActualSurfaceFrameStart(traceCookie + 2, surfaceFrameToken,
                                               displayFrameToken1, sPidOne, sLayerNameOne,
                                               FrameTimelineEvent::PRESENT_DROPPED, true, false,
                                               FrameTimelineEvent::JANK_DROPPED,
                                               FrameTimelineEvent::SEVERITY_UNKNOWN,
                                               FrameTimelineEvent::PREDICTION_VALID, true);
    auto protoDroppedSurfaceFrameActualEnd = createProtoFrameEnd(traceCookie + 2);

    auto protoPresentedSurfaceFrameExpectedStart =
            createProtoExpectedSurfaceFrameStart(traceCookie + 3, surfaceFrameToken,
                                                 displayFrameToken1, sPidOne, sLayerNameOne);
    auto protoPresentedSurfaceFrameExpectedEnd = createProtoFrameEnd(traceCookie + 3);
    auto protoPresentedSurfaceFrameActualStart =
            createProtoActualSurfaceFrameStart(traceCookie + 4, surfaceFrameToken,
                                               displayFrameToken1, sPidOne, sLayerNameOne,
                                               FrameTimelineEvent::PRESENT_ON_TIME, true, false,
                                               FrameTimelineEvent::JANK_NONE,
                                               FrameTimelineEvent::SEVERITY_NONE,
                                               FrameTimelineEvent::PREDICTION_VALID, true);
    auto protoPresentedSurfaceFrameActualEnd = createProtoFrameEnd(traceCookie + 4);

    // Set up the display frame
    mFrameTimeline->setSfWakeUp(displayFrameToken1, 20, RR_11, RR_11);
    surfaceFrame1->setPresentState(SurfaceFrame::PresentState::Dropped);
    surfaceFrame2->setPresentState(SurfaceFrame::PresentState::Presented);
    mFrameTimeline->addSurfaceFrame(surfaceFrame1);
    mFrameTimeline->addSurfaceFrame(surfaceFrame2);
    mFrameTimeline->setSfPresent(26, presentFence1);
    presentFence1->signalForTest(40);

    addEmptyDisplayFrame();
    flushTrace();
    tracingSession->StopBlocking();

    auto packets = readFrameTimelinePacketsBlocking(tracingSession.get());
    // 4 DisplayFrame + 4 DroppedSurfaceFrame + 4 PresentedSurfaceFrame
    EXPECT_EQ(packets.size(), 12u);

    // Packet - 4 : ExpectedSurfaceFrameStart1
    const auto& packet4 = packets[4];
    ASSERT_TRUE(packet4.has_timestamp());
    EXPECT_EQ(packet4.timestamp(), 10u);
    ASSERT_TRUE(packet4.has_frame_timeline_event());

    const auto& event4 = packet4.frame_timeline_event();
    ASSERT_TRUE(event4.has_expected_surface_frame_start());
    const auto& expectedSurfaceFrameStart1 = event4.expected_surface_frame_start();
    validateTraceEvent(expectedSurfaceFrameStart1, protoDroppedSurfaceFrameExpectedStart);

    // Packet - 5 : FrameEnd (ExpectedSurfaceFrame1)
    const auto& packet5 = packets[5];
    ASSERT_TRUE(packet5.has_timestamp());
    EXPECT_EQ(packet5.timestamp(), 25u);
    ASSERT_TRUE(packet5.has_frame_timeline_event());

    const auto& event5 = packet5.frame_timeline_event();
    ASSERT_TRUE(event5.has_frame_end());
    const auto& expectedSurfaceFrameEnd1 = event5.frame_end();
    validateTraceEvent(expectedSurfaceFrameEnd1, protoDroppedSurfaceFrameExpectedEnd);

    // Packet - 6 : ActualSurfaceFrameStart1
    const auto& packet6 = packets[6];
    ASSERT_TRUE(packet6.has_timestamp());
    EXPECT_EQ(packet6.timestamp(), 10u);
    ASSERT_TRUE(packet6.has_frame_timeline_event());

    const auto& event6 = packet6.frame_timeline_event();
    ASSERT_TRUE(event6.has_actual_surface_frame_start());
    const auto& actualSurfaceFrameStart1 = event6.actual_surface_frame_start();
    validateTraceEvent(actualSurfaceFrameStart1, protoDroppedSurfaceFrameActualStart);

    // Packet - 7 : FrameEnd (ActualSurfaceFrame1)
    const auto& packet7 = packets[7];
    ASSERT_TRUE(packet7.has_timestamp());
    EXPECT_EQ(packet7.timestamp(), 15u);
    ASSERT_TRUE(packet7.has_frame_timeline_event());

    const auto& event7 = packet7.frame_timeline_event();
    ASSERT_TRUE(event7.has_frame_end());
    const auto& actualSurfaceFrameEnd1 = event7.frame_end();
    validateTraceEvent(actualSurfaceFrameEnd1, protoDroppedSurfaceFrameActualEnd);

    // Packet - 8 : ExpectedSurfaceFrameStart2
    const auto& packet8 = packets[8];
    ASSERT_TRUE(packet8.has_timestamp());
    EXPECT_EQ(packet8.timestamp(), 10u);
    ASSERT_TRUE(packet8.has_frame_timeline_event());

    const auto& event8 = packet8.frame_timeline_event();
    ASSERT_TRUE(event8.has_expected_surface_frame_start());
    const auto& expectedSurfaceFrameStart2 = event8.expected_surface_frame_start();
    validateTraceEvent(expectedSurfaceFrameStart2, protoPresentedSurfaceFrameExpectedStart);

    // Packet - 9 : FrameEnd (ExpectedSurfaceFrame2)
    const auto& packet9 = packets[9];
    ASSERT_TRUE(packet9.has_timestamp());
    EXPECT_EQ(packet9.timestamp(), 25u);
    ASSERT_TRUE(packet9.has_frame_timeline_event());

    const auto& event9 = packet9.frame_timeline_event();
    ASSERT_TRUE(event9.has_frame_end());
    const auto& expectedSurfaceFrameEnd2 = event9.frame_end();
    validateTraceEvent(expectedSurfaceFrameEnd2, protoPresentedSurfaceFrameExpectedEnd);

    // Packet - 10 : ActualSurfaceFrameStart2
    const auto& packet10 = packets[10];
    ASSERT_TRUE(packet10.has_timestamp());
    EXPECT_EQ(packet10.timestamp(), 10u);
    ASSERT_TRUE(packet10.has_frame_timeline_event());

    const auto& event10 = packet10.frame_timeline_event();
    ASSERT_TRUE(event10.has_actual_surface_frame_start());
    const auto& actualSurfaceFrameStart2 = event10.actual_surface_frame_start();
    validateTraceEvent(actualSurfaceFrameStart2, protoPresentedSurfaceFrameActualStart);

    // Packet - 11 : FrameEnd (ActualSurfaceFrame2)
    const auto& packet11 = packets[11];
    ASSERT_TRUE(packet11.has_timestamp());
    EXPECT_EQ(packet11.timestamp(), 20u);
    ASSERT_TRUE(packet11.has_frame_timeline_event());

    const auto& event11 = packet11.frame_timeline_event();
    ASSERT_TRUE(event11.has_frame_end());
    const auto& actualSurfaceFrameEnd2 = event11.frame_end();
    validateTraceEvent(actualSurfaceFrameEnd2, protoPresentedSurfaceFrameActualEnd);
}

TEST_F(FrameTimelineTest, traceSurfaceFrame_predictionExpiredIsAppMissedDeadline) {
    auto tracingSession = getTracingSessionForTest();
    auto presentFence1 = fenceFactory.createFenceTimeForTest(Fence::NO_FENCE);

    tracingSession->StartBlocking();
    constexpr nsecs_t appStartTime = std::chrono::nanoseconds(10ms).count();
    constexpr nsecs_t appEndTime = std::chrono::nanoseconds(20ms).count();
    constexpr nsecs_t appPresentTime = std::chrono::nanoseconds(30ms).count();
    int64_t surfaceFrameToken =
            mTokenManager->generateTokenForPredictions({appStartTime, appEndTime, appPresentTime});

    // Flush the token so that it would expire
    flushTokens();
    FrameTimelineInfo ftInfo;
    ftInfo.vsyncId = surfaceFrameToken;
    ftInfo.inputEventId = 0;
    auto surfaceFrame1 =
            mFrameTimeline->createSurfaceFrameForToken(ftInfo, sPidOne, sUidOne, sLayerIdOne,
                                                       sLayerNameOne, sLayerNameOne,
                                                       /*isBuffer*/ true, sGameMode);
    surfaceFrame1->setActualQueueTime(appEndTime);
    surfaceFrame1->setAcquireFenceTime(appEndTime);

    constexpr nsecs_t sfStartTime = std::chrono::nanoseconds(20ms).count();
    constexpr nsecs_t sfEndTime = std::chrono::nanoseconds(30ms).count();
    constexpr nsecs_t sfPresentTime = std::chrono::nanoseconds(30ms).count();
    int64_t displayFrameToken =
            mTokenManager->generateTokenForPredictions({sfStartTime, sfEndTime, sfPresentTime});

    // First 2 cookies will be used by the DisplayFrame
    int64_t traceCookie = snoopCurrentTraceCookie() + 2;

    auto protoActualSurfaceFrameStart =
            createProtoActualSurfaceFrameStart(traceCookie + 1, surfaceFrameToken,
                                               displayFrameToken, sPidOne, sLayerNameOne,
                                               FrameTimelineEvent::PRESENT_UNSPECIFIED, false,
                                               false, FrameTimelineEvent::JANK_APP_DEADLINE_MISSED,
                                               FrameTimelineEvent::SEVERITY_UNKNOWN,
                                               FrameTimelineEvent::PREDICTION_EXPIRED, true);
    auto protoActualSurfaceFrameEnd = createProtoFrameEnd(traceCookie + 1);

    // Set up the display frame
    mFrameTimeline->setSfWakeUp(displayFrameToken, sfStartTime, RR_11, RR_11);
    surfaceFrame1->setPresentState(SurfaceFrame::PresentState::Presented);
    mFrameTimeline->addSurfaceFrame(surfaceFrame1);
    mFrameTimeline->setSfPresent(sfEndTime, presentFence1);
    presentFence1->signalForTest(sfPresentTime);

    addEmptyDisplayFrame();
    flushTrace();
    tracingSession->StopBlocking();

    auto packets = readFrameTimelinePacketsBlocking(tracingSession.get());
    // Display Frame 4 packets + SurfaceFrame 2 packets
    ASSERT_EQ(packets.size(), 6u);

    // Packet - 4 : ActualSurfaceFrameStart
    const auto& packet4 = packets[4];
    ASSERT_TRUE(packet4.has_timestamp());
    EXPECT_EQ(packet4.timestamp(),
              static_cast<uint64_t>(appEndTime - SurfaceFrame::kPredictionExpiredStartTimeDelta));
    ASSERT_TRUE(packet4.has_frame_timeline_event());

    const auto& event4 = packet4.frame_timeline_event();
    ASSERT_TRUE(event4.has_actual_surface_frame_start());
    const auto& actualSurfaceFrameStart = event4.actual_surface_frame_start();
    validateTraceEvent(actualSurfaceFrameStart, protoActualSurfaceFrameStart);

    // Packet - 5 : FrameEnd (ActualSurfaceFrame)
    const auto& packet5 = packets[5];
    ASSERT_TRUE(packet5.has_timestamp());
    EXPECT_EQ(packet5.timestamp(), static_cast<uint64_t>(appEndTime));
    ASSERT_TRUE(packet5.has_frame_timeline_event());

    const auto& event5 = packet5.frame_timeline_event();
    ASSERT_TRUE(event5.has_frame_end());
    const auto& actualSurfaceFrameEnd = event5.frame_end();
    validateTraceEvent(actualSurfaceFrameEnd, protoActualSurfaceFrameEnd);
}

TEST_F(FrameTimelineTest, traceSurfaceFrame_predictionExpiredDroppedFramesTracedProperly) {
    auto tracingSession = getTracingSessionForTest();
    auto presentFence1 = fenceFactory.createFenceTimeForTest(Fence::NO_FENCE);

    tracingSession->StartBlocking();
    constexpr nsecs_t appStartTime = std::chrono::nanoseconds(10ms).count();
    constexpr nsecs_t appEndTime = std::chrono::nanoseconds(20ms).count();
    constexpr nsecs_t appPresentTime = std::chrono::nanoseconds(30ms).count();
    int64_t surfaceFrameToken =
            mTokenManager->generateTokenForPredictions({appStartTime, appEndTime, appPresentTime});

    // Flush the token so that it would expire
    flushTokens();
    FrameTimelineInfo ftInfo;
    ftInfo.vsyncId = surfaceFrameToken;
    ftInfo.inputEventId = 0;
    auto surfaceFrame1 =
            mFrameTimeline->createSurfaceFrameForToken(ftInfo, sPidOne, sUidOne, sLayerIdOne,
                                                       sLayerNameOne, sLayerNameOne,
                                                       /*isBuffer*/ true, sGameMode);

    constexpr nsecs_t sfStartTime = std::chrono::nanoseconds(22ms).count();
    constexpr nsecs_t sfEndTime = std::chrono::nanoseconds(30ms).count();
    constexpr nsecs_t sfPresentTime = std::chrono::nanoseconds(30ms).count();
    int64_t displayFrameToken =
            mTokenManager->generateTokenForPredictions({sfStartTime, sfEndTime, sfPresentTime});

    // First 2 cookies will be used by the DisplayFrame
    int64_t traceCookie = snoopCurrentTraceCookie() + 2;

    auto protoActualSurfaceFrameStart =
            createProtoActualSurfaceFrameStart(traceCookie + 1, surfaceFrameToken,
                                               displayFrameToken, sPidOne, sLayerNameOne,
                                               FrameTimelineEvent::PRESENT_DROPPED, false, false,
                                               FrameTimelineEvent::JANK_DROPPED,
                                               FrameTimelineEvent::SEVERITY_UNKNOWN,
                                               FrameTimelineEvent::PREDICTION_EXPIRED, true);
    auto protoActualSurfaceFrameEnd = createProtoFrameEnd(traceCookie + 1);

    // Set up the display frame
    mFrameTimeline->setSfWakeUp(displayFrameToken, sfStartTime, RR_11, RR_11);
    surfaceFrame1->setDropTime(sfStartTime);
    surfaceFrame1->setPresentState(SurfaceFrame::PresentState::Dropped);
    mFrameTimeline->addSurfaceFrame(surfaceFrame1);
    mFrameTimeline->setSfPresent(sfEndTime, presentFence1);
    presentFence1->signalForTest(sfPresentTime);

    addEmptyDisplayFrame();
    flushTrace();
    tracingSession->StopBlocking();

    auto packets = readFrameTimelinePacketsBlocking(tracingSession.get());
    // Display Frame 4 packets + SurfaceFrame 2 packets
    ASSERT_EQ(packets.size(), 6u);

    // Packet - 4 : ActualSurfaceFrameStart
    const auto& packet4 = packets[4];
    ASSERT_TRUE(packet4.has_timestamp());
    EXPECT_EQ(packet4.timestamp(),
              static_cast<uint64_t>(sfStartTime - SurfaceFrame::kPredictionExpiredStartTimeDelta));
    ASSERT_TRUE(packet4.has_frame_timeline_event());

    const auto& event4 = packet4.frame_timeline_event();
    ASSERT_TRUE(event4.has_actual_surface_frame_start());
    const auto& actualSurfaceFrameStart = event4.actual_surface_frame_start();
    validateTraceEvent(actualSurfaceFrameStart, protoActualSurfaceFrameStart);

    // Packet - 5 : FrameEnd (ActualSurfaceFrame)
    const auto& packet5 = packets[5];
    ASSERT_TRUE(packet5.has_timestamp());
    EXPECT_EQ(packet5.timestamp(), static_cast<uint64_t>(sfStartTime));
    ASSERT_TRUE(packet5.has_frame_timeline_event());

    const auto& event5 = packet5.frame_timeline_event();
    ASSERT_TRUE(event5.has_frame_end());
    const auto& actualSurfaceFrameEnd = event5.frame_end();
    validateTraceEvent(actualSurfaceFrameEnd, protoActualSurfaceFrameEnd);
}

// Tests for Jank classification
TEST_F(FrameTimelineTest, jankClassification_presentOnTimeDoesNotClassify) {
    // Layer specific increment
    EXPECT_CALL(*mTimeStats, incrementJankyFrames(_));
    auto presentFence1 = fenceFactory.createFenceTimeForTest(Fence::NO_FENCE);
    int64_t surfaceFrameToken = mTokenManager->generateTokenForPredictions({10, 20, 30});
    int64_t sfToken1 = mTokenManager->generateTokenForPredictions({22, 30, 30});
    FrameTimelineInfo ftInfo;
    ftInfo.vsyncId = surfaceFrameToken;
    ftInfo.inputEventId = sInputEventId;
    auto surfaceFrame =
            mFrameTimeline->createSurfaceFrameForToken(ftInfo, sPidOne, sUidOne, sLayerIdOne,
                                                       sLayerNameOne, sLayerNameOne,
                                                       /*isBuffer*/ true, sGameMode);
    mFrameTimeline->setSfWakeUp(sfToken1, 22, RR_11, RR_11);
    surfaceFrame->setPresentState(SurfaceFrame::PresentState::Presented);
    mFrameTimeline->addSurfaceFrame(surfaceFrame);
    mFrameTimeline->setSfPresent(26, presentFence1);
    auto displayFrame = getDisplayFrame(0);
    auto& presentedSurfaceFrame = getSurfaceFrame(0, 0);
    presentFence1->signalForTest(29);

    // Fences haven't been flushed yet, so it should be 0
    EXPECT_EQ(displayFrame->getActuals().presentTime, 0);
    EXPECT_EQ(presentedSurfaceFrame.getActuals().presentTime, 0);

    addEmptyDisplayFrame();
    displayFrame = getDisplayFrame(0);

    // Fences have flushed, so the present timestamps should be updated
    EXPECT_EQ(displayFrame->getActuals().presentTime, 29);
    EXPECT_EQ(presentedSurfaceFrame.getActuals().presentTime, 29);
    EXPECT_EQ(displayFrame->getFramePresentMetadata(), FramePresentMetadata::OnTimePresent);
    EXPECT_EQ(displayFrame->getFrameReadyMetadata(), FrameReadyMetadata::OnTimeFinish);
    EXPECT_EQ(displayFrame->getJankType(), JankType::None);
    EXPECT_EQ(displayFrame->getJankSeverityType(), JankSeverityType::None);
}

TEST_F(FrameTimelineTest, jankClassification_displayFrameOnTimeFinishEarlyPresent) {
    Fps vsyncRate = RR_11;
    auto presentFence1 = fenceFactory.createFenceTimeForTest(Fence::NO_FENCE);
    int64_t sfToken1 = mTokenManager->generateTokenForPredictions({22, 30, 40});
    int64_t sfToken2 = mTokenManager->generateTokenForPredictions({52, 60, 70});
    mFrameTimeline->setSfWakeUp(sfToken1, 22, vsyncRate, vsyncRate);
    mFrameTimeline->setSfPresent(26, presentFence1);
    auto displayFrame = getDisplayFrame(0);
    presentFence1->signalForTest(30);

    // Fences for the first frame haven't been flushed yet, so it should be 0
    EXPECT_EQ(displayFrame->getActuals().presentTime, 0);

    // Trigger a flush by finalizing the next DisplayFrame
    auto presentFence2 = fenceFactory.createFenceTimeForTest(Fence::NO_FENCE);
    mFrameTimeline->setSfWakeUp(sfToken2, 52, vsyncRate, vsyncRate);
    mFrameTimeline->setSfPresent(56, presentFence2);
    displayFrame = getDisplayFrame(0);

    // Fences for the first frame have flushed, so the present timestamps should be updated
    EXPECT_EQ(displayFrame->getActuals().presentTime, 30);
    EXPECT_EQ(displayFrame->getFramePresentMetadata(), FramePresentMetadata::EarlyPresent);
    EXPECT_EQ(displayFrame->getFrameReadyMetadata(), FrameReadyMetadata::OnTimeFinish);
    EXPECT_EQ(displayFrame->getJankType(), JankType::SurfaceFlingerScheduling);
    EXPECT_EQ(displayFrame->getJankSeverityType(), JankSeverityType::Partial);

    // Fences for the second frame haven't been flushed yet, so it should be 0
    auto displayFrame2 = getDisplayFrame(1);
    presentFence2->signalForTest(65);
    EXPECT_EQ(displayFrame2->getActuals().presentTime, 0);
    addEmptyDisplayFrame();
    displayFrame2 = getDisplayFrame(1);

    // Fences for the second frame have flushed, so the present timestamps should be updated
    EXPECT_EQ(displayFrame2->getActuals().presentTime, 65);
    EXPECT_EQ(displayFrame2->getFramePresentMetadata(), FramePresentMetadata::EarlyPresent);
    EXPECT_EQ(displayFrame2->getFrameReadyMetadata(), FrameReadyMetadata::OnTimeFinish);
    EXPECT_EQ(displayFrame2->getJankType(), JankType::PredictionError);
    EXPECT_EQ(displayFrame->getJankSeverityType(), JankSeverityType::Partial);
}

TEST_F(FrameTimelineTest, jankClassification_displayFrameOnTimeFinishLatePresent) {
    Fps vsyncRate = RR_11;
    auto presentFence1 = fenceFactory.createFenceTimeForTest(Fence::NO_FENCE);
    int64_t sfToken1 = mTokenManager->generateTokenForPredictions({22, 30, 40});
    int64_t sfToken2 = mTokenManager->generateTokenForPredictions({52, 60, 70});
    mFrameTimeline->setSfWakeUp(sfToken1, 22, vsyncRate, vsyncRate);
    mFrameTimeline->setSfPresent(26, presentFence1);
    auto displayFrame = getDisplayFrame(0);
    presentFence1->signalForTest(50);

    // Fences for the first frame haven't been flushed yet, so it should be 0
    EXPECT_EQ(displayFrame->getActuals().presentTime, 0);

    // Trigger a flush by finalizing the next DisplayFrame
    auto presentFence2 = fenceFactory.createFenceTimeForTest(Fence::NO_FENCE);
    mFrameTimeline->setSfWakeUp(sfToken2, 52, vsyncRate, vsyncRate);
    mFrameTimeline->setSfPresent(56, presentFence2);
    displayFrame = getDisplayFrame(0);

    // Fences for the first frame have flushed, so the present timestamps should be updated
    EXPECT_EQ(displayFrame->getActuals().presentTime, 50);
    EXPECT_EQ(displayFrame->getFramePresentMetadata(), FramePresentMetadata::LatePresent);
    EXPECT_EQ(displayFrame->getFrameReadyMetadata(), FrameReadyMetadata::OnTimeFinish);
    EXPECT_EQ(displayFrame->getJankType(), JankType::DisplayHAL);
    EXPECT_EQ(displayFrame->getJankSeverityType(), JankSeverityType::Partial);

    // Fences for the second frame haven't been flushed yet, so it should be 0
    auto displayFrame2 = getDisplayFrame(1);
    presentFence2->signalForTest(75);
    EXPECT_EQ(displayFrame2->getActuals().presentTime, 0);

    addEmptyDisplayFrame();
    displayFrame2 = getDisplayFrame(1);

    // Fences for the second frame have flushed, so the present timestamps should be updated
    EXPECT_EQ(displayFrame2->getActuals().presentTime, 75);
    EXPECT_EQ(displayFrame2->getFramePresentMetadata(), FramePresentMetadata::LatePresent);
    EXPECT_EQ(displayFrame2->getFrameReadyMetadata(), FrameReadyMetadata::OnTimeFinish);
    EXPECT_EQ(displayFrame2->getJankType(), JankType::PredictionError);
    EXPECT_EQ(displayFrame->getJankSeverityType(), JankSeverityType::Partial);
}

TEST_F(FrameTimelineTest, jankClassification_displayFrameLateFinishEarlyPresent) {
    auto presentFence1 = fenceFactory.createFenceTimeForTest(Fence::NO_FENCE);
    int64_t sfToken1 = mTokenManager->generateTokenForPredictions({12, 18, 40});
    mFrameTimeline->setSfWakeUp(sfToken1, 12, RR_11, RR_11);

    mFrameTimeline->setSfPresent(22, presentFence1);
    auto displayFrame = getDisplayFrame(0);
    presentFence1->signalForTest(28);

    // Fences haven't been flushed yet, so it should be 0
    EXPECT_EQ(displayFrame->getActuals().presentTime, 0);

    addEmptyDisplayFrame();
    displayFrame = getDisplayFrame(0);

    // Fences have flushed, so the present timestamps should be updated
    EXPECT_EQ(displayFrame->getActuals().presentTime, 28);
    EXPECT_EQ(displayFrame->getFramePresentMetadata(), FramePresentMetadata::EarlyPresent);
    EXPECT_EQ(displayFrame->getFrameReadyMetadata(), FrameReadyMetadata::LateFinish);
    EXPECT_EQ(displayFrame->getJankType(), JankType::SurfaceFlingerScheduling);
    EXPECT_EQ(displayFrame->getJankSeverityType(), JankSeverityType::Full);
}

TEST_F(FrameTimelineTest, jankClassification_displayFrameLateFinishLatePresent) {
    /*
     * Case 1 - cpu time > vsync period but combined time > deadline > deadline -> cpudeadlinemissed
     * Case 2 - cpu time < vsync period but combined time > deadline -> gpudeadlinemissed
     * Case 3 - previous frame ran longer -> sf_stuffing
     * Case 4 - Long cpu under SF stuffing -> cpudeadlinemissed
     */
    auto presentFence1 = fenceFactory.createFenceTimeForTest(Fence::NO_FENCE);
    auto presentFence2 = fenceFactory.createFenceTimeForTest(Fence::NO_FENCE);
    auto presentFence3 = fenceFactory.createFenceTimeForTest(Fence::NO_FENCE);
    auto presentFence4 = fenceFactory.createFenceTimeForTest(Fence::NO_FENCE);
    auto gpuFence1 = fenceFactory.createFenceTimeForTest(Fence::NO_FENCE);
    auto gpuFence2 = fenceFactory.createFenceTimeForTest(Fence::NO_FENCE);
    auto gpuFence3 = fenceFactory.createFenceTimeForTest(Fence::NO_FENCE);
    auto gpuFence4 = fenceFactory.createFenceTimeForTest(Fence::NO_FENCE);
    int64_t sfToken1 = mTokenManager->generateTokenForPredictions({22, 26, 40});
    int64_t sfToken2 = mTokenManager->generateTokenForPredictions({52, 60, 60});
    int64_t sfToken3 = mTokenManager->generateTokenForPredictions({82, 90, 90});
    int64_t sfToken4 = mTokenManager->generateTokenForPredictions({112, 120, 120});

    // case 1 - cpu time = 33 - 12 = 21, vsync period = 11
    mFrameTimeline->setSfWakeUp(sfToken1, 12, RR_11, RR_11);
    mFrameTimeline->setSfPresent(33, presentFence1, gpuFence1);
    auto displayFrame0 = getDisplayFrame(0);
    gpuFence1->signalForTest(36);
    presentFence1->signalForTest(52);

    // Fences haven't been flushed yet, so it should be 0
    EXPECT_EQ(displayFrame0->getActuals().presentTime, 0);

    // case 2 - cpu time = 56 - 52 = 4, vsync period = 30
    mFrameTimeline->setSfWakeUp(sfToken2, 52, RR_30, RR_30);
    mFrameTimeline->setSfPresent(56, presentFence2, gpuFence2);
    auto displayFrame1 = getDisplayFrame(1);
    gpuFence2->signalForTest(76);
    presentFence2->signalForTest(90);

    EXPECT_EQ(displayFrame1->getActuals().presentTime, 0);
    // Fences have flushed for first displayFrame, so the present timestamps should be updated
    EXPECT_EQ(displayFrame0->getActuals().presentTime, 52);
    EXPECT_EQ(displayFrame0->getFramePresentMetadata(), FramePresentMetadata::LatePresent);
    EXPECT_EQ(displayFrame0->getFrameReadyMetadata(), FrameReadyMetadata::LateFinish);
    EXPECT_EQ(displayFrame0->getJankType(), JankType::SurfaceFlingerGpuDeadlineMissed);
    EXPECT_EQ(displayFrame0->getJankSeverityType(), JankSeverityType::Full);

    // case 3 - cpu time = 86 - 82 = 4, vsync period = 30
    mFrameTimeline->setSfWakeUp(sfToken3, 106, RR_30, RR_30);
    mFrameTimeline->setSfPresent(112, presentFence3, gpuFence3);
    auto displayFrame2 = getDisplayFrame(2);
    gpuFence3->signalForTest(116);
    presentFence3->signalForTest(120);

    EXPECT_EQ(displayFrame2->getActuals().presentTime, 0);
    // Fences have flushed for second displayFrame, so the present timestamps should be updated
    EXPECT_EQ(displayFrame1->getActuals().presentTime, 90);
    EXPECT_EQ(displayFrame1->getFramePresentMetadata(), FramePresentMetadata::LatePresent);
    EXPECT_EQ(displayFrame1->getFrameReadyMetadata(), FrameReadyMetadata::LateFinish);
    EXPECT_EQ(displayFrame1->getJankType(), JankType::SurfaceFlingerGpuDeadlineMissed);
    EXPECT_EQ(displayFrame1->getJankSeverityType(), JankSeverityType::Full);

    // case 4 - cpu time = 86 - 82 = 4, vsync period = 30
    mFrameTimeline->setSfWakeUp(sfToken4, 120, RR_30, RR_30);
    mFrameTimeline->setSfPresent(140, presentFence4, gpuFence4);
    auto displayFrame3 = getDisplayFrame(3);
    gpuFence4->signalForTest(156);
    presentFence4->signalForTest(180);

    EXPECT_EQ(displayFrame3->getActuals().presentTime, 0);
    // Fences have flushed for third displayFrame, so the present timestamps should be updated
    EXPECT_EQ(displayFrame2->getActuals().presentTime, 120);
    EXPECT_EQ(displayFrame2->getFramePresentMetadata(), FramePresentMetadata::LatePresent);
    EXPECT_EQ(displayFrame2->getFrameReadyMetadata(), FrameReadyMetadata::LateFinish);
    EXPECT_EQ(displayFrame2->getJankType(), JankType::SurfaceFlingerStuffing);
    EXPECT_EQ(displayFrame2->getJankSeverityType(), JankSeverityType::Full);

    addEmptyDisplayFrame();

    // Fences have flushed for third displayFrame, so the present timestamps should be updated
    EXPECT_EQ(displayFrame3->getActuals().presentTime, 180);
    EXPECT_EQ(displayFrame3->getFramePresentMetadata(), FramePresentMetadata::LatePresent);
    EXPECT_EQ(displayFrame3->getFrameReadyMetadata(), FrameReadyMetadata::LateFinish);
    EXPECT_EQ(displayFrame3->getJankType(), JankType::SurfaceFlingerGpuDeadlineMissed);
    EXPECT_EQ(displayFrame3->getJankSeverityType(), JankSeverityType::Full);
}

TEST_F(FrameTimelineTest, jankClassification_surfaceFrameOnTimeFinishEarlyPresent) {
    EXPECT_CALL(*mTimeStats, incrementJankyFrames(_));
    auto presentFence1 = fenceFactory.createFenceTimeForTest(Fence::NO_FENCE);
    int64_t sfToken1 = mTokenManager->generateTokenForPredictions({22, 30, 40});
    int64_t sfToken2 = mTokenManager->generateTokenForPredictions({52, 60, 70});
    int64_t surfaceFrameToken1 = mTokenManager->generateTokenForPredictions({5, 16, 40});
    int64_t surfaceFrameToken2 = mTokenManager->generateTokenForPredictions({25, 36, 70});
    FrameTimelineInfo ftInfo;
    ftInfo.vsyncId = surfaceFrameToken1;
    ftInfo.inputEventId = sInputEventId;
    auto surfaceFrame1 =
            mFrameTimeline->createSurfaceFrameForToken(ftInfo, sPidOne, sUidOne, sLayerIdOne,
                                                       sLayerNameOne, sLayerNameOne,
                                                       /*isBuffer*/ true, sGameMode);
    surfaceFrame1->setAcquireFenceTime(16);
    mFrameTimeline->setSfWakeUp(sfToken1, 22, RR_11, RR_11);
    surfaceFrame1->setPresentState(SurfaceFrame::PresentState::Presented);
    mFrameTimeline->addSurfaceFrame(surfaceFrame1);
    mFrameTimeline->setSfPresent(27, presentFence1);
    auto displayFrame1 = getDisplayFrame(0);
    auto& presentedSurfaceFrame1 = getSurfaceFrame(0, 0);
    presentFence1->signalForTest(30);

    // Fences for the first frame haven't been flushed yet, so it should be 0
    EXPECT_EQ(displayFrame1->getActuals().presentTime, 0);
    auto actuals1 = presentedSurfaceFrame1.getActuals();
    EXPECT_EQ(actuals1.presentTime, 0);

    // Trigger a flush by finalizing the next DisplayFrame
    auto presentFence2 = fenceFactory.createFenceTimeForTest(Fence::NO_FENCE);
    FrameTimelineInfo ftInfo2;
    ftInfo2.vsyncId = surfaceFrameToken2;
    ftInfo2.inputEventId = sInputEventId;
    auto surfaceFrame2 =
            mFrameTimeline->createSurfaceFrameForToken(ftInfo2, sPidOne, sUidOne, sLayerIdOne,
                                                       sLayerNameOne, sLayerNameOne,
                                                       /*isBuffer*/ true, sGameMode);
    surfaceFrame2->setAcquireFenceTime(36);
    mFrameTimeline->setSfWakeUp(sfToken2, 52, RR_11, RR_11);
    surfaceFrame2->setPresentState(SurfaceFrame::PresentState::Presented);
    mFrameTimeline->addSurfaceFrame(surfaceFrame2);
    mFrameTimeline->setSfPresent(57, presentFence2);
    auto displayFrame2 = getDisplayFrame(1);
    auto& presentedSurfaceFrame2 = getSurfaceFrame(1, 0);

    // Fences for the first frame have flushed, so the present timestamps should be updated
    EXPECT_EQ(displayFrame1->getActuals().presentTime, 30);
    EXPECT_EQ(displayFrame1->getFramePresentMetadata(), FramePresentMetadata::EarlyPresent);
    EXPECT_EQ(displayFrame1->getFrameReadyMetadata(), FrameReadyMetadata::OnTimeFinish);
    EXPECT_EQ(displayFrame1->getJankType(), JankType::SurfaceFlingerScheduling);
    EXPECT_EQ(displayFrame1->getJankSeverityType(), JankSeverityType::Partial);

    actuals1 = presentedSurfaceFrame1.getActuals();
    EXPECT_EQ(actuals1.presentTime, 30);
    EXPECT_EQ(presentedSurfaceFrame1.getFramePresentMetadata(), FramePresentMetadata::EarlyPresent);
    EXPECT_EQ(presentedSurfaceFrame1.getFrameReadyMetadata(), FrameReadyMetadata::OnTimeFinish);
    EXPECT_EQ(presentedSurfaceFrame1.getJankType(), JankType::SurfaceFlingerScheduling);
    EXPECT_EQ(presentedSurfaceFrame1.getJankSeverityType(), JankSeverityType::Partial);

    // Fences for the second frame haven't been flushed yet, so it should be 0
    presentFence2->signalForTest(65);
    EXPECT_EQ(displayFrame2->getActuals().presentTime, 0);
    auto actuals2 = presentedSurfaceFrame2.getActuals();
    EXPECT_EQ(actuals2.presentTime, 0);

    ::testing::Mock::VerifyAndClearExpectations(mTimeStats.get());

    EXPECT_CALL(*mTimeStats,
                incrementJankyFrames(TimeStats::JankyFramesInfo{RR_11, std::nullopt, sUidOne,
                                                                sLayerNameOne, sGameMode,
                                                                JankType::PredictionError, -3, 5,
                                                                0}));

    addEmptyDisplayFrame();

    // Fences for the second frame have flushed, so the present timestamps should be updated
    EXPECT_EQ(displayFrame2->getActuals().presentTime, 65);
    EXPECT_EQ(displayFrame2->getFramePresentMetadata(), FramePresentMetadata::EarlyPresent);
    EXPECT_EQ(displayFrame2->getFrameReadyMetadata(), FrameReadyMetadata::OnTimeFinish);
    EXPECT_EQ(displayFrame2->getJankType(), JankType::PredictionError);
    EXPECT_EQ(displayFrame2->getJankSeverityType(), JankSeverityType::Partial);

    actuals2 = presentedSurfaceFrame2.getActuals();
    EXPECT_EQ(actuals2.presentTime, 65);
    EXPECT_EQ(presentedSurfaceFrame2.getFramePresentMetadata(), FramePresentMetadata::EarlyPresent);
    EXPECT_EQ(presentedSurfaceFrame2.getFrameReadyMetadata(), FrameReadyMetadata::OnTimeFinish);
    EXPECT_EQ(presentedSurfaceFrame2.getJankType(), JankType::PredictionError);
    EXPECT_EQ(presentedSurfaceFrame2.getJankSeverityType(), JankSeverityType::Partial);
}

TEST_F(FrameTimelineTest, jankClassification_surfaceFrameOnTimeFinishLatePresent) {
    EXPECT_CALL(*mTimeStats, incrementJankyFrames(_));
    auto presentFence1 = fenceFactory.createFenceTimeForTest(Fence::NO_FENCE);
    int64_t sfToken1 = mTokenManager->generateTokenForPredictions({22, 30, 40});
    int64_t sfToken2 = mTokenManager->generateTokenForPredictions({52, 60, 70});
    int64_t surfaceFrameToken1 = mTokenManager->generateTokenForPredictions({5, 16, 40});
    int64_t surfaceFrameToken2 = mTokenManager->generateTokenForPredictions({25, 36, 70});
    FrameTimelineInfo ftInfo;
    ftInfo.vsyncId = surfaceFrameToken1;
    ftInfo.inputEventId = sInputEventId;
    auto surfaceFrame1 =
            mFrameTimeline->createSurfaceFrameForToken(ftInfo, sPidOne, sUidOne, sLayerIdOne,
                                                       sLayerNameOne, sLayerNameOne,
                                                       /*isBuffer*/ true, sGameMode);
    surfaceFrame1->setAcquireFenceTime(16);
    mFrameTimeline->setSfWakeUp(sfToken1, 22, RR_11, RR_11);
    surfaceFrame1->setPresentState(SurfaceFrame::PresentState::Presented);
    mFrameTimeline->addSurfaceFrame(surfaceFrame1);
    mFrameTimeline->setSfPresent(26, presentFence1);
    auto displayFrame1 = getDisplayFrame(0);
    auto& presentedSurfaceFrame1 = getSurfaceFrame(0, 0);
    presentFence1->signalForTest(50);

    // Fences for the first frame haven't been flushed yet, so it should be 0
    EXPECT_EQ(displayFrame1->getActuals().presentTime, 0);
    auto actuals1 = presentedSurfaceFrame1.getActuals();
    EXPECT_EQ(actuals1.presentTime, 0);

    // Trigger a flush by finalizing the next DisplayFrame
    auto presentFence2 = fenceFactory.createFenceTimeForTest(Fence::NO_FENCE);
    FrameTimelineInfo ftInfo2;
    ftInfo2.vsyncId = surfaceFrameToken2;
    ftInfo2.inputEventId = sInputEventId;
    auto surfaceFrame2 =
            mFrameTimeline->createSurfaceFrameForToken(ftInfo2, sPidOne, sUidOne, sLayerIdOne,
                                                       sLayerNameOne, sLayerNameOne,
                                                       /*isBuffer*/ true, sGameMode);
    surfaceFrame2->setAcquireFenceTime(36);
    mFrameTimeline->setSfWakeUp(sfToken2, 52, RR_11, RR_11);
    surfaceFrame2->setPresentState(SurfaceFrame::PresentState::Presented);
    mFrameTimeline->addSurfaceFrame(surfaceFrame2);
    mFrameTimeline->setSfPresent(57, presentFence2);
    auto displayFrame2 = getDisplayFrame(1);
    auto& presentedSurfaceFrame2 = getSurfaceFrame(1, 0);

    // Fences for the first frame have flushed, so the present timestamps should be updated
    EXPECT_EQ(displayFrame1->getActuals().presentTime, 50);
    EXPECT_EQ(displayFrame1->getFramePresentMetadata(), FramePresentMetadata::LatePresent);
    EXPECT_EQ(displayFrame1->getFrameReadyMetadata(), FrameReadyMetadata::OnTimeFinish);
    EXPECT_EQ(displayFrame1->getJankType(), JankType::DisplayHAL);
    EXPECT_EQ(displayFrame1->getJankSeverityType(), JankSeverityType::Partial);

    actuals1 = presentedSurfaceFrame1.getActuals();
    EXPECT_EQ(actuals1.presentTime, 50);
    EXPECT_EQ(presentedSurfaceFrame1.getFramePresentMetadata(), FramePresentMetadata::LatePresent);
    EXPECT_EQ(presentedSurfaceFrame1.getFrameReadyMetadata(), FrameReadyMetadata::OnTimeFinish);
    EXPECT_EQ(presentedSurfaceFrame1.getJankType(), JankType::DisplayHAL);
    EXPECT_EQ(presentedSurfaceFrame1.getJankSeverityType(), JankSeverityType::Partial);

    // Fences for the second frame haven't been flushed yet, so it should be 0
    presentFence2->signalForTest(86);
    EXPECT_EQ(displayFrame2->getActuals().presentTime, 0);
    auto actuals2 = presentedSurfaceFrame2.getActuals();
    EXPECT_EQ(actuals2.presentTime, 0);

    ::testing::Mock::VerifyAndClearExpectations(mTimeStats.get());

    EXPECT_CALL(*mTimeStats,
                incrementJankyFrames(TimeStats::JankyFramesInfo{RR_11, std::nullopt, sUidOne,
                                                                sLayerNameOne, sGameMode,
                                                                JankType::PredictionError, -3, 5,
                                                                0}));

    addEmptyDisplayFrame();

    // Fences for the second frame have flushed, so the present timestamps should be updated
    EXPECT_EQ(displayFrame2->getActuals().presentTime, 86);
    EXPECT_EQ(displayFrame2->getFramePresentMetadata(), FramePresentMetadata::LatePresent);
    EXPECT_EQ(displayFrame2->getFrameReadyMetadata(), FrameReadyMetadata::OnTimeFinish);
    EXPECT_EQ(displayFrame2->getJankType(), JankType::PredictionError);
    EXPECT_EQ(displayFrame2->getJankSeverityType(), JankSeverityType::Full);

    actuals2 = presentedSurfaceFrame2.getActuals();
    EXPECT_EQ(actuals2.presentTime, 86);
    EXPECT_EQ(presentedSurfaceFrame2.getFramePresentMetadata(), FramePresentMetadata::LatePresent);
    EXPECT_EQ(presentedSurfaceFrame2.getFrameReadyMetadata(), FrameReadyMetadata::OnTimeFinish);
    EXPECT_EQ(presentedSurfaceFrame2.getJankType(), JankType::PredictionError);
    EXPECT_EQ(presentedSurfaceFrame2.getJankSeverityType(), JankSeverityType::Full);
}

TEST_F(FrameTimelineTest, jankClassification_surfaceFrameLateFinishEarlyPresent) {
    EXPECT_CALL(*mTimeStats, incrementJankyFrames(_));

    auto presentFence1 = fenceFactory.createFenceTimeForTest(Fence::NO_FENCE);
    int64_t sfToken1 = mTokenManager->generateTokenForPredictions({42, 50, 50});
    int64_t surfaceFrameToken1 = mTokenManager->generateTokenForPredictions({5, 26, 60});
    FrameTimelineInfo ftInfo;
    ftInfo.vsyncId = surfaceFrameToken1;
    ftInfo.inputEventId = sInputEventId;
    auto surfaceFrame1 =
            mFrameTimeline->createSurfaceFrameForToken(ftInfo, sPidOne, sUidOne, sLayerIdOne,
                                                       sLayerNameOne, sLayerNameOne,
                                                       /*isBuffer*/ true, sGameMode);
    surfaceFrame1->setAcquireFenceTime(40);
    mFrameTimeline->setSfWakeUp(sfToken1, 42, RR_11, RR_11);
    surfaceFrame1->setPresentState(SurfaceFrame::PresentState::Presented);
    mFrameTimeline->addSurfaceFrame(surfaceFrame1);
    mFrameTimeline->setSfPresent(46, presentFence1);
    auto displayFrame1 = getDisplayFrame(0);
    auto& presentedSurfaceFrame1 = getSurfaceFrame(0, 0);
    presentFence1->signalForTest(50);

    // Fences for the first frame haven't been flushed yet, so it should be 0
    EXPECT_EQ(displayFrame1->getActuals().presentTime, 0);
    auto actuals1 = presentedSurfaceFrame1.getActuals();
    EXPECT_EQ(actuals1.presentTime, 0);

    addEmptyDisplayFrame();

    // Fences for the first frame have flushed, so the present timestamps should be updated
    EXPECT_EQ(displayFrame1->getActuals().presentTime, 50);
    EXPECT_EQ(displayFrame1->getFramePresentMetadata(), FramePresentMetadata::OnTimePresent);
    EXPECT_EQ(displayFrame1->getFrameReadyMetadata(), FrameReadyMetadata::OnTimeFinish);
    EXPECT_EQ(displayFrame1->getJankType(), JankType::None);
    EXPECT_EQ(displayFrame1->getJankSeverityType(), JankSeverityType::None);

    actuals1 = presentedSurfaceFrame1.getActuals();
    EXPECT_EQ(actuals1.presentTime, 50);
    EXPECT_EQ(presentedSurfaceFrame1.getFramePresentMetadata(), FramePresentMetadata::EarlyPresent);
    EXPECT_EQ(presentedSurfaceFrame1.getFrameReadyMetadata(), FrameReadyMetadata::LateFinish);
    EXPECT_EQ(presentedSurfaceFrame1.getJankType(), JankType::Unknown);
    EXPECT_EQ(presentedSurfaceFrame1.getJankSeverityType(), JankSeverityType::Partial);
}

TEST_F(FrameTimelineTest, jankClassification_surfaceFrameLateFinishLatePresent) {
    // First frame - DisplayFrame is not janky. This should classify the SurfaceFrame as only
    // AppDeadlineMissed. Second frame - DisplayFrame is janky. This should propagate DisplayFrame's
    // jank to the SurfaceFrame along with AppDeadlineMissed.

    EXPECT_CALL(*mTimeStats, incrementJankyFrames(_)).Times(2);
    auto presentFence1 = fenceFactory.createFenceTimeForTest(Fence::NO_FENCE);
    int64_t sfToken1 = mTokenManager->generateTokenForPredictions({32, 40, 40});
    int64_t sfToken2 = mTokenManager->generateTokenForPredictions({42, 50, 50});
    int64_t surfaceFrameToken1 = mTokenManager->generateTokenForPredictions({5, 16, 30});
    int64_t surfaceFrameToken2 = mTokenManager->generateTokenForPredictions({25, 36, 50});
    FrameTimelineInfo ftInfo;
    ftInfo.vsyncId = surfaceFrameToken1;
    ftInfo.inputEventId = sInputEventId;
    auto surfaceFrame1 =
            mFrameTimeline->createSurfaceFrameForToken(ftInfo, sPidOne, sUidOne, sLayerIdOne,
                                                       sLayerNameOne, sLayerNameOne,
                                                       /*isBuffer*/ true, sGameMode);
    surfaceFrame1->setAcquireFenceTime(26);
    mFrameTimeline->setSfWakeUp(sfToken1, 32, RR_11, RR_11);
    surfaceFrame1->setPresentState(SurfaceFrame::PresentState::Presented);
    mFrameTimeline->addSurfaceFrame(surfaceFrame1);
    mFrameTimeline->setSfPresent(36, presentFence1);
    auto displayFrame1 = getDisplayFrame(0);
    auto& presentedSurfaceFrame1 = getSurfaceFrame(0, 0);
    presentFence1->signalForTest(40);

    // Fences for the first frame haven't been flushed yet, so it should be 0
    EXPECT_EQ(displayFrame1->getActuals().presentTime, 0);
    auto actuals1 = presentedSurfaceFrame1.getActuals();
    EXPECT_EQ(actuals1.presentTime, 0);

    // Trigger a flush by finalizing the next DisplayFrame
    auto presentFence2 = fenceFactory.createFenceTimeForTest(Fence::NO_FENCE);
    FrameTimelineInfo ftInfo2;
    ftInfo2.vsyncId = surfaceFrameToken2;
    ftInfo2.inputEventId = sInputEventId;
    auto surfaceFrame2 =
            mFrameTimeline->createSurfaceFrameForToken(ftInfo2, sPidOne, sUidOne, sLayerIdOne,
                                                       sLayerNameOne, sLayerNameOne,
                                                       /*isBuffer*/ true, sGameMode);
    surfaceFrame2->setAcquireFenceTime(40);
    mFrameTimeline->setSfWakeUp(sfToken2, 43, RR_11, RR_11);
    surfaceFrame2->setPresentState(SurfaceFrame::PresentState::Presented);
    mFrameTimeline->addSurfaceFrame(surfaceFrame2);
    mFrameTimeline->setSfPresent(56, presentFence2);
    auto displayFrame2 = getDisplayFrame(1);
    auto& presentedSurfaceFrame2 = getSurfaceFrame(1, 0);

    // Fences for the first frame have flushed, so the present timestamps should be updated
    EXPECT_EQ(displayFrame1->getActuals().presentTime, 40);
    EXPECT_EQ(displayFrame1->getFramePresentMetadata(), FramePresentMetadata::OnTimePresent);
    EXPECT_EQ(displayFrame1->getFrameReadyMetadata(), FrameReadyMetadata::OnTimeFinish);
    EXPECT_EQ(displayFrame1->getJankType(), JankType::None);
    EXPECT_EQ(displayFrame1->getJankSeverityType(), JankSeverityType::None);

    actuals1 = presentedSurfaceFrame1.getActuals();
    EXPECT_EQ(actuals1.presentTime, 40);
    EXPECT_EQ(presentedSurfaceFrame1.getFramePresentMetadata(), FramePresentMetadata::LatePresent);
    EXPECT_EQ(presentedSurfaceFrame1.getFrameReadyMetadata(), FrameReadyMetadata::LateFinish);
    EXPECT_EQ(presentedSurfaceFrame1.getJankType(), JankType::AppDeadlineMissed);
    EXPECT_EQ(presentedSurfaceFrame1.getJankSeverityType(), JankSeverityType::Partial);

    // Fences for the second frame haven't been flushed yet, so it should be 0
    presentFence2->signalForTest(60);
    EXPECT_EQ(displayFrame2->getActuals().presentTime, 0);
    auto actuals2 = presentedSurfaceFrame2.getActuals();
    EXPECT_EQ(actuals2.presentTime, 0);

    addEmptyDisplayFrame();

    // Fences for the second frame have flushed, so the present timestamps should be updated
    EXPECT_EQ(displayFrame2->getActuals().presentTime, 60);
    EXPECT_EQ(displayFrame2->getFramePresentMetadata(), FramePresentMetadata::LatePresent);
    EXPECT_EQ(displayFrame2->getFrameReadyMetadata(), FrameReadyMetadata::LateFinish);
    EXPECT_EQ(displayFrame2->getJankType(), JankType::SurfaceFlingerCpuDeadlineMissed);
    EXPECT_EQ(displayFrame2->getJankSeverityType(), JankSeverityType::Partial);

    actuals2 = presentedSurfaceFrame2.getActuals();
    EXPECT_EQ(actuals2.presentTime, 60);
    EXPECT_EQ(presentedSurfaceFrame2.getFramePresentMetadata(), FramePresentMetadata::LatePresent);
    EXPECT_EQ(presentedSurfaceFrame2.getFrameReadyMetadata(), FrameReadyMetadata::LateFinish);
    EXPECT_EQ(presentedSurfaceFrame2.getJankType(),
              JankType::SurfaceFlingerCpuDeadlineMissed | JankType::AppDeadlineMissed);
    EXPECT_EQ(presentedSurfaceFrame2.getJankSeverityType(), JankSeverityType::Partial);
}

TEST_F(FrameTimelineTest, jankClassification_multiJankBufferStuffingAndAppDeadlineMissed) {
    // Layer specific increment
    EXPECT_CALL(*mTimeStats, incrementJankyFrames(_)).Times(2);
    auto presentFence1 = fenceFactory.createFenceTimeForTest(Fence::NO_FENCE);
    int64_t surfaceFrameToken1 = mTokenManager->generateTokenForPredictions({10, 20, 30});
    int64_t surfaceFrameToken2 = mTokenManager->generateTokenForPredictions({40, 50, 60});

    int64_t sfToken1 = mTokenManager->generateTokenForPredictions({52, 60, 60});
    int64_t sfToken2 = mTokenManager->generateTokenForPredictions({112, 120, 120});
    FrameTimelineInfo ftInfo;
    ftInfo.vsyncId = surfaceFrameToken1;
    ftInfo.inputEventId = sInputEventId;
    auto surfaceFrame1 =
            mFrameTimeline->createSurfaceFrameForToken(ftInfo, sPidOne, sUidOne, sLayerIdOne,
                                                       sLayerNameOne, sLayerNameOne,
                                                       /*isBuffer*/ true, sGameMode);
    surfaceFrame1->setAcquireFenceTime(50);
    mFrameTimeline->setSfWakeUp(sfToken1, 52, RR_30, RR_30);
    surfaceFrame1->setPresentState(SurfaceFrame::PresentState::Presented);
    mFrameTimeline->addSurfaceFrame(surfaceFrame1);
    mFrameTimeline->setSfPresent(56, presentFence1);
    auto displayFrame1 = getDisplayFrame(0);
    auto& presentedSurfaceFrame1 = getSurfaceFrame(0, 0);
    presentFence1->signalForTest(60);

    // Fences for the first frame haven't been flushed yet, so it should be 0
    EXPECT_EQ(displayFrame1->getActuals().presentTime, 0);
    auto actuals1 = presentedSurfaceFrame1.getActuals();
    EXPECT_EQ(actuals1.presentTime, 0);

    // Trigger a flush by finalizing the next DisplayFrame
    auto presentFence2 = fenceFactory.createFenceTimeForTest(Fence::NO_FENCE);
    FrameTimelineInfo ftInfo2;
    ftInfo2.vsyncId = surfaceFrameToken2;
    ftInfo2.inputEventId = sInputEventId;
    auto surfaceFrame2 =
            mFrameTimeline->createSurfaceFrameForToken(ftInfo2, sPidOne, sUidOne, sLayerIdOne,
                                                       sLayerNameOne, sLayerNameOne,
                                                       /*isBuffer*/ true, sGameMode);
    surfaceFrame2->setAcquireFenceTime(84);
    mFrameTimeline->setSfWakeUp(sfToken2, 112, RR_30, RR_30);
    surfaceFrame2->setPresentState(SurfaceFrame::PresentState::Presented, 54);
    mFrameTimeline->addSurfaceFrame(surfaceFrame2);
    mFrameTimeline->setSfPresent(116, presentFence2);
    auto displayFrame2 = getDisplayFrame(1);
    auto& presentedSurfaceFrame2 = getSurfaceFrame(1, 0);
    presentFence2->signalForTest(120);

    // Fences for the first frame have flushed, so the present timestamps should be updated
    EXPECT_EQ(displayFrame1->getActuals().presentTime, 60);
    actuals1 = presentedSurfaceFrame1.getActuals();
    EXPECT_EQ(actuals1.endTime, 50);
    EXPECT_EQ(actuals1.presentTime, 60);

    EXPECT_EQ(displayFrame1->getFramePresentMetadata(), FramePresentMetadata::OnTimePresent);
    EXPECT_EQ(displayFrame1->getFrameReadyMetadata(), FrameReadyMetadata::OnTimeFinish);
    EXPECT_EQ(displayFrame1->getJankType(), JankType::None);
    EXPECT_EQ(displayFrame1->getJankSeverityType(), JankSeverityType::None);

    EXPECT_EQ(presentedSurfaceFrame1.getFramePresentMetadata(), FramePresentMetadata::LatePresent);
    EXPECT_EQ(presentedSurfaceFrame1.getFrameReadyMetadata(), FrameReadyMetadata::LateFinish);
    EXPECT_EQ(presentedSurfaceFrame1.getJankType(), JankType::AppDeadlineMissed);
    EXPECT_EQ(presentedSurfaceFrame1.getJankSeverityType(), JankSeverityType::Full);

    // Fences for the second frame haven't been flushed yet, so it should be 0
    EXPECT_EQ(displayFrame2->getActuals().presentTime, 0);
    auto actuals2 = presentedSurfaceFrame2.getActuals();
    EXPECT_EQ(actuals2.presentTime, 0);

    addEmptyDisplayFrame();

    // Fences for the second frame have flushed, so the present timestamps should be updated
    EXPECT_EQ(displayFrame2->getActuals().presentTime, 120);
    actuals2 = presentedSurfaceFrame2.getActuals();
    EXPECT_EQ(actuals2.presentTime, 120);

    EXPECT_EQ(displayFrame2->getFramePresentMetadata(), FramePresentMetadata::OnTimePresent);
    EXPECT_EQ(displayFrame2->getFrameReadyMetadata(), FrameReadyMetadata::OnTimeFinish);
    EXPECT_EQ(displayFrame2->getJankType(), JankType::None);
    EXPECT_EQ(displayFrame2->getJankSeverityType(), JankSeverityType::None);

    EXPECT_EQ(presentedSurfaceFrame2.getFramePresentMetadata(), FramePresentMetadata::LatePresent);
    EXPECT_EQ(presentedSurfaceFrame2.getFrameReadyMetadata(), FrameReadyMetadata::LateFinish);
    EXPECT_EQ(presentedSurfaceFrame2.getJankType(),
              JankType::AppDeadlineMissed | JankType::BufferStuffing);
    EXPECT_EQ(presentedSurfaceFrame2.getJankSeverityType(), JankSeverityType::Full);
}

TEST_F(FrameTimelineTest, jankClassification_appDeadlineAdjustedForBufferStuffing) {
    // Layer specific increment
    EXPECT_CALL(*mTimeStats, incrementJankyFrames(_)).Times(2);
    auto presentFence1 = fenceFactory.createFenceTimeForTest(Fence::NO_FENCE);
    int64_t surfaceFrameToken1 = mTokenManager->generateTokenForPredictions({10, 20, 30});
    int64_t surfaceFrameToken2 = mTokenManager->generateTokenForPredictions({40, 50, 60});

    int64_t sfToken1 = mTokenManager->generateTokenForPredictions({52, 60, 60});
    int64_t sfToken2 = mTokenManager->generateTokenForPredictions({82, 90, 90});
    FrameTimelineInfo ftInfo;
    ftInfo.vsyncId = surfaceFrameToken1;
    ftInfo.inputEventId = sInputEventId;
    auto surfaceFrame1 =
            mFrameTimeline->createSurfaceFrameForToken(ftInfo, sPidOne, sUidOne, sLayerIdOne,
                                                       sLayerNameOne, sLayerNameOne,
                                                       /*isBuffer*/ true, sGameMode);
    surfaceFrame1->setAcquireFenceTime(50);
    mFrameTimeline->setSfWakeUp(sfToken1, 52, RR_30, RR_30);
    surfaceFrame1->setPresentState(SurfaceFrame::PresentState::Presented);
    mFrameTimeline->addSurfaceFrame(surfaceFrame1);
    mFrameTimeline->setSfPresent(56, presentFence1);
    auto displayFrame1 = getDisplayFrame(0);
    auto& presentedSurfaceFrame1 = getSurfaceFrame(0, 0);
    presentFence1->signalForTest(60);

    // Fences for the first frame haven't been flushed yet, so it should be 0
    EXPECT_EQ(displayFrame1->getActuals().presentTime, 0);
    auto actuals1 = presentedSurfaceFrame1.getActuals();
    EXPECT_EQ(actuals1.presentTime, 0);

    // Trigger a flush by finalizing the next DisplayFrame
    auto presentFence2 = fenceFactory.createFenceTimeForTest(Fence::NO_FENCE);
    FrameTimelineInfo ftInfo2;
    ftInfo2.vsyncId = surfaceFrameToken2;
    ftInfo2.inputEventId = sInputEventId;
    auto surfaceFrame2 =
            mFrameTimeline->createSurfaceFrameForToken(ftInfo2, sPidOne, sUidOne, sLayerIdOne,
                                                       sLayerNameOne, sLayerNameOne,
                                                       /*isBuffer*/ true, sGameMode);
    surfaceFrame2->setAcquireFenceTime(80);
    mFrameTimeline->setSfWakeUp(sfToken2, 82, RR_30, RR_30);
    // Setting previous latch time to 54, adjusted deadline will be 54 + vsyncTime(30) = 84
    surfaceFrame2->setPresentState(SurfaceFrame::PresentState::Presented, 54);
    mFrameTimeline->addSurfaceFrame(surfaceFrame2);
    mFrameTimeline->setSfPresent(86, presentFence2);
    auto displayFrame2 = getDisplayFrame(1);
    auto& presentedSurfaceFrame2 = getSurfaceFrame(1, 0);
    presentFence2->signalForTest(90);

    // Fences for the first frame have flushed, so the present timestamps should be updated
    EXPECT_EQ(displayFrame1->getActuals().presentTime, 60);
    actuals1 = presentedSurfaceFrame1.getActuals();
    EXPECT_EQ(actuals1.endTime, 50);
    EXPECT_EQ(actuals1.presentTime, 60);

    EXPECT_EQ(displayFrame1->getFramePresentMetadata(), FramePresentMetadata::OnTimePresent);
    EXPECT_EQ(displayFrame1->getFrameReadyMetadata(), FrameReadyMetadata::OnTimeFinish);
    EXPECT_EQ(displayFrame1->getJankType(), JankType::None);
    EXPECT_EQ(displayFrame1->getJankSeverityType(), JankSeverityType::None);

    EXPECT_EQ(presentedSurfaceFrame1.getFramePresentMetadata(), FramePresentMetadata::LatePresent);
    EXPECT_EQ(presentedSurfaceFrame1.getFrameReadyMetadata(), FrameReadyMetadata::LateFinish);
    EXPECT_EQ(presentedSurfaceFrame1.getJankType(), JankType::AppDeadlineMissed);
    EXPECT_EQ(presentedSurfaceFrame1.getJankSeverityType(), JankSeverityType::Full);

    // Fences for the second frame haven't been flushed yet, so it should be 0
    EXPECT_EQ(displayFrame2->getActuals().presentTime, 0);
    auto actuals2 = presentedSurfaceFrame2.getActuals();
    EXPECT_EQ(actuals2.presentTime, 0);

    addEmptyDisplayFrame();

    // Fences for the second frame have flushed, so the present timestamps should be updated
    EXPECT_EQ(displayFrame2->getActuals().presentTime, 90);
    actuals2 = presentedSurfaceFrame2.getActuals();
    EXPECT_EQ(actuals2.presentTime, 90);

    EXPECT_EQ(displayFrame2->getFramePresentMetadata(), FramePresentMetadata::OnTimePresent);
    EXPECT_EQ(displayFrame2->getFrameReadyMetadata(), FrameReadyMetadata::OnTimeFinish);
    EXPECT_EQ(displayFrame2->getJankType(), JankType::None);
    EXPECT_EQ(displayFrame2->getJankSeverityType(), JankSeverityType::None);

    EXPECT_EQ(presentedSurfaceFrame2.getFramePresentMetadata(), FramePresentMetadata::LatePresent);
    EXPECT_EQ(presentedSurfaceFrame2.getFrameReadyMetadata(), FrameReadyMetadata::OnTimeFinish);
    EXPECT_EQ(presentedSurfaceFrame2.getJankType(), JankType::BufferStuffing);
    EXPECT_EQ(presentedSurfaceFrame2.getJankSeverityType(), JankSeverityType::Full);
}

TEST_F(FrameTimelineTest, jankClassification_displayFrameLateFinishLatePresent_GpuAndCpuMiss) {
    auto presentFence1 = fenceFactory.createFenceTimeForTest(Fence::NO_FENCE);
    auto presentFence2 = fenceFactory.createFenceTimeForTest(Fence::NO_FENCE);
    auto gpuFence1 = fenceFactory.createFenceTimeForTest(Fence::NO_FENCE);
    int64_t sfToken1 = mTokenManager->generateTokenForPredictions({22, 26, 40});
    int64_t sfToken2 = mTokenManager->generateTokenForPredictions({52, 60, 60});

    // Case 1: cpu time = 33 - 12 = 21, vsync period = 11
    mFrameTimeline->setSfWakeUp(sfToken1, 12, RR_11, RR_11);
    mFrameTimeline->setSfPresent(33, presentFence1, gpuFence1);
    auto displayFrame = getDisplayFrame(0);
    gpuFence1->signalForTest(36);
    presentFence1->signalForTest(52);

    // Fences haven't been flushed yet, so it should be 0
    EXPECT_EQ(displayFrame->getActuals().presentTime, 0);

    addEmptyDisplayFrame();
    displayFrame = getDisplayFrame(0);

    // Fences have flushed, so the present timestamps should be updated
    EXPECT_EQ(displayFrame->getActuals().presentTime, 52);
    EXPECT_EQ(displayFrame->getFramePresentMetadata(), FramePresentMetadata::LatePresent);
    EXPECT_EQ(displayFrame->getFrameReadyMetadata(), FrameReadyMetadata::LateFinish);
    EXPECT_EQ(displayFrame->getJankType(), JankType::SurfaceFlingerGpuDeadlineMissed);
    EXPECT_EQ(displayFrame->getJankSeverityType(), JankSeverityType::Full);

    // Case 2: No GPU fence so it will not use GPU composition.
    mFrameTimeline->setSfWakeUp(sfToken2, 52, RR_30, RR_30);
    mFrameTimeline->setSfPresent(66, presentFence2);
    auto displayFrame2 = getDisplayFrame(2); // 2 because of previous empty frame
    presentFence2->signalForTest(90);

    // Fences for the frame haven't been flushed yet, so it should be 0
    EXPECT_EQ(displayFrame2->getActuals().presentTime, 0);

    addEmptyDisplayFrame();

    // Fences have flushed, so the present timestamps should be updated
    EXPECT_EQ(displayFrame2->getActuals().presentTime, 90);
    EXPECT_EQ(displayFrame2->getFramePresentMetadata(), FramePresentMetadata::LatePresent);
    EXPECT_EQ(displayFrame2->getFrameReadyMetadata(), FrameReadyMetadata::LateFinish);
    EXPECT_EQ(displayFrame2->getJankType(), JankType::SurfaceFlingerCpuDeadlineMissed);
    EXPECT_EQ(displayFrame2->getJankSeverityType(), JankSeverityType::Full);
}

TEST_F(FrameTimelineTest, jankClassification_presentFenceError) {
    auto erroneousPresentFence1 = fenceFactory.createFenceTimeForTest(Fence::NO_FENCE);
    auto erroneousPresentFence2 = fenceFactory.createFenceTimeForTest(Fence::NO_FENCE);
    auto validPresentFence = fenceFactory.createFenceTimeForTest(Fence::NO_FENCE);
    int64_t sfToken1 = mTokenManager->generateTokenForPredictions({22, 26, 40});
    int64_t sfToken2 = mTokenManager->generateTokenForPredictions({52, 60, 60});
    int64_t sfToken3 = mTokenManager->generateTokenForPredictions({72, 80, 80});

    mFrameTimeline->setSfWakeUp(sfToken1, 22, RR_11, RR_11);
    mFrameTimeline->setSfPresent(26, erroneousPresentFence1);

    mFrameTimeline->setSfWakeUp(sfToken2, 52, RR_11, RR_11);
    mFrameTimeline->setSfPresent(60, erroneousPresentFence2);

    mFrameTimeline->setSfWakeUp(sfToken3, 72, RR_11, RR_11);
    mFrameTimeline->setSfPresent(80, validPresentFence);

    erroneousPresentFence2->signalForTest(2);
    validPresentFence->signalForTest(80);

    addEmptyDisplayFrame();

    {
        auto displayFrame = getDisplayFrame(0);
        EXPECT_EQ(displayFrame->getActuals().presentTime, 26);
        EXPECT_EQ(displayFrame->getFramePresentMetadata(), FramePresentMetadata::UnknownPresent);
        EXPECT_EQ(displayFrame->getFrameReadyMetadata(), FrameReadyMetadata::UnknownFinish);
        EXPECT_EQ(displayFrame->getJankType(), JankType::Unknown | JankType::DisplayHAL);
        EXPECT_EQ(displayFrame->getJankSeverityType(), JankSeverityType::Unknown);
    }
    {
        auto displayFrame = getDisplayFrame(1);
        EXPECT_EQ(displayFrame->getActuals().presentTime, 60);
        EXPECT_EQ(displayFrame->getFramePresentMetadata(), FramePresentMetadata::UnknownPresent);
        EXPECT_EQ(displayFrame->getFrameReadyMetadata(), FrameReadyMetadata::UnknownFinish);
        EXPECT_EQ(displayFrame->getJankType(), JankType::Unknown | JankType::DisplayHAL);
        EXPECT_EQ(displayFrame->getJankSeverityType(), JankSeverityType::Unknown);
    }
    {
        auto displayFrame = getDisplayFrame(2);
        EXPECT_EQ(displayFrame->getActuals().presentTime, 80);
        EXPECT_EQ(displayFrame->getFramePresentMetadata(), FramePresentMetadata::OnTimePresent);
        EXPECT_EQ(displayFrame->getFrameReadyMetadata(), FrameReadyMetadata::OnTimeFinish);
        EXPECT_EQ(displayFrame->getJankType(), JankType::None);
        EXPECT_EQ(displayFrame->getJankSeverityType(), JankSeverityType::None);
    }
}

TEST_F(FrameTimelineTest, computeFps_noLayerIds_returnsZero) {
    EXPECT_EQ(mFrameTimeline->computeFps({}), 0.0f);
}

TEST_F(FrameTimelineTest, computeFps_singleDisplayFrame_returnsZero) {
    const auto oneHundredMs = std::chrono::nanoseconds(100ms).count();

    auto surfaceFrame1 =
            mFrameTimeline->createSurfaceFrameForToken(FrameTimelineInfo(), sPidOne, sUidOne,
                                                       sLayerIdOne, sLayerNameOne, sLayerNameOne,
                                                       /*isBuffer*/ true, sGameMode);
    auto presentFence1 = fenceFactory.createFenceTimeForTest(Fence::NO_FENCE);
    surfaceFrame1->setPresentState(SurfaceFrame::PresentState::Presented);
    mFrameTimeline->addSurfaceFrame(surfaceFrame1);
    presentFence1->signalForTest(oneHundredMs);
    mFrameTimeline->setSfPresent(oneHundredMs, presentFence1);

    EXPECT_EQ(mFrameTimeline->computeFps({sLayerIdOne}), 0.0f);
}

TEST_F(FrameTimelineTest, computeFps_twoDisplayFrames_oneLayer) {
    const auto oneHundredMs = std::chrono::nanoseconds(100ms).count();
    const auto twoHundredMs = std::chrono::nanoseconds(200ms).count();
    auto surfaceFrame1 =
            mFrameTimeline->createSurfaceFrameForToken(FrameTimelineInfo(), sPidOne, sUidOne,
                                                       sLayerIdOne, sLayerNameOne, sLayerNameOne,
                                                       /*isBuffer*/ true, sGameMode);
    auto presentFence1 = fenceFactory.createFenceTimeForTest(Fence::NO_FENCE);
    surfaceFrame1->setPresentState(SurfaceFrame::PresentState::Presented);
    mFrameTimeline->addSurfaceFrame(surfaceFrame1);
    presentFence1->signalForTest(oneHundredMs);
    mFrameTimeline->setSfPresent(oneHundredMs, presentFence1);

    auto surfaceFrame2 =
            mFrameTimeline->createSurfaceFrameForToken(FrameTimelineInfo(), sPidOne, sUidOne,
                                                       sLayerIdOne, sLayerNameOne, sLayerNameOne,
                                                       /*isBuffer*/ true, sGameMode);
    auto presentFence2 = fenceFactory.createFenceTimeForTest(Fence::NO_FENCE);
    surfaceFrame2->setPresentState(SurfaceFrame::PresentState::Presented);
    mFrameTimeline->addSurfaceFrame(surfaceFrame2);
    presentFence2->signalForTest(twoHundredMs);
    mFrameTimeline->setSfPresent(twoHundredMs, presentFence2);

    EXPECT_EQ(mFrameTimeline->computeFps({sLayerIdOne}), 10.0);
}

TEST_F(FrameTimelineTest, computeFps_twoDisplayFrames_twoLayers) {
    const auto oneHundredMs = std::chrono::nanoseconds(100ms).count();
    const auto twoHundredMs = std::chrono::nanoseconds(200ms).count();
    auto surfaceFrame1 =
            mFrameTimeline->createSurfaceFrameForToken(FrameTimelineInfo(), sPidOne, sUidOne,
                                                       sLayerIdOne, sLayerNameOne, sLayerNameOne,
                                                       /*isBuffer*/ true, sGameMode);
    auto presentFence1 = fenceFactory.createFenceTimeForTest(Fence::NO_FENCE);
    surfaceFrame1->setPresentState(SurfaceFrame::PresentState::Presented);
    mFrameTimeline->addSurfaceFrame(surfaceFrame1);
    presentFence1->signalForTest(oneHundredMs);
    mFrameTimeline->setSfPresent(oneHundredMs, presentFence1);

    auto surfaceFrame2 =
            mFrameTimeline->createSurfaceFrameForToken(FrameTimelineInfo(), sPidOne, sUidOne,
                                                       sLayerIdTwo, sLayerNameTwo, sLayerNameTwo,
                                                       /*isBuffer*/ true, sGameMode);
    auto presentFence2 = fenceFactory.createFenceTimeForTest(Fence::NO_FENCE);
    surfaceFrame2->setPresentState(SurfaceFrame::PresentState::Presented);
    mFrameTimeline->addSurfaceFrame(surfaceFrame2);
    presentFence2->signalForTest(twoHundredMs);
    mFrameTimeline->setSfPresent(twoHundredMs, presentFence2);

    EXPECT_EQ(mFrameTimeline->computeFps({sLayerIdOne, sLayerIdTwo}), 10.0f);
}

TEST_F(FrameTimelineTest, computeFps_filtersOutLayers) {
    const auto oneHundredMs = std::chrono::nanoseconds(100ms).count();
    const auto twoHundredMs = std::chrono::nanoseconds(200ms).count();
    auto surfaceFrame1 =
            mFrameTimeline->createSurfaceFrameForToken(FrameTimelineInfo(), sPidOne, sUidOne,
                                                       sLayerIdOne, sLayerNameOne, sLayerNameOne,
                                                       /*isBuffer*/ true, sGameMode);
    auto presentFence1 = fenceFactory.createFenceTimeForTest(Fence::NO_FENCE);
    surfaceFrame1->setPresentState(SurfaceFrame::PresentState::Presented);
    mFrameTimeline->addSurfaceFrame(surfaceFrame1);
    presentFence1->signalForTest(oneHundredMs);
    mFrameTimeline->setSfPresent(oneHundredMs, presentFence1);

    auto surfaceFrame2 =
            mFrameTimeline->createSurfaceFrameForToken(FrameTimelineInfo(), sPidOne, sUidOne,
                                                       sLayerIdTwo, sLayerNameTwo, sLayerNameTwo,
                                                       /*isBuffer*/ true, sGameMode);
    auto presentFence2 = fenceFactory.createFenceTimeForTest(Fence::NO_FENCE);
    surfaceFrame2->setPresentState(SurfaceFrame::PresentState::Presented);
    mFrameTimeline->addSurfaceFrame(surfaceFrame2);
    presentFence2->signalForTest(twoHundredMs);
    mFrameTimeline->setSfPresent(twoHundredMs, presentFence2);

    EXPECT_EQ(mFrameTimeline->computeFps({sLayerIdOne}), 0.0f);
}

TEST_F(FrameTimelineTest, computeFps_averagesOverMultipleFrames) {
    const auto oneHundredMs = std::chrono::nanoseconds(100ms).count();
    const auto twoHundredMs = std::chrono::nanoseconds(200ms).count();
    const auto threeHundredMs = std::chrono::nanoseconds(300ms).count();
    const auto fiveHundredMs = std::chrono::nanoseconds(500ms).count();
    const auto sixHundredMs = std::chrono::nanoseconds(600ms).count();
    auto surfaceFrame1 =
            mFrameTimeline->createSurfaceFrameForToken(FrameTimelineInfo(), sPidOne, sUidOne,
                                                       sLayerIdOne, sLayerNameOne, sLayerNameOne,
                                                       /*isBuffer*/ true, sGameMode);
    auto presentFence1 = fenceFactory.createFenceTimeForTest(Fence::NO_FENCE);
    surfaceFrame1->setPresentState(SurfaceFrame::PresentState::Presented);
    mFrameTimeline->addSurfaceFrame(surfaceFrame1);
    presentFence1->signalForTest(oneHundredMs);
    mFrameTimeline->setSfPresent(oneHundredMs, presentFence1);

    auto surfaceFrame2 =
            mFrameTimeline->createSurfaceFrameForToken(FrameTimelineInfo(), sPidOne, sUidOne,
                                                       sLayerIdOne, sLayerNameOne, sLayerNameOne,
                                                       /*isBuffer*/ true, sGameMode);
    auto presentFence2 = fenceFactory.createFenceTimeForTest(Fence::NO_FENCE);
    surfaceFrame2->setPresentState(SurfaceFrame::PresentState::Presented);
    mFrameTimeline->addSurfaceFrame(surfaceFrame2);
    presentFence2->signalForTest(twoHundredMs);
    mFrameTimeline->setSfPresent(twoHundredMs, presentFence2);

    auto surfaceFrame3 =
            mFrameTimeline->createSurfaceFrameForToken(FrameTimelineInfo(), sPidOne, sUidOne,
                                                       sLayerIdTwo, sLayerNameTwo, sLayerNameTwo,
                                                       /*isBuffer*/ true, sGameMode);
    auto presentFence3 = fenceFactory.createFenceTimeForTest(Fence::NO_FENCE);
    surfaceFrame3->setPresentState(SurfaceFrame::PresentState::Presented);
    mFrameTimeline->addSurfaceFrame(surfaceFrame3);
    presentFence3->signalForTest(threeHundredMs);
    mFrameTimeline->setSfPresent(threeHundredMs, presentFence3);

    auto surfaceFrame4 =
            mFrameTimeline->createSurfaceFrameForToken(FrameTimelineInfo(), sPidOne, sUidOne,
                                                       sLayerIdOne, sLayerNameOne, sLayerNameOne,
                                                       /*isBuffer*/ true, sGameMode);
    auto presentFence4 = fenceFactory.createFenceTimeForTest(Fence::NO_FENCE);
    surfaceFrame4->setPresentState(SurfaceFrame::PresentState::Presented);
    mFrameTimeline->addSurfaceFrame(surfaceFrame4);
    presentFence4->signalForTest(fiveHundredMs);
    mFrameTimeline->setSfPresent(fiveHundredMs, presentFence4);

    auto surfaceFrame5 =
            mFrameTimeline->createSurfaceFrameForToken(FrameTimelineInfo(), sPidOne, sUidOne,
                                                       sLayerIdOne, sLayerNameOne, sLayerNameOne,
                                                       /*isBuffer*/ true, sGameMode);
    auto presentFence5 = fenceFactory.createFenceTimeForTest(Fence::NO_FENCE);
    // Dropped frames will be excluded from fps computation
    surfaceFrame5->setPresentState(SurfaceFrame::PresentState::Dropped);
    mFrameTimeline->addSurfaceFrame(surfaceFrame5);
    presentFence5->signalForTest(sixHundredMs);
    mFrameTimeline->setSfPresent(sixHundredMs, presentFence5);

    EXPECT_EQ(mFrameTimeline->computeFps({sLayerIdOne}), 5.0f);
}

TEST_F(FrameTimelineTest, getMinTime) {
    // Use SurfaceFrame::getBaseTime to test the getMinTime.
    FrameTimelineInfo ftInfo;

    // Valid prediction state test.
    ftInfo.vsyncId = 0L;
    mTokenManager->generateTokenForPredictions({10});
    auto surfaceFrame =
            mFrameTimeline->createSurfaceFrameForToken(ftInfo, sPidOne, sUidOne, sLayerIdOne,
                                                       sLayerNameOne, sLayerNameOne,
                                                       /*isBuffer*/ true, sGameMode);
    ASSERT_EQ(surfaceFrame->getBaseTime(), 10);

    // Test prediction state which is not valid.
    ftInfo.vsyncId = FrameTimelineInfo::INVALID_VSYNC_ID;
    surfaceFrame = mFrameTimeline->createSurfaceFrameForToken(ftInfo, sPidOne, sUidOne, sLayerIdOne,
                                                              sLayerNameOne, sLayerNameOne,
                                                              /*isBuffer*/ true, sGameMode);
    // Start time test.
    surfaceFrame->setActualStartTime(200);
    ASSERT_EQ(surfaceFrame->getBaseTime(), 200);

    // End time test.
    surfaceFrame->setAcquireFenceTime(100);
    ASSERT_EQ(surfaceFrame->getBaseTime(), 100);

    // Present time test.
    auto presentFence = fenceFactory.createFenceTimeForTest(Fence::NO_FENCE);
    surfaceFrame->setPresentState(SurfaceFrame::PresentState::Presented);
    mFrameTimeline->addSurfaceFrame(surfaceFrame);
    presentFence->signalForTest(std::chrono::nanoseconds(50ns).count());
    mFrameTimeline->setSfPresent(50, presentFence);
    ASSERT_EQ(surfaceFrame->getBaseTime(), 50);
}

TEST_F(FrameTimelineTest, surfaceFrameRenderRateUsingDisplayRate) {
    auto presentFence1 = fenceFactory.createFenceTimeForTest(Fence::NO_FENCE);
    int64_t token1 = mTokenManager->generateTokenForPredictions({10, 20, 30});
    FrameTimelineInfo ftInfo;
    ftInfo.vsyncId = token1;
    ftInfo.inputEventId = sInputEventId;
    auto surfaceFrame =
            mFrameTimeline->createSurfaceFrameForToken(ftInfo, sPidOne, sUidOne, sLayerIdOne,
                                                       sLayerNameOne, sLayerNameOne,
                                                       /*isBuffer*/ true, sGameMode);

    mFrameTimeline->setSfWakeUp(token1, 20, RR_30, RR_11);
    surfaceFrame->setPresentState(SurfaceFrame::PresentState::Presented);
    mFrameTimeline->addSurfaceFrame(surfaceFrame);
    presentFence1->signalForTest(std::chrono::nanoseconds(50ns).count());
    mFrameTimeline->setSfPresent(50, presentFence1);

    EXPECT_EQ(surfaceFrame->getRenderRate().getPeriodNsecs(), 11);
}

TEST_F(FrameTimelineTest, surfaceFrameRenderRateUsingAppFrameRate) {
    auto presentFence1 = fenceFactory.createFenceTimeForTest(Fence::NO_FENCE);
    int64_t token1 = mTokenManager->generateTokenForPredictions({10, 20, 30});
    FrameTimelineInfo ftInfo;
    ftInfo.vsyncId = token1;
    ftInfo.inputEventId = sInputEventId;
    auto surfaceFrame =
            mFrameTimeline->createSurfaceFrameForToken(ftInfo, sPidOne, sUidOne, sLayerIdOne,
                                                       sLayerNameOne, sLayerNameOne,
                                                       /*isBuffer*/ true, sGameMode);
    surfaceFrame->setRenderRate(RR_30);
    mFrameTimeline->setSfWakeUp(token1, 20, RR_11, RR_11);
    surfaceFrame->setPresentState(SurfaceFrame::PresentState::Presented);
    mFrameTimeline->addSurfaceFrame(surfaceFrame);
    presentFence1->signalForTest(std::chrono::nanoseconds(50ns).count());
    mFrameTimeline->setSfPresent(50, presentFence1);

    EXPECT_EQ(surfaceFrame->getRenderRate().getPeriodNsecs(), 30);
}
} // namespace android::frametimeline
