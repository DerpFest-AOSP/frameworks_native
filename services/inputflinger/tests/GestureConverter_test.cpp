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

#include <memory>

#include <com_android_input_flags.h>
#include <flag_macros.h>
#include <gestures/GestureConverter.h>
#include <gtest/gtest.h>
#include <gui/constants.h>

#include "FakeEventHub.h"
#include "FakeInputReaderPolicy.h"
#include "FakePointerController.h"
#include "InstrumentedInputReader.h"
#include "NotifyArgs.h"
#include "TestConstants.h"
#include "TestEventMatchers.h"
#include "TestInputListener.h"
#include "include/gestures.h"
#include "ui/Rotation.h"

namespace android {

namespace input_flags = com::android::input::flags;

namespace {

const auto TOUCHPAD_PALM_REJECTION =
        ACONFIG_FLAG(input_flags, enable_touchpad_typing_palm_rejection);

} // namespace

using testing::AllOf;

class GestureConverterTestBase : public testing::Test {
protected:
    static constexpr int32_t DEVICE_ID = END_RESERVED_ID + 1000;
    static constexpr int32_t EVENTHUB_ID = 1;
    static constexpr stime_t ARBITRARY_GESTURE_TIME = 1.2;
    static constexpr float POINTER_X = 500;
    static constexpr float POINTER_Y = 200;

    void SetUp() {
        mFakeEventHub = std::make_unique<FakeEventHub>();
        mFakePolicy = sp<FakeInputReaderPolicy>::make();
        mFakeListener = std::make_unique<TestInputListener>();
        mReader = std::make_unique<InstrumentedInputReader>(mFakeEventHub, mFakePolicy,
                                                            *mFakeListener);
        mDevice = newDevice();
        mFakeEventHub->addAbsoluteAxis(EVENTHUB_ID, ABS_MT_POSITION_X, -500, 500, 0, 0, 20);
        mFakeEventHub->addAbsoluteAxis(EVENTHUB_ID, ABS_MT_POSITION_Y, -500, 500, 0, 0, 20);

        mFakePointerController = std::make_shared<FakePointerController>();
        mFakePointerController->setBounds(0, 0, 800 - 1, 480 - 1);
        mFakePointerController->setPosition(POINTER_X, POINTER_Y);
        mFakePolicy->setPointerController(mFakePointerController);
    }

    std::shared_ptr<InputDevice> newDevice() {
        InputDeviceIdentifier identifier;
        identifier.name = "device";
        identifier.location = "USB1";
        identifier.bus = 0;
        std::shared_ptr<InputDevice> device =
                std::make_shared<InputDevice>(mReader->getContext(), DEVICE_ID, /* generation= */ 2,
                                              identifier);
        mReader->pushNextDevice(device);
        mFakeEventHub->addDevice(EVENTHUB_ID, identifier.name, InputDeviceClass::TOUCHPAD,
                                 identifier.bus);
        mReader->loopOnce();
        return device;
    }

    std::shared_ptr<FakeEventHub> mFakeEventHub;
    sp<FakeInputReaderPolicy> mFakePolicy;
    std::unique_ptr<TestInputListener> mFakeListener;
    std::unique_ptr<InstrumentedInputReader> mReader;
    std::shared_ptr<InputDevice> mDevice;
    std::shared_ptr<FakePointerController> mFakePointerController;
};

class GestureConverterTest : public GestureConverterTestBase {
protected:
    void SetUp() override {
        input_flags::enable_pointer_choreographer(false);
        GestureConverterTestBase::SetUp();
    }
};

TEST_F(GestureConverterTest, Move) {
    InputDeviceContext deviceContext(*mDevice, EVENTHUB_ID);
    GestureConverter converter(*mReader->getContext(), deviceContext, DEVICE_ID);
    converter.setDisplayId(ADISPLAY_ID_DEFAULT);

    Gesture moveGesture(kGestureMove, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME, -5, 10);
    std::list<NotifyArgs> args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, moveGesture);
    ASSERT_EQ(1u, args.size());

    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_HOVER_MOVE),
                      WithCoords(POINTER_X - 5, POINTER_Y + 10), WithRelativeMotion(-5, 10),
                      WithToolType(ToolType::FINGER), WithButtonState(0), WithPressure(0.0f),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));

    ASSERT_NO_FATAL_FAILURE(mFakePointerController->assertPosition(POINTER_X - 5, POINTER_Y + 10));
}

TEST_F(GestureConverterTest, Move_Rotated) {
    InputDeviceContext deviceContext(*mDevice, EVENTHUB_ID);
    GestureConverter converter(*mReader->getContext(), deviceContext, DEVICE_ID);
    converter.setOrientation(ui::ROTATION_90);
    converter.setDisplayId(ADISPLAY_ID_DEFAULT);

    Gesture moveGesture(kGestureMove, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME, -5, 10);
    std::list<NotifyArgs> args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, moveGesture);
    ASSERT_EQ(1u, args.size());

    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_HOVER_MOVE),
                      WithCoords(POINTER_X + 10, POINTER_Y + 5), WithRelativeMotion(10, 5),
                      WithToolType(ToolType::FINGER), WithButtonState(0), WithPressure(0.0f),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));

    ASSERT_NO_FATAL_FAILURE(mFakePointerController->assertPosition(POINTER_X + 10, POINTER_Y + 5));
}

TEST_F(GestureConverterTest, ButtonsChange) {
    InputDeviceContext deviceContext(*mDevice, EVENTHUB_ID);
    GestureConverter converter(*mReader->getContext(), deviceContext, DEVICE_ID);
    converter.setDisplayId(ADISPLAY_ID_DEFAULT);

    // Press left and right buttons at once
    Gesture downGesture(kGestureButtonsChange, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME,
                        /* down= */ GESTURES_BUTTON_LEFT | GESTURES_BUTTON_RIGHT,
                        /* up= */ GESTURES_BUTTON_NONE, /* is_tap= */ false);
    std::list<NotifyArgs> args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, downGesture);
    ASSERT_EQ(3u, args.size());

    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_DOWN),
                      WithButtonState(AMOTION_EVENT_BUTTON_PRIMARY |
                                      AMOTION_EVENT_BUTTON_SECONDARY),
                      WithCoords(POINTER_X, POINTER_Y), WithToolType(ToolType::FINGER),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));
    args.pop_front();
    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_BUTTON_PRESS),
                      WithActionButton(AMOTION_EVENT_BUTTON_PRIMARY),
                      WithButtonState(AMOTION_EVENT_BUTTON_PRIMARY),
                      WithCoords(POINTER_X, POINTER_Y), WithToolType(ToolType::FINGER),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));
    args.pop_front();
    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_BUTTON_PRESS),
                      WithActionButton(AMOTION_EVENT_BUTTON_SECONDARY),
                      WithButtonState(AMOTION_EVENT_BUTTON_PRIMARY |
                                      AMOTION_EVENT_BUTTON_SECONDARY),
                      WithCoords(POINTER_X, POINTER_Y), WithToolType(ToolType::FINGER),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));

    // Then release the left button
    Gesture leftUpGesture(kGestureButtonsChange, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME,
                          /* down= */ GESTURES_BUTTON_NONE, /* up= */ GESTURES_BUTTON_LEFT,
                          /* is_tap= */ false);
    args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, leftUpGesture);
    ASSERT_EQ(1u, args.size());

    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_BUTTON_RELEASE),
                      WithActionButton(AMOTION_EVENT_BUTTON_PRIMARY),
                      WithButtonState(AMOTION_EVENT_BUTTON_SECONDARY),
                      WithCoords(POINTER_X, POINTER_Y), WithToolType(ToolType::FINGER),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));

    // Finally release the right button
    Gesture rightUpGesture(kGestureButtonsChange, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME,
                           /* down= */ GESTURES_BUTTON_NONE, /* up= */ GESTURES_BUTTON_RIGHT,
                           /* is_tap= */ false);
    args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, rightUpGesture);
    ASSERT_EQ(3u, args.size());

    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_BUTTON_RELEASE),
                      WithActionButton(AMOTION_EVENT_BUTTON_SECONDARY), WithButtonState(0),
                      WithCoords(POINTER_X, POINTER_Y), WithToolType(ToolType::FINGER),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));
    args.pop_front();
    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_UP), WithButtonState(0),
                      WithCoords(POINTER_X, POINTER_Y), WithToolType(ToolType::FINGER),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));
    args.pop_front();
    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_HOVER_MOVE), WithButtonState(0),
                      WithCoords(POINTER_X, POINTER_Y), WithToolType(ToolType::FINGER),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));
}

TEST_F(GestureConverterTest, DragWithButton) {
    InputDeviceContext deviceContext(*mDevice, EVENTHUB_ID);
    GestureConverter converter(*mReader->getContext(), deviceContext, DEVICE_ID);
    converter.setDisplayId(ADISPLAY_ID_DEFAULT);

    // Press the button
    Gesture downGesture(kGestureButtonsChange, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME,
                        /* down= */ GESTURES_BUTTON_LEFT, /* up= */ GESTURES_BUTTON_NONE,
                        /* is_tap= */ false);
    std::list<NotifyArgs> args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, downGesture);
    ASSERT_EQ(2u, args.size());

    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_DOWN),
                      WithButtonState(AMOTION_EVENT_BUTTON_PRIMARY),
                      WithCoords(POINTER_X, POINTER_Y), WithToolType(ToolType::FINGER),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));
    args.pop_front();
    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_BUTTON_PRESS),
                      WithActionButton(AMOTION_EVENT_BUTTON_PRIMARY),
                      WithButtonState(AMOTION_EVENT_BUTTON_PRIMARY),
                      WithCoords(POINTER_X, POINTER_Y), WithToolType(ToolType::FINGER),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));

    // Move
    Gesture moveGesture(kGestureMove, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME, -5, 10);
    args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, moveGesture);
    ASSERT_EQ(1u, args.size());

    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_MOVE),
                      WithCoords(POINTER_X - 5, POINTER_Y + 10), WithRelativeMotion(-5, 10),
                      WithToolType(ToolType::FINGER), WithButtonState(AMOTION_EVENT_BUTTON_PRIMARY),
                      WithPressure(1.0f), WithDisplayId(ADISPLAY_ID_DEFAULT)));

    ASSERT_NO_FATAL_FAILURE(mFakePointerController->assertPosition(POINTER_X - 5, POINTER_Y + 10));

    // Release the button
    Gesture upGesture(kGestureButtonsChange, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME,
                      /* down= */ GESTURES_BUTTON_NONE, /* up= */ GESTURES_BUTTON_LEFT,
                      /* is_tap= */ false);
    args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, upGesture);
    ASSERT_EQ(3u, args.size());

    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_BUTTON_RELEASE),
                      WithActionButton(AMOTION_EVENT_BUTTON_PRIMARY), WithButtonState(0),
                      WithCoords(POINTER_X - 5, POINTER_Y + 10), WithToolType(ToolType::FINGER),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));
    args.pop_front();
    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_UP), WithButtonState(0),
                      WithCoords(POINTER_X - 5, POINTER_Y + 10), WithToolType(ToolType::FINGER),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));
    args.pop_front();
    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_HOVER_MOVE), WithButtonState(0),
                      WithCoords(POINTER_X - 5, POINTER_Y + 10), WithToolType(ToolType::FINGER),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));
}

TEST_F(GestureConverterTest, Scroll) {
    const nsecs_t downTime = 12345;
    InputDeviceContext deviceContext(*mDevice, EVENTHUB_ID);
    GestureConverter converter(*mReader->getContext(), deviceContext, DEVICE_ID);
    converter.setDisplayId(ADISPLAY_ID_DEFAULT);

    Gesture startGesture(kGestureScroll, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME, 0, -10);
    std::list<NotifyArgs> args = converter.handleGesture(downTime, READ_TIME, startGesture);
    ASSERT_EQ(2u, args.size());

    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_DOWN), WithCoords(POINTER_X, POINTER_Y),
                      WithGestureScrollDistance(0, 0, EPSILON),
                      WithMotionClassification(MotionClassification::TWO_FINGER_SWIPE),
                      WithToolType(ToolType::FINGER), WithDownTime(downTime),
                      WithFlags(AMOTION_EVENT_FLAG_IS_GENERATED_GESTURE),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));
    args.pop_front();
    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_MOVE),
                      WithCoords(POINTER_X, POINTER_Y - 10),
                      WithGestureScrollDistance(0, 10, EPSILON),
                      WithMotionClassification(MotionClassification::TWO_FINGER_SWIPE),
                      WithToolType(ToolType::FINGER),
                      WithFlags(AMOTION_EVENT_FLAG_IS_GENERATED_GESTURE),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));

    Gesture continueGesture(kGestureScroll, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME, 0, -5);
    args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, continueGesture);
    ASSERT_EQ(1u, args.size());
    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_MOVE),
                      WithCoords(POINTER_X, POINTER_Y - 15),
                      WithGestureScrollDistance(0, 5, EPSILON),
                      WithMotionClassification(MotionClassification::TWO_FINGER_SWIPE),
                      WithToolType(ToolType::FINGER),
                      WithFlags(AMOTION_EVENT_FLAG_IS_GENERATED_GESTURE),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));

    Gesture flingGesture(kGestureFling, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME, 1, 1,
                         GESTURES_FLING_START);
    args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, flingGesture);
    ASSERT_EQ(1u, args.size());
    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_UP),
                      WithCoords(POINTER_X, POINTER_Y - 15),
                      WithGestureScrollDistance(0, 0, EPSILON),
                      WithMotionClassification(MotionClassification::TWO_FINGER_SWIPE),
                      WithToolType(ToolType::FINGER),
                      WithFlags(AMOTION_EVENT_FLAG_IS_GENERATED_GESTURE),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));
}

TEST_F(GestureConverterTest, Scroll_Rotated) {
    const nsecs_t downTime = 12345;
    InputDeviceContext deviceContext(*mDevice, EVENTHUB_ID);
    GestureConverter converter(*mReader->getContext(), deviceContext, DEVICE_ID);
    converter.setOrientation(ui::ROTATION_90);
    converter.setDisplayId(ADISPLAY_ID_DEFAULT);

    Gesture startGesture(kGestureScroll, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME, 0, -10);
    std::list<NotifyArgs> args = converter.handleGesture(downTime, READ_TIME, startGesture);
    ASSERT_EQ(2u, args.size());

    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_DOWN), WithCoords(POINTER_X, POINTER_Y),
                      WithGestureScrollDistance(0, 0, EPSILON),
                      WithMotionClassification(MotionClassification::TWO_FINGER_SWIPE),
                      WithToolType(ToolType::FINGER), WithDownTime(downTime),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));
    args.pop_front();
    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_MOVE),
                      WithCoords(POINTER_X - 10, POINTER_Y),
                      WithGestureScrollDistance(0, 10, EPSILON),
                      WithMotionClassification(MotionClassification::TWO_FINGER_SWIPE),
                      WithToolType(ToolType::FINGER), WithDisplayId(ADISPLAY_ID_DEFAULT)));

    Gesture continueGesture(kGestureScroll, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME, 0, -5);
    args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, continueGesture);
    ASSERT_EQ(1u, args.size());
    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_MOVE),
                      WithCoords(POINTER_X - 15, POINTER_Y),
                      WithGestureScrollDistance(0, 5, EPSILON),
                      WithMotionClassification(MotionClassification::TWO_FINGER_SWIPE),
                      WithToolType(ToolType::FINGER), WithDisplayId(ADISPLAY_ID_DEFAULT)));

    Gesture flingGesture(kGestureFling, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME, 1, 1,
                         GESTURES_FLING_START);
    args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, flingGesture);
    ASSERT_EQ(1u, args.size());
    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_UP),
                      WithCoords(POINTER_X - 15, POINTER_Y),
                      WithGestureScrollDistance(0, 0, EPSILON),
                      WithMotionClassification(MotionClassification::TWO_FINGER_SWIPE),
                      WithToolType(ToolType::FINGER), WithDisplayId(ADISPLAY_ID_DEFAULT)));
}

TEST_F(GestureConverterTest, Scroll_ClearsClassificationAfterGesture) {
    InputDeviceContext deviceContext(*mDevice, EVENTHUB_ID);
    GestureConverter converter(*mReader->getContext(), deviceContext, DEVICE_ID);
    converter.setDisplayId(ADISPLAY_ID_DEFAULT);

    Gesture startGesture(kGestureScroll, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME, 0, -10);
    std::list<NotifyArgs> args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, startGesture);

    Gesture continueGesture(kGestureScroll, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME, 0, -5);
    args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, continueGesture);

    Gesture flingGesture(kGestureFling, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME, 1, 1,
                         GESTURES_FLING_START);
    args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, flingGesture);

    Gesture moveGesture(kGestureMove, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME, -5, 10);
    args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, moveGesture);
    ASSERT_EQ(1u, args.size());
    EXPECT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionClassification(MotionClassification::NONE),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));
}

TEST_F(GestureConverterTest, Scroll_ClearsScrollDistanceAfterGesture) {
    InputDeviceContext deviceContext(*mDevice, EVENTHUB_ID);
    GestureConverter converter(*mReader->getContext(), deviceContext, DEVICE_ID);
    converter.setDisplayId(ADISPLAY_ID_DEFAULT);

    Gesture startGesture(kGestureScroll, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME, 0, -10);
    std::list<NotifyArgs> args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, startGesture);

    Gesture continueGesture(kGestureScroll, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME, 0, -5);
    args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, continueGesture);

    Gesture flingGesture(kGestureFling, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME, 1, 1,
                         GESTURES_FLING_START);
    args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, flingGesture);

    // Move gestures don't use the fake finger array, so to test that gesture axes are cleared we
    // need to use another gesture type, like pinch.
    Gesture pinchGesture(kGesturePinch, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME, /*dz=*/1,
                         GESTURES_ZOOM_START);
    args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, pinchGesture);
    ASSERT_FALSE(args.empty());
    EXPECT_THAT(std::get<NotifyMotionArgs>(args.front()), WithGestureScrollDistance(0, 0, EPSILON));
}

TEST_F(GestureConverterTest, ThreeFingerSwipe_ClearsClassificationAfterGesture) {
    InputDeviceContext deviceContext(*mDevice, EVENTHUB_ID);
    GestureConverter converter(*mReader->getContext(), deviceContext, DEVICE_ID);
    converter.setDisplayId(ADISPLAY_ID_DEFAULT);

    Gesture startGesture(kGestureSwipe, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME, /*dx=*/0,
                         /*dy=*/0);
    std::list<NotifyArgs> args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, startGesture);

    Gesture liftGesture(kGestureSwipeLift, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME);
    args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, liftGesture);

    Gesture moveGesture(kGestureMove, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME, /*dx=*/-5,
                        /*dy=*/10);
    args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, moveGesture);
    ASSERT_EQ(1u, args.size());
    EXPECT_THAT(std::get<NotifyMotionArgs>(args.front()),
                WithMotionClassification(MotionClassification::NONE));
}

TEST_F(GestureConverterTest, ThreeFingerSwipe_ClearsGestureAxesAfterGesture) {
    InputDeviceContext deviceContext(*mDevice, EVENTHUB_ID);
    GestureConverter converter(*mReader->getContext(), deviceContext, DEVICE_ID);
    converter.setDisplayId(ADISPLAY_ID_DEFAULT);

    Gesture startGesture(kGestureSwipe, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME, /*dx=*/5,
                         /*dy=*/5);
    std::list<NotifyArgs> args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, startGesture);

    Gesture liftGesture(kGestureSwipeLift, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME);
    args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, liftGesture);

    // Move gestures don't use the fake finger array, so to test that gesture axes are cleared we
    // need to use another gesture type, like pinch.
    Gesture pinchGesture(kGesturePinch, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME, /*dz=*/1,
                         GESTURES_ZOOM_START);
    args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, pinchGesture);
    ASSERT_FALSE(args.empty());
    EXPECT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithGestureOffset(0, 0, EPSILON), WithGestureSwipeFingerCount(0)));
}

TEST_F(GestureConverterTest, ThreeFingerSwipe_Vertical) {
    // The gestures library will "lock" a swipe into the dimension it starts in. For example, if you
    // start swiping up and then start moving left or right, it'll return gesture events with only Y
    // deltas until you lift your fingers and start swiping again. That's why each of these tests
    // only checks movement in one dimension.
    InputDeviceContext deviceContext(*mDevice, EVENTHUB_ID);
    GestureConverter converter(*mReader->getContext(), deviceContext, DEVICE_ID);
    converter.setDisplayId(ADISPLAY_ID_DEFAULT);

    Gesture startGesture(kGestureSwipe, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME, /* dx= */ 0,
                         /* dy= */ 10);
    std::list<NotifyArgs> args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, startGesture);
    ASSERT_EQ(4u, args.size());

    // Three fake fingers should be created. We don't actually care where they are, so long as they
    // move appropriately.
    NotifyMotionArgs arg = std::get<NotifyMotionArgs>(args.front());
    ASSERT_THAT(arg,
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_DOWN), WithGestureOffset(0, 0, EPSILON),
                      WithGestureSwipeFingerCount(3),
                      WithMotionClassification(MotionClassification::MULTI_FINGER_SWIPE),
                      WithPointerCount(1u), WithToolType(ToolType::FINGER),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));
    PointerCoords finger0Start = arg.pointerCoords[0];
    args.pop_front();
    arg = std::get<NotifyMotionArgs>(args.front());
    ASSERT_THAT(arg,
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_POINTER_DOWN |
                                       1 << AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT),
                      WithGestureOffset(0, 0, EPSILON), WithGestureSwipeFingerCount(3),
                      WithMotionClassification(MotionClassification::MULTI_FINGER_SWIPE),
                      WithPointerCount(2u), WithToolType(ToolType::FINGER),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));
    PointerCoords finger1Start = arg.pointerCoords[1];
    args.pop_front();
    arg = std::get<NotifyMotionArgs>(args.front());
    ASSERT_THAT(arg,
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_POINTER_DOWN |
                                       2 << AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT),
                      WithGestureOffset(0, 0, EPSILON), WithGestureSwipeFingerCount(3),
                      WithMotionClassification(MotionClassification::MULTI_FINGER_SWIPE),
                      WithPointerCount(3u), WithToolType(ToolType::FINGER),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));
    PointerCoords finger2Start = arg.pointerCoords[2];
    args.pop_front();

    arg = std::get<NotifyMotionArgs>(args.front());
    ASSERT_THAT(arg,
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_MOVE),
                      WithGestureOffset(0, -0.01, EPSILON), WithGestureSwipeFingerCount(3),
                      WithMotionClassification(MotionClassification::MULTI_FINGER_SWIPE),
                      WithPointerCount(3u), WithToolType(ToolType::FINGER),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));
    EXPECT_EQ(arg.pointerCoords[0].getX(), finger0Start.getX());
    EXPECT_EQ(arg.pointerCoords[1].getX(), finger1Start.getX());
    EXPECT_EQ(arg.pointerCoords[2].getX(), finger2Start.getX());
    EXPECT_EQ(arg.pointerCoords[0].getY(), finger0Start.getY() - 10);
    EXPECT_EQ(arg.pointerCoords[1].getY(), finger1Start.getY() - 10);
    EXPECT_EQ(arg.pointerCoords[2].getY(), finger2Start.getY() - 10);

    Gesture continueGesture(kGestureSwipe, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME,
                            /* dx= */ 0, /* dy= */ 5);
    args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, continueGesture);
    ASSERT_EQ(1u, args.size());
    arg = std::get<NotifyMotionArgs>(args.front());
    ASSERT_THAT(arg,
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_MOVE),
                      WithGestureOffset(0, -0.005, EPSILON), WithGestureSwipeFingerCount(3),
                      WithMotionClassification(MotionClassification::MULTI_FINGER_SWIPE),
                      WithPointerCount(3u), WithToolType(ToolType::FINGER),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));
    EXPECT_EQ(arg.pointerCoords[0].getX(), finger0Start.getX());
    EXPECT_EQ(arg.pointerCoords[1].getX(), finger1Start.getX());
    EXPECT_EQ(arg.pointerCoords[2].getX(), finger2Start.getX());
    EXPECT_EQ(arg.pointerCoords[0].getY(), finger0Start.getY() - 15);
    EXPECT_EQ(arg.pointerCoords[1].getY(), finger1Start.getY() - 15);
    EXPECT_EQ(arg.pointerCoords[2].getY(), finger2Start.getY() - 15);

    Gesture liftGesture(kGestureSwipeLift, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME);
    args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, liftGesture);
    ASSERT_EQ(3u, args.size());
    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_POINTER_UP |
                                       2 << AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT),
                      WithGestureOffset(0, 0, EPSILON), WithGestureSwipeFingerCount(3),
                      WithMotionClassification(MotionClassification::MULTI_FINGER_SWIPE),
                      WithPointerCount(3u), WithToolType(ToolType::FINGER),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));
    args.pop_front();
    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_POINTER_UP |
                                       1 << AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT),
                      WithGestureOffset(0, 0, EPSILON), WithGestureSwipeFingerCount(3),
                      WithMotionClassification(MotionClassification::MULTI_FINGER_SWIPE),
                      WithPointerCount(2u), WithToolType(ToolType::FINGER),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));
    args.pop_front();
    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_UP), WithGestureOffset(0, 0, EPSILON),
                      WithGestureSwipeFingerCount(3),
                      WithMotionClassification(MotionClassification::MULTI_FINGER_SWIPE),
                      WithPointerCount(1u), WithToolType(ToolType::FINGER),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));
}

TEST_F(GestureConverterTest, ThreeFingerSwipe_Rotated) {
    InputDeviceContext deviceContext(*mDevice, EVENTHUB_ID);
    GestureConverter converter(*mReader->getContext(), deviceContext, DEVICE_ID);
    converter.setOrientation(ui::ROTATION_90);
    converter.setDisplayId(ADISPLAY_ID_DEFAULT);

    Gesture startGesture(kGestureSwipe, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME, /* dx= */ 0,
                         /* dy= */ 10);
    std::list<NotifyArgs> args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, startGesture);
    ASSERT_EQ(4u, args.size());

    // Three fake fingers should be created. We don't actually care where they are, so long as they
    // move appropriately.
    NotifyMotionArgs arg = std::get<NotifyMotionArgs>(args.front());
    ASSERT_THAT(arg,
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_DOWN), WithGestureOffset(0, 0, EPSILON),
                      WithPointerCount(1u), WithDisplayId(ADISPLAY_ID_DEFAULT)));
    PointerCoords finger0Start = arg.pointerCoords[0];
    args.pop_front();
    arg = std::get<NotifyMotionArgs>(args.front());
    ASSERT_THAT(arg,
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_POINTER_DOWN |
                                       1 << AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT),
                      WithGestureOffset(0, 0, EPSILON), WithPointerCount(2u),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));
    PointerCoords finger1Start = arg.pointerCoords[1];
    args.pop_front();
    arg = std::get<NotifyMotionArgs>(args.front());
    ASSERT_THAT(arg,
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_POINTER_DOWN |
                                       2 << AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT),
                      WithGestureOffset(0, 0, EPSILON), WithPointerCount(3u),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));
    PointerCoords finger2Start = arg.pointerCoords[2];
    args.pop_front();

    arg = std::get<NotifyMotionArgs>(args.front());
    ASSERT_THAT(arg,
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_MOVE),
                      WithGestureOffset(0, -0.01, EPSILON), WithPointerCount(3u),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));
    EXPECT_EQ(arg.pointerCoords[0].getX(), finger0Start.getX() - 10);
    EXPECT_EQ(arg.pointerCoords[1].getX(), finger1Start.getX() - 10);
    EXPECT_EQ(arg.pointerCoords[2].getX(), finger2Start.getX() - 10);
    EXPECT_EQ(arg.pointerCoords[0].getY(), finger0Start.getY());
    EXPECT_EQ(arg.pointerCoords[1].getY(), finger1Start.getY());
    EXPECT_EQ(arg.pointerCoords[2].getY(), finger2Start.getY());

    Gesture continueGesture(kGestureSwipe, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME,
                            /* dx= */ 0, /* dy= */ 5);
    args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, continueGesture);
    ASSERT_EQ(1u, args.size());
    arg = std::get<NotifyMotionArgs>(args.front());
    ASSERT_THAT(arg,
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_MOVE),
                      WithGestureOffset(0, -0.005, EPSILON), WithPointerCount(3u),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));
    EXPECT_EQ(arg.pointerCoords[0].getX(), finger0Start.getX() - 15);
    EXPECT_EQ(arg.pointerCoords[1].getX(), finger1Start.getX() - 15);
    EXPECT_EQ(arg.pointerCoords[2].getX(), finger2Start.getX() - 15);
    EXPECT_EQ(arg.pointerCoords[0].getY(), finger0Start.getY());
    EXPECT_EQ(arg.pointerCoords[1].getY(), finger1Start.getY());
    EXPECT_EQ(arg.pointerCoords[2].getY(), finger2Start.getY());

    Gesture liftGesture(kGestureSwipeLift, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME);
    args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, liftGesture);
    ASSERT_EQ(3u, args.size());
    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_POINTER_UP |
                                       2 << AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT),
                      WithGestureOffset(0, 0, EPSILON), WithPointerCount(3u),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));
    args.pop_front();
    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_POINTER_UP |
                                       1 << AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT),
                      WithGestureOffset(0, 0, EPSILON), WithPointerCount(2u),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));
    args.pop_front();
    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_UP), WithGestureOffset(0, 0, EPSILON),
                      WithPointerCount(1u), WithDisplayId(ADISPLAY_ID_DEFAULT)));
}

TEST_F(GestureConverterTest, FourFingerSwipe_Horizontal) {
    InputDeviceContext deviceContext(*mDevice, EVENTHUB_ID);
    GestureConverter converter(*mReader->getContext(), deviceContext, DEVICE_ID);
    converter.setDisplayId(ADISPLAY_ID_DEFAULT);

    Gesture startGesture(kGestureFourFingerSwipe, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME,
                         /* dx= */ 10, /* dy= */ 0);
    std::list<NotifyArgs> args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, startGesture);
    ASSERT_EQ(5u, args.size());

    // Four fake fingers should be created. We don't actually care where they are, so long as they
    // move appropriately.
    NotifyMotionArgs arg = std::get<NotifyMotionArgs>(args.front());
    ASSERT_THAT(arg,
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_DOWN), WithGestureOffset(0, 0, EPSILON),
                      WithGestureSwipeFingerCount(4),
                      WithMotionClassification(MotionClassification::MULTI_FINGER_SWIPE),
                      WithPointerCount(1u), WithToolType(ToolType::FINGER),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));
    PointerCoords finger0Start = arg.pointerCoords[0];
    args.pop_front();
    arg = std::get<NotifyMotionArgs>(args.front());
    ASSERT_THAT(arg,
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_POINTER_DOWN |
                                       1 << AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT),
                      WithGestureOffset(0, 0, EPSILON), WithGestureSwipeFingerCount(4),
                      WithMotionClassification(MotionClassification::MULTI_FINGER_SWIPE),
                      WithPointerCount(2u), WithToolType(ToolType::FINGER),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));
    PointerCoords finger1Start = arg.pointerCoords[1];
    args.pop_front();
    arg = std::get<NotifyMotionArgs>(args.front());
    ASSERT_THAT(arg,
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_POINTER_DOWN |
                                       2 << AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT),
                      WithGestureOffset(0, 0, EPSILON), WithGestureSwipeFingerCount(4),
                      WithMotionClassification(MotionClassification::MULTI_FINGER_SWIPE),
                      WithPointerCount(3u), WithToolType(ToolType::FINGER),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));
    PointerCoords finger2Start = arg.pointerCoords[2];
    args.pop_front();
    arg = std::get<NotifyMotionArgs>(args.front());
    ASSERT_THAT(arg,
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_POINTER_DOWN |
                                       3 << AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT),
                      WithGestureOffset(0, 0, EPSILON), WithGestureSwipeFingerCount(4),
                      WithMotionClassification(MotionClassification::MULTI_FINGER_SWIPE),
                      WithPointerCount(4u), WithToolType(ToolType::FINGER),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));
    PointerCoords finger3Start = arg.pointerCoords[3];
    args.pop_front();

    arg = std::get<NotifyMotionArgs>(args.front());
    ASSERT_THAT(arg,
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_MOVE),
                      WithGestureOffset(0.01, 0, EPSILON), WithGestureSwipeFingerCount(4),
                      WithMotionClassification(MotionClassification::MULTI_FINGER_SWIPE),
                      WithPointerCount(4u), WithToolType(ToolType::FINGER),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));
    EXPECT_EQ(arg.pointerCoords[0].getX(), finger0Start.getX() + 10);
    EXPECT_EQ(arg.pointerCoords[1].getX(), finger1Start.getX() + 10);
    EXPECT_EQ(arg.pointerCoords[2].getX(), finger2Start.getX() + 10);
    EXPECT_EQ(arg.pointerCoords[3].getX(), finger3Start.getX() + 10);
    EXPECT_EQ(arg.pointerCoords[0].getY(), finger0Start.getY());
    EXPECT_EQ(arg.pointerCoords[1].getY(), finger1Start.getY());
    EXPECT_EQ(arg.pointerCoords[2].getY(), finger2Start.getY());
    EXPECT_EQ(arg.pointerCoords[3].getY(), finger3Start.getY());

    Gesture continueGesture(kGestureFourFingerSwipe, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME,
                            /* dx= */ 5, /* dy= */ 0);
    args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, continueGesture);
    ASSERT_EQ(1u, args.size());
    arg = std::get<NotifyMotionArgs>(args.front());
    ASSERT_THAT(arg,
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_MOVE),
                      WithGestureOffset(0.005, 0, EPSILON), WithGestureSwipeFingerCount(4),
                      WithMotionClassification(MotionClassification::MULTI_FINGER_SWIPE),
                      WithPointerCount(4u), WithToolType(ToolType::FINGER),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));
    EXPECT_EQ(arg.pointerCoords[0].getX(), finger0Start.getX() + 15);
    EXPECT_EQ(arg.pointerCoords[1].getX(), finger1Start.getX() + 15);
    EXPECT_EQ(arg.pointerCoords[2].getX(), finger2Start.getX() + 15);
    EXPECT_EQ(arg.pointerCoords[3].getX(), finger3Start.getX() + 15);
    EXPECT_EQ(arg.pointerCoords[0].getY(), finger0Start.getY());
    EXPECT_EQ(arg.pointerCoords[1].getY(), finger1Start.getY());
    EXPECT_EQ(arg.pointerCoords[2].getY(), finger2Start.getY());
    EXPECT_EQ(arg.pointerCoords[3].getY(), finger3Start.getY());

    Gesture liftGesture(kGestureSwipeLift, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME);
    args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, liftGesture);
    ASSERT_EQ(4u, args.size());
    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_POINTER_UP |
                                       3 << AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT),
                      WithGestureOffset(0, 0, EPSILON), WithGestureSwipeFingerCount(4),
                      WithMotionClassification(MotionClassification::MULTI_FINGER_SWIPE),
                      WithPointerCount(4u), WithToolType(ToolType::FINGER),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));
    args.pop_front();
    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_POINTER_UP |
                                       2 << AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT),
                      WithGestureOffset(0, 0, EPSILON), WithGestureSwipeFingerCount(4),
                      WithMotionClassification(MotionClassification::MULTI_FINGER_SWIPE),
                      WithPointerCount(3u), WithToolType(ToolType::FINGER),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));
    args.pop_front();
    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_POINTER_UP |
                                       1 << AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT),
                      WithGestureOffset(0, 0, EPSILON), WithGestureSwipeFingerCount(4),
                      WithMotionClassification(MotionClassification::MULTI_FINGER_SWIPE),
                      WithPointerCount(2u), WithToolType(ToolType::FINGER),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));
    args.pop_front();
    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_UP), WithGestureOffset(0, 0, EPSILON),
                      WithGestureSwipeFingerCount(4),
                      WithMotionClassification(MotionClassification::MULTI_FINGER_SWIPE),
                      WithPointerCount(1u), WithToolType(ToolType::FINGER),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));
}

TEST_F(GestureConverterTest, Pinch_Inwards) {
    InputDeviceContext deviceContext(*mDevice, EVENTHUB_ID);
    GestureConverter converter(*mReader->getContext(), deviceContext, DEVICE_ID);
    converter.setDisplayId(ADISPLAY_ID_DEFAULT);

    Gesture startGesture(kGesturePinch, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME, /* dz= */ 1,
                         GESTURES_ZOOM_START);
    std::list<NotifyArgs> args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, startGesture);
    ASSERT_EQ(2u, args.size());
    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_DOWN),
                      WithMotionClassification(MotionClassification::PINCH),
                      WithGesturePinchScaleFactor(1.0f, EPSILON),
                      WithCoords(POINTER_X - 100, POINTER_Y), WithPointerCount(1u),
                      WithToolType(ToolType::FINGER), WithDisplayId(ADISPLAY_ID_DEFAULT)));
    args.pop_front();
    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_POINTER_DOWN |
                                       1 << AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT),
                      WithMotionClassification(MotionClassification::PINCH),
                      WithGesturePinchScaleFactor(1.0f, EPSILON),
                      WithPointerCoords(1, POINTER_X + 100, POINTER_Y), WithPointerCount(2u),
                      WithToolType(ToolType::FINGER), WithDisplayId(ADISPLAY_ID_DEFAULT)));

    Gesture updateGesture(kGesturePinch, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME,
                          /* dz= */ 0.8, GESTURES_ZOOM_UPDATE);
    args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, updateGesture);
    ASSERT_EQ(1u, args.size());
    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_MOVE),
                      WithMotionClassification(MotionClassification::PINCH),
                      WithGesturePinchScaleFactor(0.8f, EPSILON),
                      WithPointerCoords(0, POINTER_X - 80, POINTER_Y),
                      WithPointerCoords(1, POINTER_X + 80, POINTER_Y), WithPointerCount(2u),
                      WithToolType(ToolType::FINGER), WithDisplayId(ADISPLAY_ID_DEFAULT)));

    Gesture endGesture(kGesturePinch, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME, /* dz= */ 1,
                       GESTURES_ZOOM_END);
    args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, endGesture);
    ASSERT_EQ(2u, args.size());
    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_POINTER_UP |
                                       1 << AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT),
                      WithMotionClassification(MotionClassification::PINCH),
                      WithGesturePinchScaleFactor(1.0f, EPSILON), WithPointerCount(2u),
                      WithToolType(ToolType::FINGER), WithDisplayId(ADISPLAY_ID_DEFAULT)));
    args.pop_front();
    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_UP),
                      WithMotionClassification(MotionClassification::PINCH),
                      WithGesturePinchScaleFactor(1.0f, EPSILON), WithPointerCount(1u),
                      WithToolType(ToolType::FINGER), WithDisplayId(ADISPLAY_ID_DEFAULT)));
}

TEST_F(GestureConverterTest, Pinch_Outwards) {
    InputDeviceContext deviceContext(*mDevice, EVENTHUB_ID);
    GestureConverter converter(*mReader->getContext(), deviceContext, DEVICE_ID);
    converter.setDisplayId(ADISPLAY_ID_DEFAULT);

    Gesture startGesture(kGesturePinch, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME, /* dz= */ 1,
                         GESTURES_ZOOM_START);
    std::list<NotifyArgs> args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, startGesture);
    ASSERT_EQ(2u, args.size());
    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_DOWN),
                      WithMotionClassification(MotionClassification::PINCH),
                      WithGesturePinchScaleFactor(1.0f, EPSILON),
                      WithCoords(POINTER_X - 100, POINTER_Y), WithPointerCount(1u),
                      WithToolType(ToolType::FINGER), WithDisplayId(ADISPLAY_ID_DEFAULT)));
    args.pop_front();
    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_POINTER_DOWN |
                                       1 << AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT),
                      WithMotionClassification(MotionClassification::PINCH),
                      WithGesturePinchScaleFactor(1.0f, EPSILON),
                      WithPointerCoords(1, POINTER_X + 100, POINTER_Y), WithPointerCount(2u),
                      WithToolType(ToolType::FINGER), WithDisplayId(ADISPLAY_ID_DEFAULT)));

    Gesture updateGesture(kGesturePinch, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME,
                          /* dz= */ 1.2, GESTURES_ZOOM_UPDATE);
    args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, updateGesture);
    ASSERT_EQ(1u, args.size());
    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_MOVE),
                      WithMotionClassification(MotionClassification::PINCH),
                      WithGesturePinchScaleFactor(1.2f, EPSILON),
                      WithPointerCoords(0, POINTER_X - 120, POINTER_Y),
                      WithPointerCoords(1, POINTER_X + 120, POINTER_Y), WithPointerCount(2u),
                      WithToolType(ToolType::FINGER), WithDisplayId(ADISPLAY_ID_DEFAULT)));

    Gesture endGesture(kGesturePinch, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME, /* dz= */ 1,
                       GESTURES_ZOOM_END);
    args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, endGesture);
    ASSERT_EQ(2u, args.size());
    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_POINTER_UP |
                                       1 << AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT),
                      WithMotionClassification(MotionClassification::PINCH),
                      WithGesturePinchScaleFactor(1.0f, EPSILON), WithPointerCount(2u),
                      WithToolType(ToolType::FINGER), WithDisplayId(ADISPLAY_ID_DEFAULT)));
    args.pop_front();
    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_UP),
                      WithMotionClassification(MotionClassification::PINCH),
                      WithGesturePinchScaleFactor(1.0f, EPSILON), WithPointerCount(1u),
                      WithToolType(ToolType::FINGER), WithDisplayId(ADISPLAY_ID_DEFAULT)));
}

TEST_F(GestureConverterTest, Pinch_ClearsClassificationAfterGesture) {
    InputDeviceContext deviceContext(*mDevice, EVENTHUB_ID);
    GestureConverter converter(*mReader->getContext(), deviceContext, DEVICE_ID);
    converter.setDisplayId(ADISPLAY_ID_DEFAULT);

    Gesture startGesture(kGesturePinch, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME, /*dz=*/1,
                         GESTURES_ZOOM_START);
    std::list<NotifyArgs> args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, startGesture);

    Gesture updateGesture(kGesturePinch, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME,
                          /*dz=*/1.2, GESTURES_ZOOM_UPDATE);
    args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, updateGesture);

    Gesture endGesture(kGesturePinch, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME, /*dz=*/1,
                       GESTURES_ZOOM_END);
    args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, endGesture);

    Gesture moveGesture(kGestureMove, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME, -5, 10);
    args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, moveGesture);
    ASSERT_EQ(1u, args.size());
    EXPECT_THAT(std::get<NotifyMotionArgs>(args.front()),
                WithMotionClassification(MotionClassification::NONE));
}

TEST_F(GestureConverterTest, Pinch_ClearsScaleFactorAfterGesture) {
    InputDeviceContext deviceContext(*mDevice, EVENTHUB_ID);
    GestureConverter converter(*mReader->getContext(), deviceContext, DEVICE_ID);
    converter.setDisplayId(ADISPLAY_ID_DEFAULT);

    Gesture startGesture(kGesturePinch, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME, /*dz=*/1,
                         GESTURES_ZOOM_START);
    std::list<NotifyArgs> args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, startGesture);

    Gesture updateGesture(kGesturePinch, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME,
                          /*dz=*/1.2, GESTURES_ZOOM_UPDATE);
    args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, updateGesture);

    Gesture endGesture(kGesturePinch, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME, /*dz=*/1,
                       GESTURES_ZOOM_END);
    args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, endGesture);

    // Move gestures don't use the fake finger array, so to test that gesture axes are cleared we
    // need to use another gesture type, like scroll.
    Gesture scrollGesture(kGestureScroll, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME, /*dx=*/1,
                          /*dy=*/0);
    args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, scrollGesture);
    ASSERT_FALSE(args.empty());
    EXPECT_THAT(std::get<NotifyMotionArgs>(args.front()), WithGesturePinchScaleFactor(0, EPSILON));
}

TEST_F(GestureConverterTest, ResetWithButtonPressed) {
    InputDeviceContext deviceContext(*mDevice, EVENTHUB_ID);
    GestureConverter converter(*mReader->getContext(), deviceContext, DEVICE_ID);
    converter.setDisplayId(ADISPLAY_ID_DEFAULT);

    Gesture downGesture(kGestureButtonsChange, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME,
                        /*down=*/GESTURES_BUTTON_LEFT | GESTURES_BUTTON_RIGHT,
                        /*up=*/GESTURES_BUTTON_NONE, /*is_tap=*/false);
    (void)converter.handleGesture(ARBITRARY_TIME, READ_TIME, downGesture);

    std::list<NotifyArgs> args = converter.reset(ARBITRARY_TIME);
    ASSERT_EQ(3u, args.size());

    EXPECT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_BUTTON_RELEASE),
                      WithActionButton(AMOTION_EVENT_BUTTON_PRIMARY),
                      WithButtonState(AMOTION_EVENT_BUTTON_SECONDARY),
                      WithCoords(POINTER_X, POINTER_Y), WithToolType(ToolType::FINGER),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));
    args.pop_front();
    EXPECT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_BUTTON_RELEASE),
                      WithActionButton(AMOTION_EVENT_BUTTON_SECONDARY), WithButtonState(0),
                      WithCoords(POINTER_X, POINTER_Y), WithToolType(ToolType::FINGER),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));
    args.pop_front();
    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_UP), WithButtonState(0),
                      WithCoords(POINTER_X, POINTER_Y), WithToolType(ToolType::FINGER),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));
}

TEST_F(GestureConverterTest, ResetDuringScroll) {
    InputDeviceContext deviceContext(*mDevice, EVENTHUB_ID);
    GestureConverter converter(*mReader->getContext(), deviceContext, DEVICE_ID);
    converter.setDisplayId(ADISPLAY_ID_DEFAULT);

    Gesture startGesture(kGestureScroll, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME, 0, -10);
    (void)converter.handleGesture(ARBITRARY_TIME, READ_TIME, startGesture);

    std::list<NotifyArgs> args = converter.reset(ARBITRARY_TIME);
    ASSERT_EQ(1u, args.size());
    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_UP),
                      WithCoords(POINTER_X, POINTER_Y - 10),
                      WithGestureScrollDistance(0, 0, EPSILON),
                      WithMotionClassification(MotionClassification::TWO_FINGER_SWIPE),
                      WithToolType(ToolType::FINGER),
                      WithFlags(AMOTION_EVENT_FLAG_IS_GENERATED_GESTURE),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));
}

TEST_F(GestureConverterTest, ResetDuringThreeFingerSwipe) {
    InputDeviceContext deviceContext(*mDevice, EVENTHUB_ID);
    GestureConverter converter(*mReader->getContext(), deviceContext, DEVICE_ID);
    converter.setDisplayId(ADISPLAY_ID_DEFAULT);

    Gesture startGesture(kGestureSwipe, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME, /*dx=*/0,
                         /*dy=*/10);
    (void)converter.handleGesture(ARBITRARY_TIME, READ_TIME, startGesture);

    std::list<NotifyArgs> args = converter.reset(ARBITRARY_TIME);
    ASSERT_EQ(3u, args.size());
    EXPECT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_POINTER_UP |
                                       2 << AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT),
                      WithGestureOffset(0, 0, EPSILON),
                      WithMotionClassification(MotionClassification::MULTI_FINGER_SWIPE),
                      WithPointerCount(3u), WithToolType(ToolType::FINGER),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));
    args.pop_front();
    EXPECT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_POINTER_UP |
                                       1 << AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT),
                      WithGestureOffset(0, 0, EPSILON),
                      WithMotionClassification(MotionClassification::MULTI_FINGER_SWIPE),
                      WithPointerCount(2u), WithToolType(ToolType::FINGER),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));
    args.pop_front();
    EXPECT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_UP), WithGestureOffset(0, 0, EPSILON),
                      WithMotionClassification(MotionClassification::MULTI_FINGER_SWIPE),
                      WithPointerCount(1u), WithToolType(ToolType::FINGER),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));
}

TEST_F(GestureConverterTest, ResetDuringPinch) {
    InputDeviceContext deviceContext(*mDevice, EVENTHUB_ID);
    GestureConverter converter(*mReader->getContext(), deviceContext, DEVICE_ID);
    converter.setDisplayId(ADISPLAY_ID_DEFAULT);

    Gesture startGesture(kGesturePinch, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME, /*dz=*/1,
                         GESTURES_ZOOM_START);
    (void)converter.handleGesture(ARBITRARY_TIME, READ_TIME, startGesture);

    std::list<NotifyArgs> args = converter.reset(ARBITRARY_TIME);
    ASSERT_EQ(2u, args.size());
    EXPECT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_POINTER_UP |
                                       1 << AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT),
                      WithMotionClassification(MotionClassification::PINCH),
                      WithGesturePinchScaleFactor(1.0f, EPSILON), WithPointerCount(2u),
                      WithToolType(ToolType::FINGER), WithDisplayId(ADISPLAY_ID_DEFAULT)));
    args.pop_front();
    EXPECT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_UP),
                      WithMotionClassification(MotionClassification::PINCH),
                      WithGesturePinchScaleFactor(1.0f, EPSILON), WithPointerCount(1u),
                      WithToolType(ToolType::FINGER), WithDisplayId(ADISPLAY_ID_DEFAULT)));
}

TEST_F(GestureConverterTest, FlingTapDown) {
    InputDeviceContext deviceContext(*mDevice, EVENTHUB_ID);
    GestureConverter converter(*mReader->getContext(), deviceContext, DEVICE_ID);
    converter.setDisplayId(ADISPLAY_ID_DEFAULT);

    Gesture tapDownGesture(kGestureFling, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME,
                           /*vx=*/0.f, /*vy=*/0.f, GESTURES_FLING_TAP_DOWN);
    std::list<NotifyArgs> args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, tapDownGesture);

    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_HOVER_MOVE),
                      WithCoords(POINTER_X, POINTER_Y), WithRelativeMotion(0.f, 0.f),
                      WithToolType(ToolType::FINGER), WithButtonState(0), WithPressure(0.0f),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));

    ASSERT_NO_FATAL_FAILURE(mFakePointerController->assertPosition(POINTER_X, POINTER_Y));
    ASSERT_TRUE(mFakePointerController->isPointerShown());
}

TEST_F(GestureConverterTest, Tap) {
    // Tap should produce button press/release events
    InputDeviceContext deviceContext(*mDevice, EVENTHUB_ID);
    GestureConverter converter(*mReader->getContext(), deviceContext, DEVICE_ID);
    converter.setDisplayId(ADISPLAY_ID_DEFAULT);

    Gesture flingGesture(kGestureFling, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME, /* vx= */ 0,
                         /* vy= */ 0, GESTURES_FLING_TAP_DOWN);
    std::list<NotifyArgs> args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, flingGesture);

    ASSERT_EQ(1u, args.size());
    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_HOVER_MOVE),
                      WithCoords(POINTER_X, POINTER_Y), WithRelativeMotion(0, 0),
                      WithToolType(ToolType::FINGER), WithButtonState(0), WithPressure(0.0f),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));

    Gesture tapGesture(kGestureButtonsChange, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME,
                       /* down= */ GESTURES_BUTTON_LEFT,
                       /* up= */ GESTURES_BUTTON_LEFT, /* is_tap= */ true);
    args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, tapGesture);

    ASSERT_EQ(5u, args.size());
    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_DOWN), WithCoords(POINTER_X, POINTER_Y),
                      WithRelativeMotion(0.f, 0.f), WithToolType(ToolType::FINGER),
                      WithButtonState(AMOTION_EVENT_BUTTON_PRIMARY), WithPressure(1.0f),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));
    args.pop_front();
    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_BUTTON_PRESS),
                      WithActionButton(AMOTION_EVENT_BUTTON_PRIMARY),
                      WithButtonState(AMOTION_EVENT_BUTTON_PRIMARY),
                      WithCoords(POINTER_X, POINTER_Y), WithRelativeMotion(0.f, 0.f),
                      WithToolType(ToolType::FINGER), WithButtonState(AMOTION_EVENT_BUTTON_PRIMARY),
                      WithPressure(1.0f), WithDisplayId(ADISPLAY_ID_DEFAULT)));
    args.pop_front();
    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_BUTTON_RELEASE),
                      WithActionButton(AMOTION_EVENT_BUTTON_PRIMARY), WithButtonState(0),
                      WithCoords(POINTER_X, POINTER_Y), WithRelativeMotion(0.f, 0.f),
                      WithToolType(ToolType::FINGER), WithButtonState(0), WithPressure(1.0f),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));
    args.pop_front();
    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_UP), WithCoords(POINTER_X, POINTER_Y),
                      WithRelativeMotion(0.f, 0.f), WithToolType(ToolType::FINGER),
                      WithButtonState(0), WithPressure(0.0f), WithDisplayId(ADISPLAY_ID_DEFAULT)));
    args.pop_front();
    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_HOVER_MOVE),
                      WithCoords(POINTER_X, POINTER_Y), WithRelativeMotion(0, 0),
                      WithToolType(ToolType::FINGER), WithButtonState(0), WithPressure(0.0f),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));
}

TEST_F(GestureConverterTest, Click) {
    // Click should produce button press/release events
    InputDeviceContext deviceContext(*mDevice, EVENTHUB_ID);
    GestureConverter converter(*mReader->getContext(), deviceContext, DEVICE_ID);
    converter.setDisplayId(ADISPLAY_ID_DEFAULT);

    Gesture flingGesture(kGestureFling, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME, /* vx= */ 0,
                         /* vy= */ 0, GESTURES_FLING_TAP_DOWN);
    std::list<NotifyArgs> args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, flingGesture);

    ASSERT_EQ(1u, args.size());
    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_HOVER_MOVE),
                      WithCoords(POINTER_X, POINTER_Y), WithRelativeMotion(0, 0),
                      WithToolType(ToolType::FINGER), WithButtonState(0), WithPressure(0.0f),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));

    Gesture buttonDownGesture(kGestureButtonsChange, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME,
                              /* down= */ GESTURES_BUTTON_LEFT,
                              /* up= */ GESTURES_BUTTON_NONE, /* is_tap= */ false);
    args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, buttonDownGesture);

    ASSERT_EQ(2u, args.size());
    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_DOWN), WithCoords(POINTER_X, POINTER_Y),
                      WithRelativeMotion(0.f, 0.f), WithToolType(ToolType::FINGER),
                      WithButtonState(AMOTION_EVENT_BUTTON_PRIMARY), WithPressure(1.0f),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));
    args.pop_front();
    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_BUTTON_PRESS),
                      WithActionButton(AMOTION_EVENT_BUTTON_PRIMARY),
                      WithButtonState(AMOTION_EVENT_BUTTON_PRIMARY),
                      WithCoords(POINTER_X, POINTER_Y), WithRelativeMotion(0.f, 0.f),
                      WithToolType(ToolType::FINGER), WithButtonState(AMOTION_EVENT_BUTTON_PRIMARY),
                      WithPressure(1.0f), WithDisplayId(ADISPLAY_ID_DEFAULT)));

    Gesture buttonUpGesture(kGestureButtonsChange, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME,
                            /* down= */ GESTURES_BUTTON_NONE,
                            /* up= */ GESTURES_BUTTON_LEFT, /* is_tap= */ false);
    args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, buttonUpGesture);

    ASSERT_EQ(3u, args.size());
    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_BUTTON_RELEASE),
                      WithActionButton(AMOTION_EVENT_BUTTON_PRIMARY), WithButtonState(0),
                      WithCoords(POINTER_X, POINTER_Y), WithRelativeMotion(0.f, 0.f),
                      WithToolType(ToolType::FINGER), WithButtonState(0), WithPressure(1.0f),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));
    args.pop_front();
    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_UP), WithCoords(POINTER_X, POINTER_Y),
                      WithRelativeMotion(0.f, 0.f), WithToolType(ToolType::FINGER),
                      WithButtonState(0), WithPressure(0.0f), WithDisplayId(ADISPLAY_ID_DEFAULT)));
    args.pop_front();
    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_HOVER_MOVE),
                      WithCoords(POINTER_X, POINTER_Y), WithRelativeMotion(0, 0),
                      WithToolType(ToolType::FINGER), WithButtonState(0), WithPressure(0.0f),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));
}

TEST_F_WITH_FLAGS(GestureConverterTest, TapWithTapToClickDisabled,
                  REQUIRES_FLAGS_ENABLED(TOUCHPAD_PALM_REJECTION)) {
    // Tap should be ignored when disabled
    mReader->getContext()->setPreventingTouchpadTaps(true);

    InputDeviceContext deviceContext(*mDevice, EVENTHUB_ID);
    GestureConverter converter(*mReader->getContext(), deviceContext, DEVICE_ID);
    converter.setDisplayId(ADISPLAY_ID_DEFAULT);

    Gesture flingGesture(kGestureFling, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME, /* vx= */ 0,
                         /* vy= */ 0, GESTURES_FLING_TAP_DOWN);
    std::list<NotifyArgs> args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, flingGesture);

    ASSERT_EQ(1u, args.size());
    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_HOVER_MOVE),
                      WithCoords(POINTER_X, POINTER_Y), WithRelativeMotion(0, 0),
                      WithToolType(ToolType::FINGER), WithButtonState(0), WithPressure(0.0f),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));
    args.pop_front();

    Gesture tapGesture(kGestureButtonsChange, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME,
                       /* down= */ GESTURES_BUTTON_LEFT,
                       /* up= */ GESTURES_BUTTON_LEFT, /* is_tap= */ true);
    args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, tapGesture);

    // no events should be generated
    ASSERT_EQ(0u, args.size());

    // Future taps should be re-enabled
    ASSERT_FALSE(mReader->getContext()->isPreventingTouchpadTaps());
}

TEST_F_WITH_FLAGS(GestureConverterTest, ClickWithTapToClickDisabled,
                  REQUIRES_FLAGS_ENABLED(TOUCHPAD_PALM_REJECTION)) {
    // Click should still produce button press/release events
    mReader->getContext()->setPreventingTouchpadTaps(true);

    InputDeviceContext deviceContext(*mDevice, EVENTHUB_ID);
    GestureConverter converter(*mReader->getContext(), deviceContext, DEVICE_ID);
    converter.setDisplayId(ADISPLAY_ID_DEFAULT);

    Gesture flingGesture(kGestureFling, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME, /* vx= */ 0,
                         /* vy= */ 0, GESTURES_FLING_TAP_DOWN);
    std::list<NotifyArgs> args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, flingGesture);

    ASSERT_EQ(1u, args.size());
    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_HOVER_MOVE),
                      WithCoords(POINTER_X, POINTER_Y), WithRelativeMotion(0, 0),
                      WithToolType(ToolType::FINGER), WithButtonState(0), WithPressure(0.0f),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));

    Gesture buttonDownGesture(kGestureButtonsChange, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME,
                              /* down= */ GESTURES_BUTTON_LEFT,
                              /* up= */ GESTURES_BUTTON_NONE, /* is_tap= */ false);
    args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, buttonDownGesture);
    ASSERT_EQ(2u, args.size());

    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_DOWN), WithCoords(POINTER_X, POINTER_Y),
                      WithRelativeMotion(0.f, 0.f), WithToolType(ToolType::FINGER),
                      WithButtonState(AMOTION_EVENT_BUTTON_PRIMARY), WithPressure(1.0f),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));
    args.pop_front();
    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_BUTTON_PRESS),
                      WithActionButton(AMOTION_EVENT_BUTTON_PRIMARY),
                      WithButtonState(AMOTION_EVENT_BUTTON_PRIMARY),
                      WithCoords(POINTER_X, POINTER_Y), WithRelativeMotion(0.f, 0.f),
                      WithToolType(ToolType::FINGER), WithButtonState(AMOTION_EVENT_BUTTON_PRIMARY),
                      WithPressure(1.0f), WithDisplayId(ADISPLAY_ID_DEFAULT)));

    Gesture buttonUpGesture(kGestureButtonsChange, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME,
                            /* down= */ GESTURES_BUTTON_NONE,
                            /* up= */ GESTURES_BUTTON_LEFT, /* is_tap= */ false);
    args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, buttonUpGesture);

    ASSERT_EQ(3u, args.size());
    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_BUTTON_RELEASE),
                      WithActionButton(AMOTION_EVENT_BUTTON_PRIMARY), WithButtonState(0),
                      WithCoords(POINTER_X, POINTER_Y), WithRelativeMotion(0.f, 0.f),
                      WithToolType(ToolType::FINGER), WithButtonState(0), WithPressure(1.0f),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));
    args.pop_front();
    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_UP), WithCoords(POINTER_X, POINTER_Y),
                      WithRelativeMotion(0.f, 0.f), WithToolType(ToolType::FINGER),
                      WithButtonState(0), WithPressure(0.0f), WithDisplayId(ADISPLAY_ID_DEFAULT)));
    args.pop_front();
    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_HOVER_MOVE),
                      WithCoords(POINTER_X, POINTER_Y), WithRelativeMotion(0, 0),
                      WithToolType(ToolType::FINGER), WithButtonState(0), WithPressure(0.0f),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));

    // Future taps should be re-enabled
    ASSERT_FALSE(mReader->getContext()->isPreventingTouchpadTaps());
}

TEST_F_WITH_FLAGS(GestureConverterTest, MoveEnablesTapToClick,
                  REQUIRES_FLAGS_ENABLED(TOUCHPAD_PALM_REJECTION)) {
    // initially disable tap-to-click
    mReader->getContext()->setPreventingTouchpadTaps(true);

    InputDeviceContext deviceContext(*mDevice, EVENTHUB_ID);
    GestureConverter converter(*mReader->getContext(), deviceContext, DEVICE_ID);
    converter.setDisplayId(ADISPLAY_ID_DEFAULT);

    Gesture moveGesture(kGestureMove, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME, -5, 10);
    std::list<NotifyArgs> args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, moveGesture);
    ASSERT_EQ(1u, args.size());

    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_HOVER_MOVE),
                      WithCoords(POINTER_X - 5, POINTER_Y + 10), WithRelativeMotion(-5, 10),
                      WithToolType(ToolType::FINGER), WithButtonState(0), WithPressure(0.0f),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));

    ASSERT_NO_FATAL_FAILURE(mFakePointerController->assertPosition(POINTER_X - 5, POINTER_Y + 10));

    // Future taps should be re-enabled
    ASSERT_FALSE(mReader->getContext()->isPreventingTouchpadTaps());
}

// TODO(b/311416205): De-duplicate the test cases after the refactoring is complete and the flagging
//   logic can be removed.
class GestureConverterTestWithChoreographer : public GestureConverterTestBase {
protected:
    void SetUp() override {
        input_flags::enable_pointer_choreographer(true);
        GestureConverterTestBase::SetUp();
    }
};

TEST_F(GestureConverterTestWithChoreographer, Move) {
    InputDeviceContext deviceContext(*mDevice, EVENTHUB_ID);
    GestureConverter converter(*mReader->getContext(), deviceContext, DEVICE_ID);
    converter.setDisplayId(ADISPLAY_ID_DEFAULT);

    Gesture moveGesture(kGestureMove, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME, -5, 10);
    std::list<NotifyArgs> args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, moveGesture);
    ASSERT_EQ(1u, args.size());

    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_HOVER_MOVE), WithCoords(0, 0),
                      WithRelativeMotion(-5, 10), WithToolType(ToolType::FINGER),
                      WithButtonState(0), WithPressure(0.0f), WithDisplayId(ADISPLAY_ID_DEFAULT)));
}

TEST_F(GestureConverterTestWithChoreographer, Move_Rotated) {
    InputDeviceContext deviceContext(*mDevice, EVENTHUB_ID);
    GestureConverter converter(*mReader->getContext(), deviceContext, DEVICE_ID);
    converter.setOrientation(ui::ROTATION_90);
    converter.setDisplayId(ADISPLAY_ID_DEFAULT);

    Gesture moveGesture(kGestureMove, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME, -5, 10);
    std::list<NotifyArgs> args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, moveGesture);
    ASSERT_EQ(1u, args.size());

    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_HOVER_MOVE), WithCoords(0, 0),
                      WithRelativeMotion(10, 5), WithToolType(ToolType::FINGER), WithButtonState(0),
                      WithPressure(0.0f), WithDisplayId(ADISPLAY_ID_DEFAULT)));
}

TEST_F(GestureConverterTestWithChoreographer, ButtonsChange) {
    InputDeviceContext deviceContext(*mDevice, EVENTHUB_ID);
    GestureConverter converter(*mReader->getContext(), deviceContext, DEVICE_ID);
    converter.setDisplayId(ADISPLAY_ID_DEFAULT);

    // Press left and right buttons at once
    Gesture downGesture(kGestureButtonsChange, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME,
                        /* down= */ GESTURES_BUTTON_LEFT | GESTURES_BUTTON_RIGHT,
                        /* up= */ GESTURES_BUTTON_NONE, /* is_tap= */ false);
    std::list<NotifyArgs> args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, downGesture);
    ASSERT_EQ(3u, args.size());

    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_DOWN),
                      WithButtonState(AMOTION_EVENT_BUTTON_PRIMARY |
                                      AMOTION_EVENT_BUTTON_SECONDARY),
                      WithCoords(0, 0), WithToolType(ToolType::FINGER),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));
    args.pop_front();
    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_BUTTON_PRESS),
                      WithActionButton(AMOTION_EVENT_BUTTON_PRIMARY),
                      WithButtonState(AMOTION_EVENT_BUTTON_PRIMARY), WithCoords(0, 0),
                      WithToolType(ToolType::FINGER), WithDisplayId(ADISPLAY_ID_DEFAULT)));
    args.pop_front();
    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_BUTTON_PRESS),
                      WithActionButton(AMOTION_EVENT_BUTTON_SECONDARY),
                      WithButtonState(AMOTION_EVENT_BUTTON_PRIMARY |
                                      AMOTION_EVENT_BUTTON_SECONDARY),
                      WithCoords(0, 0), WithToolType(ToolType::FINGER),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));

    // Then release the left button
    Gesture leftUpGesture(kGestureButtonsChange, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME,
                          /* down= */ GESTURES_BUTTON_NONE, /* up= */ GESTURES_BUTTON_LEFT,
                          /* is_tap= */ false);
    args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, leftUpGesture);
    ASSERT_EQ(1u, args.size());

    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_BUTTON_RELEASE),
                      WithActionButton(AMOTION_EVENT_BUTTON_PRIMARY),
                      WithButtonState(AMOTION_EVENT_BUTTON_SECONDARY), WithCoords(0, 0),
                      WithToolType(ToolType::FINGER), WithDisplayId(ADISPLAY_ID_DEFAULT)));

    // Finally release the right button
    Gesture rightUpGesture(kGestureButtonsChange, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME,
                           /* down= */ GESTURES_BUTTON_NONE, /* up= */ GESTURES_BUTTON_RIGHT,
                           /* is_tap= */ false);
    args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, rightUpGesture);
    ASSERT_EQ(3u, args.size());

    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_BUTTON_RELEASE),
                      WithActionButton(AMOTION_EVENT_BUTTON_SECONDARY), WithButtonState(0),
                      WithCoords(0, 0), WithToolType(ToolType::FINGER),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));
    args.pop_front();
    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_UP), WithButtonState(0),
                      WithCoords(0, 0), WithToolType(ToolType::FINGER),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));
    args.pop_front();
    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_HOVER_MOVE), WithButtonState(0),
                      WithCoords(0, 0), WithToolType(ToolType::FINGER),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));
}

TEST_F(GestureConverterTestWithChoreographer, DragWithButton) {
    InputDeviceContext deviceContext(*mDevice, EVENTHUB_ID);
    GestureConverter converter(*mReader->getContext(), deviceContext, DEVICE_ID);
    converter.setDisplayId(ADISPLAY_ID_DEFAULT);

    // Press the button
    Gesture downGesture(kGestureButtonsChange, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME,
                        /* down= */ GESTURES_BUTTON_LEFT, /* up= */ GESTURES_BUTTON_NONE,
                        /* is_tap= */ false);
    std::list<NotifyArgs> args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, downGesture);
    ASSERT_EQ(2u, args.size());

    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_DOWN),
                      WithButtonState(AMOTION_EVENT_BUTTON_PRIMARY), WithCoords(0, 0),
                      WithToolType(ToolType::FINGER), WithDisplayId(ADISPLAY_ID_DEFAULT)));
    args.pop_front();
    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_BUTTON_PRESS),
                      WithActionButton(AMOTION_EVENT_BUTTON_PRIMARY),
                      WithButtonState(AMOTION_EVENT_BUTTON_PRIMARY), WithCoords(0, 0),
                      WithToolType(ToolType::FINGER), WithDisplayId(ADISPLAY_ID_DEFAULT)));

    // Move
    Gesture moveGesture(kGestureMove, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME, -5, 10);
    args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, moveGesture);
    ASSERT_EQ(1u, args.size());

    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_MOVE), WithCoords(0, 0),
                      WithRelativeMotion(-5, 10), WithToolType(ToolType::FINGER),
                      WithButtonState(AMOTION_EVENT_BUTTON_PRIMARY), WithPressure(1.0f),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));

    // Release the button
    Gesture upGesture(kGestureButtonsChange, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME,
                      /* down= */ GESTURES_BUTTON_NONE, /* up= */ GESTURES_BUTTON_LEFT,
                      /* is_tap= */ false);
    args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, upGesture);
    ASSERT_EQ(3u, args.size());

    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_BUTTON_RELEASE),
                      WithActionButton(AMOTION_EVENT_BUTTON_PRIMARY), WithButtonState(0),
                      WithCoords(0, 0), WithToolType(ToolType::FINGER),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));
    args.pop_front();
    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_UP), WithButtonState(0),
                      WithCoords(0, 0), WithToolType(ToolType::FINGER),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));
    args.pop_front();
    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_HOVER_MOVE), WithButtonState(0),
                      WithCoords(0, 0), WithToolType(ToolType::FINGER),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));
}

TEST_F(GestureConverterTestWithChoreographer, Scroll) {
    const nsecs_t downTime = 12345;
    InputDeviceContext deviceContext(*mDevice, EVENTHUB_ID);
    GestureConverter converter(*mReader->getContext(), deviceContext, DEVICE_ID);
    converter.setDisplayId(ADISPLAY_ID_DEFAULT);

    Gesture startGesture(kGestureScroll, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME, 0, -10);
    std::list<NotifyArgs> args = converter.handleGesture(downTime, READ_TIME, startGesture);
    ASSERT_EQ(2u, args.size());

    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_DOWN), WithCoords(0, 0),
                      WithGestureScrollDistance(0, 0, EPSILON),
                      WithMotionClassification(MotionClassification::TWO_FINGER_SWIPE),
                      WithToolType(ToolType::FINGER), WithDownTime(downTime),
                      WithFlags(AMOTION_EVENT_FLAG_IS_GENERATED_GESTURE),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));
    args.pop_front();
    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_MOVE), WithCoords(0, -10),
                      WithGestureScrollDistance(0, 10, EPSILON),
                      WithMotionClassification(MotionClassification::TWO_FINGER_SWIPE),
                      WithToolType(ToolType::FINGER),
                      WithFlags(AMOTION_EVENT_FLAG_IS_GENERATED_GESTURE),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));

    Gesture continueGesture(kGestureScroll, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME, 0, -5);
    args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, continueGesture);
    ASSERT_EQ(1u, args.size());
    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_MOVE), WithCoords(0, -15),
                      WithGestureScrollDistance(0, 5, EPSILON),
                      WithMotionClassification(MotionClassification::TWO_FINGER_SWIPE),
                      WithToolType(ToolType::FINGER),
                      WithFlags(AMOTION_EVENT_FLAG_IS_GENERATED_GESTURE),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));

    Gesture flingGesture(kGestureFling, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME, 1, 1,
                         GESTURES_FLING_START);
    args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, flingGesture);
    ASSERT_EQ(1u, args.size());
    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_UP), WithCoords(0, 0 - 15),
                      WithGestureScrollDistance(0, 0, EPSILON),
                      WithMotionClassification(MotionClassification::TWO_FINGER_SWIPE),
                      WithToolType(ToolType::FINGER),
                      WithFlags(AMOTION_EVENT_FLAG_IS_GENERATED_GESTURE),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));
}

TEST_F(GestureConverterTestWithChoreographer, Scroll_Rotated) {
    const nsecs_t downTime = 12345;
    InputDeviceContext deviceContext(*mDevice, EVENTHUB_ID);
    GestureConverter converter(*mReader->getContext(), deviceContext, DEVICE_ID);
    converter.setOrientation(ui::ROTATION_90);
    converter.setDisplayId(ADISPLAY_ID_DEFAULT);

    Gesture startGesture(kGestureScroll, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME, 0, -10);
    std::list<NotifyArgs> args = converter.handleGesture(downTime, READ_TIME, startGesture);
    ASSERT_EQ(2u, args.size());

    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_DOWN), WithCoords(0, 0),
                      WithGestureScrollDistance(0, 0, EPSILON),
                      WithMotionClassification(MotionClassification::TWO_FINGER_SWIPE),
                      WithToolType(ToolType::FINGER), WithDownTime(downTime),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));
    args.pop_front();
    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_MOVE), WithCoords(-10, 0),
                      WithGestureScrollDistance(0, 10, EPSILON),
                      WithMotionClassification(MotionClassification::TWO_FINGER_SWIPE),
                      WithToolType(ToolType::FINGER), WithDisplayId(ADISPLAY_ID_DEFAULT)));

    Gesture continueGesture(kGestureScroll, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME, 0, -5);
    args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, continueGesture);
    ASSERT_EQ(1u, args.size());
    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_MOVE), WithCoords(-15, 0),
                      WithGestureScrollDistance(0, 5, EPSILON),
                      WithMotionClassification(MotionClassification::TWO_FINGER_SWIPE),
                      WithToolType(ToolType::FINGER), WithDisplayId(ADISPLAY_ID_DEFAULT)));

    Gesture flingGesture(kGestureFling, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME, 1, 1,
                         GESTURES_FLING_START);
    args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, flingGesture);
    ASSERT_EQ(1u, args.size());
    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_UP), WithCoords(-15, 0),
                      WithGestureScrollDistance(0, 0, EPSILON),
                      WithMotionClassification(MotionClassification::TWO_FINGER_SWIPE),
                      WithToolType(ToolType::FINGER), WithDisplayId(ADISPLAY_ID_DEFAULT)));
}

TEST_F(GestureConverterTestWithChoreographer, Scroll_ClearsClassificationAfterGesture) {
    InputDeviceContext deviceContext(*mDevice, EVENTHUB_ID);
    GestureConverter converter(*mReader->getContext(), deviceContext, DEVICE_ID);
    converter.setDisplayId(ADISPLAY_ID_DEFAULT);

    Gesture startGesture(kGestureScroll, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME, 0, -10);
    std::list<NotifyArgs> args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, startGesture);

    Gesture continueGesture(kGestureScroll, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME, 0, -5);
    args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, continueGesture);

    Gesture flingGesture(kGestureFling, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME, 1, 1,
                         GESTURES_FLING_START);
    args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, flingGesture);

    Gesture moveGesture(kGestureMove, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME, -5, 10);
    args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, moveGesture);
    ASSERT_EQ(1u, args.size());
    EXPECT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionClassification(MotionClassification::NONE),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));
}

TEST_F(GestureConverterTestWithChoreographer, Scroll_ClearsScrollDistanceAfterGesture) {
    InputDeviceContext deviceContext(*mDevice, EVENTHUB_ID);
    GestureConverter converter(*mReader->getContext(), deviceContext, DEVICE_ID);
    converter.setDisplayId(ADISPLAY_ID_DEFAULT);

    Gesture startGesture(kGestureScroll, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME, 0, -10);
    std::list<NotifyArgs> args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, startGesture);

    Gesture continueGesture(kGestureScroll, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME, 0, -5);
    args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, continueGesture);

    Gesture flingGesture(kGestureFling, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME, 1, 1,
                         GESTURES_FLING_START);
    args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, flingGesture);

    // Move gestures don't use the fake finger array, so to test that gesture axes are cleared we
    // need to use another gesture type, like pinch.
    Gesture pinchGesture(kGesturePinch, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME, /*dz=*/1,
                         GESTURES_ZOOM_START);
    args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, pinchGesture);
    ASSERT_FALSE(args.empty());
    EXPECT_THAT(std::get<NotifyMotionArgs>(args.front()), WithGestureScrollDistance(0, 0, EPSILON));
}

TEST_F(GestureConverterTestWithChoreographer, ThreeFingerSwipe_ClearsClassificationAfterGesture) {
    InputDeviceContext deviceContext(*mDevice, EVENTHUB_ID);
    GestureConverter converter(*mReader->getContext(), deviceContext, DEVICE_ID);
    converter.setDisplayId(ADISPLAY_ID_DEFAULT);

    Gesture startGesture(kGestureSwipe, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME, /*dx=*/0,
                         /*dy=*/0);
    std::list<NotifyArgs> args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, startGesture);

    Gesture liftGesture(kGestureSwipeLift, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME);
    args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, liftGesture);

    Gesture moveGesture(kGestureMove, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME, /*dx=*/-5,
                        /*dy=*/10);
    args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, moveGesture);
    ASSERT_EQ(1u, args.size());
    EXPECT_THAT(std::get<NotifyMotionArgs>(args.front()),
                WithMotionClassification(MotionClassification::NONE));
}

TEST_F(GestureConverterTestWithChoreographer, ThreeFingerSwipe_ClearsGestureAxesAfterGesture) {
    InputDeviceContext deviceContext(*mDevice, EVENTHUB_ID);
    GestureConverter converter(*mReader->getContext(), deviceContext, DEVICE_ID);
    converter.setDisplayId(ADISPLAY_ID_DEFAULT);

    Gesture startGesture(kGestureSwipe, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME, /*dx=*/5,
                         /*dy=*/5);
    std::list<NotifyArgs> args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, startGesture);

    Gesture liftGesture(kGestureSwipeLift, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME);
    args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, liftGesture);

    // Move gestures don't use the fake finger array, so to test that gesture axes are cleared we
    // need to use another gesture type, like pinch.
    Gesture pinchGesture(kGesturePinch, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME, /*dz=*/1,
                         GESTURES_ZOOM_START);
    args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, pinchGesture);
    ASSERT_FALSE(args.empty());
    EXPECT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithGestureOffset(0, 0, EPSILON), WithGestureSwipeFingerCount(0)));
}

TEST_F(GestureConverterTestWithChoreographer, ThreeFingerSwipe_Vertical) {
    // The gestures library will "lock" a swipe into the dimension it starts in. For example, if you
    // start swiping up and then start moving left or right, it'll return gesture events with only Y
    // deltas until you lift your fingers and start swiping again. That's why each of these tests
    // only checks movement in one dimension.
    InputDeviceContext deviceContext(*mDevice, EVENTHUB_ID);
    GestureConverter converter(*mReader->getContext(), deviceContext, DEVICE_ID);
    converter.setDisplayId(ADISPLAY_ID_DEFAULT);

    Gesture startGesture(kGestureSwipe, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME, /* dx= */ 0,
                         /* dy= */ 10);
    std::list<NotifyArgs> args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, startGesture);
    ASSERT_EQ(4u, args.size());

    // Three fake fingers should be created. We don't actually care where they are, so long as they
    // move appropriately.
    NotifyMotionArgs arg = std::get<NotifyMotionArgs>(args.front());
    ASSERT_THAT(arg,
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_DOWN), WithGestureOffset(0, 0, EPSILON),
                      WithGestureSwipeFingerCount(3),
                      WithMotionClassification(MotionClassification::MULTI_FINGER_SWIPE),
                      WithPointerCount(1u), WithToolType(ToolType::FINGER),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));
    PointerCoords finger0Start = arg.pointerCoords[0];
    args.pop_front();
    arg = std::get<NotifyMotionArgs>(args.front());
    ASSERT_THAT(arg,
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_POINTER_DOWN |
                                       1 << AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT),
                      WithGestureOffset(0, 0, EPSILON), WithGestureSwipeFingerCount(3),
                      WithMotionClassification(MotionClassification::MULTI_FINGER_SWIPE),
                      WithPointerCount(2u), WithToolType(ToolType::FINGER),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));
    PointerCoords finger1Start = arg.pointerCoords[1];
    args.pop_front();
    arg = std::get<NotifyMotionArgs>(args.front());
    ASSERT_THAT(arg,
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_POINTER_DOWN |
                                       2 << AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT),
                      WithGestureOffset(0, 0, EPSILON), WithGestureSwipeFingerCount(3),
                      WithMotionClassification(MotionClassification::MULTI_FINGER_SWIPE),
                      WithPointerCount(3u), WithToolType(ToolType::FINGER),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));
    PointerCoords finger2Start = arg.pointerCoords[2];
    args.pop_front();

    arg = std::get<NotifyMotionArgs>(args.front());
    ASSERT_THAT(arg,
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_MOVE),
                      WithGestureOffset(0, -0.01, EPSILON), WithGestureSwipeFingerCount(3),
                      WithMotionClassification(MotionClassification::MULTI_FINGER_SWIPE),
                      WithPointerCount(3u), WithToolType(ToolType::FINGER),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));
    EXPECT_EQ(arg.pointerCoords[0].getX(), finger0Start.getX());
    EXPECT_EQ(arg.pointerCoords[1].getX(), finger1Start.getX());
    EXPECT_EQ(arg.pointerCoords[2].getX(), finger2Start.getX());
    EXPECT_EQ(arg.pointerCoords[0].getY(), finger0Start.getY() - 10);
    EXPECT_EQ(arg.pointerCoords[1].getY(), finger1Start.getY() - 10);
    EXPECT_EQ(arg.pointerCoords[2].getY(), finger2Start.getY() - 10);

    Gesture continueGesture(kGestureSwipe, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME,
                            /* dx= */ 0, /* dy= */ 5);
    args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, continueGesture);
    ASSERT_EQ(1u, args.size());
    arg = std::get<NotifyMotionArgs>(args.front());
    ASSERT_THAT(arg,
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_MOVE),
                      WithGestureOffset(0, -0.005, EPSILON), WithGestureSwipeFingerCount(3),
                      WithMotionClassification(MotionClassification::MULTI_FINGER_SWIPE),
                      WithPointerCount(3u), WithToolType(ToolType::FINGER),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));
    EXPECT_EQ(arg.pointerCoords[0].getX(), finger0Start.getX());
    EXPECT_EQ(arg.pointerCoords[1].getX(), finger1Start.getX());
    EXPECT_EQ(arg.pointerCoords[2].getX(), finger2Start.getX());
    EXPECT_EQ(arg.pointerCoords[0].getY(), finger0Start.getY() - 15);
    EXPECT_EQ(arg.pointerCoords[1].getY(), finger1Start.getY() - 15);
    EXPECT_EQ(arg.pointerCoords[2].getY(), finger2Start.getY() - 15);

    Gesture liftGesture(kGestureSwipeLift, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME);
    args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, liftGesture);
    ASSERT_EQ(3u, args.size());
    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_POINTER_UP |
                                       2 << AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT),
                      WithGestureOffset(0, 0, EPSILON), WithGestureSwipeFingerCount(3),
                      WithMotionClassification(MotionClassification::MULTI_FINGER_SWIPE),
                      WithPointerCount(3u), WithToolType(ToolType::FINGER),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));
    args.pop_front();
    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_POINTER_UP |
                                       1 << AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT),
                      WithGestureOffset(0, 0, EPSILON), WithGestureSwipeFingerCount(3),
                      WithMotionClassification(MotionClassification::MULTI_FINGER_SWIPE),
                      WithPointerCount(2u), WithToolType(ToolType::FINGER),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));
    args.pop_front();
    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_UP), WithGestureOffset(0, 0, EPSILON),
                      WithGestureSwipeFingerCount(3),
                      WithMotionClassification(MotionClassification::MULTI_FINGER_SWIPE),
                      WithPointerCount(1u), WithToolType(ToolType::FINGER),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));
}

TEST_F(GestureConverterTestWithChoreographer, ThreeFingerSwipe_Rotated) {
    InputDeviceContext deviceContext(*mDevice, EVENTHUB_ID);
    GestureConverter converter(*mReader->getContext(), deviceContext, DEVICE_ID);
    converter.setOrientation(ui::ROTATION_90);
    converter.setDisplayId(ADISPLAY_ID_DEFAULT);

    Gesture startGesture(kGestureSwipe, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME, /* dx= */ 0,
                         /* dy= */ 10);
    std::list<NotifyArgs> args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, startGesture);
    ASSERT_EQ(4u, args.size());

    // Three fake fingers should be created. We don't actually care where they are, so long as they
    // move appropriately.
    NotifyMotionArgs arg = std::get<NotifyMotionArgs>(args.front());
    ASSERT_THAT(arg,
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_DOWN), WithGestureOffset(0, 0, EPSILON),
                      WithPointerCount(1u), WithDisplayId(ADISPLAY_ID_DEFAULT)));
    PointerCoords finger0Start = arg.pointerCoords[0];
    args.pop_front();
    arg = std::get<NotifyMotionArgs>(args.front());
    ASSERT_THAT(arg,
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_POINTER_DOWN |
                                       1 << AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT),
                      WithGestureOffset(0, 0, EPSILON), WithPointerCount(2u),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));
    PointerCoords finger1Start = arg.pointerCoords[1];
    args.pop_front();
    arg = std::get<NotifyMotionArgs>(args.front());
    ASSERT_THAT(arg,
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_POINTER_DOWN |
                                       2 << AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT),
                      WithGestureOffset(0, 0, EPSILON), WithPointerCount(3u),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));
    PointerCoords finger2Start = arg.pointerCoords[2];
    args.pop_front();

    arg = std::get<NotifyMotionArgs>(args.front());
    ASSERT_THAT(arg,
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_MOVE),
                      WithGestureOffset(0, -0.01, EPSILON), WithPointerCount(3u),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));
    EXPECT_EQ(arg.pointerCoords[0].getX(), finger0Start.getX() - 10);
    EXPECT_EQ(arg.pointerCoords[1].getX(), finger1Start.getX() - 10);
    EXPECT_EQ(arg.pointerCoords[2].getX(), finger2Start.getX() - 10);
    EXPECT_EQ(arg.pointerCoords[0].getY(), finger0Start.getY());
    EXPECT_EQ(arg.pointerCoords[1].getY(), finger1Start.getY());
    EXPECT_EQ(arg.pointerCoords[2].getY(), finger2Start.getY());

    Gesture continueGesture(kGestureSwipe, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME,
                            /* dx= */ 0, /* dy= */ 5);
    args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, continueGesture);
    ASSERT_EQ(1u, args.size());
    arg = std::get<NotifyMotionArgs>(args.front());
    ASSERT_THAT(arg,
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_MOVE),
                      WithGestureOffset(0, -0.005, EPSILON), WithPointerCount(3u),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));
    EXPECT_EQ(arg.pointerCoords[0].getX(), finger0Start.getX() - 15);
    EXPECT_EQ(arg.pointerCoords[1].getX(), finger1Start.getX() - 15);
    EXPECT_EQ(arg.pointerCoords[2].getX(), finger2Start.getX() - 15);
    EXPECT_EQ(arg.pointerCoords[0].getY(), finger0Start.getY());
    EXPECT_EQ(arg.pointerCoords[1].getY(), finger1Start.getY());
    EXPECT_EQ(arg.pointerCoords[2].getY(), finger2Start.getY());

    Gesture liftGesture(kGestureSwipeLift, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME);
    args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, liftGesture);
    ASSERT_EQ(3u, args.size());
    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_POINTER_UP |
                                       2 << AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT),
                      WithGestureOffset(0, 0, EPSILON), WithPointerCount(3u),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));
    args.pop_front();
    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_POINTER_UP |
                                       1 << AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT),
                      WithGestureOffset(0, 0, EPSILON), WithPointerCount(2u),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));
    args.pop_front();
    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_UP), WithGestureOffset(0, 0, EPSILON),
                      WithPointerCount(1u), WithDisplayId(ADISPLAY_ID_DEFAULT)));
}

TEST_F(GestureConverterTestWithChoreographer, FourFingerSwipe_Horizontal) {
    InputDeviceContext deviceContext(*mDevice, EVENTHUB_ID);
    GestureConverter converter(*mReader->getContext(), deviceContext, DEVICE_ID);
    converter.setDisplayId(ADISPLAY_ID_DEFAULT);

    Gesture startGesture(kGestureFourFingerSwipe, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME,
                         /* dx= */ 10, /* dy= */ 0);
    std::list<NotifyArgs> args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, startGesture);
    ASSERT_EQ(5u, args.size());

    // Four fake fingers should be created. We don't actually care where they are, so long as they
    // move appropriately.
    NotifyMotionArgs arg = std::get<NotifyMotionArgs>(args.front());
    ASSERT_THAT(arg,
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_DOWN), WithGestureOffset(0, 0, EPSILON),
                      WithGestureSwipeFingerCount(4),
                      WithMotionClassification(MotionClassification::MULTI_FINGER_SWIPE),
                      WithPointerCount(1u), WithToolType(ToolType::FINGER),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));
    PointerCoords finger0Start = arg.pointerCoords[0];
    args.pop_front();
    arg = std::get<NotifyMotionArgs>(args.front());
    ASSERT_THAT(arg,
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_POINTER_DOWN |
                                       1 << AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT),
                      WithGestureOffset(0, 0, EPSILON), WithGestureSwipeFingerCount(4),
                      WithMotionClassification(MotionClassification::MULTI_FINGER_SWIPE),
                      WithPointerCount(2u), WithToolType(ToolType::FINGER),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));
    PointerCoords finger1Start = arg.pointerCoords[1];
    args.pop_front();
    arg = std::get<NotifyMotionArgs>(args.front());
    ASSERT_THAT(arg,
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_POINTER_DOWN |
                                       2 << AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT),
                      WithGestureOffset(0, 0, EPSILON), WithGestureSwipeFingerCount(4),
                      WithMotionClassification(MotionClassification::MULTI_FINGER_SWIPE),
                      WithPointerCount(3u), WithToolType(ToolType::FINGER),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));
    PointerCoords finger2Start = arg.pointerCoords[2];
    args.pop_front();
    arg = std::get<NotifyMotionArgs>(args.front());
    ASSERT_THAT(arg,
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_POINTER_DOWN |
                                       3 << AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT),
                      WithGestureOffset(0, 0, EPSILON), WithGestureSwipeFingerCount(4),
                      WithMotionClassification(MotionClassification::MULTI_FINGER_SWIPE),
                      WithPointerCount(4u), WithToolType(ToolType::FINGER),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));
    PointerCoords finger3Start = arg.pointerCoords[3];
    args.pop_front();

    arg = std::get<NotifyMotionArgs>(args.front());
    ASSERT_THAT(arg,
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_MOVE),
                      WithGestureOffset(0.01, 0, EPSILON), WithGestureSwipeFingerCount(4),
                      WithMotionClassification(MotionClassification::MULTI_FINGER_SWIPE),
                      WithPointerCount(4u), WithToolType(ToolType::FINGER),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));
    EXPECT_EQ(arg.pointerCoords[0].getX(), finger0Start.getX() + 10);
    EXPECT_EQ(arg.pointerCoords[1].getX(), finger1Start.getX() + 10);
    EXPECT_EQ(arg.pointerCoords[2].getX(), finger2Start.getX() + 10);
    EXPECT_EQ(arg.pointerCoords[3].getX(), finger3Start.getX() + 10);
    EXPECT_EQ(arg.pointerCoords[0].getY(), finger0Start.getY());
    EXPECT_EQ(arg.pointerCoords[1].getY(), finger1Start.getY());
    EXPECT_EQ(arg.pointerCoords[2].getY(), finger2Start.getY());
    EXPECT_EQ(arg.pointerCoords[3].getY(), finger3Start.getY());

    Gesture continueGesture(kGestureFourFingerSwipe, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME,
                            /* dx= */ 5, /* dy= */ 0);
    args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, continueGesture);
    ASSERT_EQ(1u, args.size());
    arg = std::get<NotifyMotionArgs>(args.front());
    ASSERT_THAT(arg,
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_MOVE),
                      WithGestureOffset(0.005, 0, EPSILON), WithGestureSwipeFingerCount(4),
                      WithMotionClassification(MotionClassification::MULTI_FINGER_SWIPE),
                      WithPointerCount(4u), WithToolType(ToolType::FINGER),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));
    EXPECT_EQ(arg.pointerCoords[0].getX(), finger0Start.getX() + 15);
    EXPECT_EQ(arg.pointerCoords[1].getX(), finger1Start.getX() + 15);
    EXPECT_EQ(arg.pointerCoords[2].getX(), finger2Start.getX() + 15);
    EXPECT_EQ(arg.pointerCoords[3].getX(), finger3Start.getX() + 15);
    EXPECT_EQ(arg.pointerCoords[0].getY(), finger0Start.getY());
    EXPECT_EQ(arg.pointerCoords[1].getY(), finger1Start.getY());
    EXPECT_EQ(arg.pointerCoords[2].getY(), finger2Start.getY());
    EXPECT_EQ(arg.pointerCoords[3].getY(), finger3Start.getY());

    Gesture liftGesture(kGestureSwipeLift, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME);
    args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, liftGesture);
    ASSERT_EQ(4u, args.size());
    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_POINTER_UP |
                                       3 << AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT),
                      WithGestureOffset(0, 0, EPSILON), WithGestureSwipeFingerCount(4),
                      WithMotionClassification(MotionClassification::MULTI_FINGER_SWIPE),
                      WithPointerCount(4u), WithToolType(ToolType::FINGER),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));
    args.pop_front();
    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_POINTER_UP |
                                       2 << AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT),
                      WithGestureOffset(0, 0, EPSILON), WithGestureSwipeFingerCount(4),
                      WithMotionClassification(MotionClassification::MULTI_FINGER_SWIPE),
                      WithPointerCount(3u), WithToolType(ToolType::FINGER),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));
    args.pop_front();
    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_POINTER_UP |
                                       1 << AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT),
                      WithGestureOffset(0, 0, EPSILON), WithGestureSwipeFingerCount(4),
                      WithMotionClassification(MotionClassification::MULTI_FINGER_SWIPE),
                      WithPointerCount(2u), WithToolType(ToolType::FINGER),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));
    args.pop_front();
    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_UP), WithGestureOffset(0, 0, EPSILON),
                      WithGestureSwipeFingerCount(4),
                      WithMotionClassification(MotionClassification::MULTI_FINGER_SWIPE),
                      WithPointerCount(1u), WithToolType(ToolType::FINGER),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));
}

TEST_F(GestureConverterTestWithChoreographer, Pinch_Inwards) {
    InputDeviceContext deviceContext(*mDevice, EVENTHUB_ID);
    GestureConverter converter(*mReader->getContext(), deviceContext, DEVICE_ID);
    converter.setDisplayId(ADISPLAY_ID_DEFAULT);

    Gesture startGesture(kGesturePinch, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME, /* dz= */ 1,
                         GESTURES_ZOOM_START);
    std::list<NotifyArgs> args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, startGesture);
    ASSERT_EQ(2u, args.size());
    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_DOWN),
                      WithMotionClassification(MotionClassification::PINCH),
                      WithGesturePinchScaleFactor(1.0f, EPSILON), WithCoords(-100, 0),
                      WithPointerCount(1u), WithToolType(ToolType::FINGER),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));
    args.pop_front();
    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_POINTER_DOWN |
                                       1 << AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT),
                      WithMotionClassification(MotionClassification::PINCH),
                      WithGesturePinchScaleFactor(1.0f, EPSILON), WithPointerCoords(1, 100, 0),
                      WithPointerCount(2u), WithToolType(ToolType::FINGER),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));

    Gesture updateGesture(kGesturePinch, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME,
                          /* dz= */ 0.8, GESTURES_ZOOM_UPDATE);
    args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, updateGesture);
    ASSERT_EQ(1u, args.size());
    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_MOVE),
                      WithMotionClassification(MotionClassification::PINCH),
                      WithGesturePinchScaleFactor(0.8f, EPSILON), WithPointerCoords(0, -80, 0),
                      WithPointerCoords(1, 80, 0), WithPointerCount(2u),
                      WithToolType(ToolType::FINGER), WithDisplayId(ADISPLAY_ID_DEFAULT)));

    Gesture endGesture(kGesturePinch, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME, /* dz= */ 1,
                       GESTURES_ZOOM_END);
    args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, endGesture);
    ASSERT_EQ(2u, args.size());
    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_POINTER_UP |
                                       1 << AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT),
                      WithMotionClassification(MotionClassification::PINCH),
                      WithGesturePinchScaleFactor(1.0f, EPSILON), WithPointerCount(2u),
                      WithToolType(ToolType::FINGER), WithDisplayId(ADISPLAY_ID_DEFAULT)));
    args.pop_front();
    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_UP),
                      WithMotionClassification(MotionClassification::PINCH),
                      WithGesturePinchScaleFactor(1.0f, EPSILON), WithPointerCount(1u),
                      WithToolType(ToolType::FINGER), WithDisplayId(ADISPLAY_ID_DEFAULT)));
}

TEST_F(GestureConverterTestWithChoreographer, Pinch_Outwards) {
    InputDeviceContext deviceContext(*mDevice, EVENTHUB_ID);
    GestureConverter converter(*mReader->getContext(), deviceContext, DEVICE_ID);
    converter.setDisplayId(ADISPLAY_ID_DEFAULT);

    Gesture startGesture(kGesturePinch, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME, /* dz= */ 1,
                         GESTURES_ZOOM_START);
    std::list<NotifyArgs> args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, startGesture);
    ASSERT_EQ(2u, args.size());
    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_DOWN),
                      WithMotionClassification(MotionClassification::PINCH),
                      WithGesturePinchScaleFactor(1.0f, EPSILON), WithCoords(-100, 0),
                      WithPointerCount(1u), WithToolType(ToolType::FINGER),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));
    args.pop_front();
    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_POINTER_DOWN |
                                       1 << AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT),
                      WithMotionClassification(MotionClassification::PINCH),
                      WithGesturePinchScaleFactor(1.0f, EPSILON), WithPointerCoords(1, 100, 0),
                      WithPointerCount(2u), WithToolType(ToolType::FINGER),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));

    Gesture updateGesture(kGesturePinch, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME,
                          /* dz= */ 1.1, GESTURES_ZOOM_UPDATE);
    args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, updateGesture);
    ASSERT_EQ(1u, args.size());
    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_MOVE),
                      WithMotionClassification(MotionClassification::PINCH),
                      WithGesturePinchScaleFactor(1.1f, EPSILON), WithPointerCoords(0, -110, 0),
                      WithPointerCoords(1, 110, 0), WithPointerCount(2u),
                      WithToolType(ToolType::FINGER), WithDisplayId(ADISPLAY_ID_DEFAULT)));

    Gesture endGesture(kGesturePinch, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME, /* dz= */ 1,
                       GESTURES_ZOOM_END);
    args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, endGesture);
    ASSERT_EQ(2u, args.size());
    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_POINTER_UP |
                                       1 << AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT),
                      WithMotionClassification(MotionClassification::PINCH),
                      WithGesturePinchScaleFactor(1.0f, EPSILON), WithPointerCount(2u),
                      WithToolType(ToolType::FINGER), WithDisplayId(ADISPLAY_ID_DEFAULT)));
    args.pop_front();
    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_UP),
                      WithMotionClassification(MotionClassification::PINCH),
                      WithGesturePinchScaleFactor(1.0f, EPSILON), WithPointerCount(1u),
                      WithToolType(ToolType::FINGER), WithDisplayId(ADISPLAY_ID_DEFAULT)));
}

TEST_F(GestureConverterTestWithChoreographer, Pinch_ClearsClassificationAfterGesture) {
    InputDeviceContext deviceContext(*mDevice, EVENTHUB_ID);
    GestureConverter converter(*mReader->getContext(), deviceContext, DEVICE_ID);
    converter.setDisplayId(ADISPLAY_ID_DEFAULT);

    Gesture startGesture(kGesturePinch, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME, /*dz=*/1,
                         GESTURES_ZOOM_START);
    std::list<NotifyArgs> args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, startGesture);

    Gesture updateGesture(kGesturePinch, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME,
                          /*dz=*/1.2, GESTURES_ZOOM_UPDATE);
    args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, updateGesture);

    Gesture endGesture(kGesturePinch, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME, /*dz=*/1,
                       GESTURES_ZOOM_END);
    args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, endGesture);

    Gesture moveGesture(kGestureMove, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME, -5, 10);
    args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, moveGesture);
    ASSERT_EQ(1u, args.size());
    EXPECT_THAT(std::get<NotifyMotionArgs>(args.front()),
                WithMotionClassification(MotionClassification::NONE));
}

TEST_F(GestureConverterTestWithChoreographer, Pinch_ClearsScaleFactorAfterGesture) {
    InputDeviceContext deviceContext(*mDevice, EVENTHUB_ID);
    GestureConverter converter(*mReader->getContext(), deviceContext, DEVICE_ID);
    converter.setDisplayId(ADISPLAY_ID_DEFAULT);

    Gesture startGesture(kGesturePinch, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME, /*dz=*/1,
                         GESTURES_ZOOM_START);
    std::list<NotifyArgs> args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, startGesture);

    Gesture updateGesture(kGesturePinch, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME,
                          /*dz=*/1.2, GESTURES_ZOOM_UPDATE);
    args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, updateGesture);

    Gesture endGesture(kGesturePinch, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME, /*dz=*/1,
                       GESTURES_ZOOM_END);
    args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, endGesture);

    // Move gestures don't use the fake finger array, so to test that gesture axes are cleared we
    // need to use another gesture type, like scroll.
    Gesture scrollGesture(kGestureScroll, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME, /*dx=*/1,
                          /*dy=*/0);
    args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, scrollGesture);
    ASSERT_FALSE(args.empty());
    EXPECT_THAT(std::get<NotifyMotionArgs>(args.front()), WithGesturePinchScaleFactor(0, EPSILON));
}

TEST_F(GestureConverterTestWithChoreographer, ResetWithButtonPressed) {
    InputDeviceContext deviceContext(*mDevice, EVENTHUB_ID);
    GestureConverter converter(*mReader->getContext(), deviceContext, DEVICE_ID);
    converter.setDisplayId(ADISPLAY_ID_DEFAULT);

    Gesture downGesture(kGestureButtonsChange, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME,
                        /*down=*/GESTURES_BUTTON_LEFT | GESTURES_BUTTON_RIGHT,
                        /*up=*/GESTURES_BUTTON_NONE, /*is_tap=*/false);
    (void)converter.handleGesture(ARBITRARY_TIME, READ_TIME, downGesture);

    std::list<NotifyArgs> args = converter.reset(ARBITRARY_TIME);
    ASSERT_EQ(3u, args.size());

    EXPECT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_BUTTON_RELEASE),
                      WithActionButton(AMOTION_EVENT_BUTTON_PRIMARY),
                      WithButtonState(AMOTION_EVENT_BUTTON_SECONDARY), WithCoords(0, 0),
                      WithToolType(ToolType::FINGER), WithDisplayId(ADISPLAY_ID_DEFAULT)));
    args.pop_front();
    EXPECT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_BUTTON_RELEASE),
                      WithActionButton(AMOTION_EVENT_BUTTON_SECONDARY), WithButtonState(0),
                      WithCoords(0, 0), WithToolType(ToolType::FINGER),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));
    args.pop_front();
    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_UP), WithButtonState(0),
                      WithCoords(0, 0), WithToolType(ToolType::FINGER),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));
}

TEST_F(GestureConverterTestWithChoreographer, ResetDuringScroll) {
    InputDeviceContext deviceContext(*mDevice, EVENTHUB_ID);
    GestureConverter converter(*mReader->getContext(), deviceContext, DEVICE_ID);
    converter.setDisplayId(ADISPLAY_ID_DEFAULT);

    Gesture startGesture(kGestureScroll, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME, 0, -10);
    (void)converter.handleGesture(ARBITRARY_TIME, READ_TIME, startGesture);

    std::list<NotifyArgs> args = converter.reset(ARBITRARY_TIME);
    ASSERT_EQ(1u, args.size());
    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_UP), WithCoords(0, -10),
                      WithGestureScrollDistance(0, 0, EPSILON),
                      WithMotionClassification(MotionClassification::TWO_FINGER_SWIPE),
                      WithToolType(ToolType::FINGER),
                      WithFlags(AMOTION_EVENT_FLAG_IS_GENERATED_GESTURE),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));
}

TEST_F(GestureConverterTestWithChoreographer, ResetDuringThreeFingerSwipe) {
    InputDeviceContext deviceContext(*mDevice, EVENTHUB_ID);
    GestureConverter converter(*mReader->getContext(), deviceContext, DEVICE_ID);
    converter.setDisplayId(ADISPLAY_ID_DEFAULT);

    Gesture startGesture(kGestureSwipe, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME, /*dx=*/0,
                         /*dy=*/10);
    (void)converter.handleGesture(ARBITRARY_TIME, READ_TIME, startGesture);

    std::list<NotifyArgs> args = converter.reset(ARBITRARY_TIME);
    ASSERT_EQ(3u, args.size());
    EXPECT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_POINTER_UP |
                                       2 << AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT),
                      WithGestureOffset(0, 0, EPSILON),
                      WithMotionClassification(MotionClassification::MULTI_FINGER_SWIPE),
                      WithPointerCount(3u), WithToolType(ToolType::FINGER),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));
    args.pop_front();
    EXPECT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_POINTER_UP |
                                       1 << AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT),
                      WithGestureOffset(0, 0, EPSILON),
                      WithMotionClassification(MotionClassification::MULTI_FINGER_SWIPE),
                      WithPointerCount(2u), WithToolType(ToolType::FINGER),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));
    args.pop_front();
    EXPECT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_UP), WithGestureOffset(0, 0, EPSILON),
                      WithMotionClassification(MotionClassification::MULTI_FINGER_SWIPE),
                      WithPointerCount(1u), WithToolType(ToolType::FINGER),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));
}

TEST_F(GestureConverterTestWithChoreographer, ResetDuringPinch) {
    InputDeviceContext deviceContext(*mDevice, EVENTHUB_ID);
    GestureConverter converter(*mReader->getContext(), deviceContext, DEVICE_ID);
    converter.setDisplayId(ADISPLAY_ID_DEFAULT);

    Gesture startGesture(kGesturePinch, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME, /*dz=*/1,
                         GESTURES_ZOOM_START);
    (void)converter.handleGesture(ARBITRARY_TIME, READ_TIME, startGesture);

    std::list<NotifyArgs> args = converter.reset(ARBITRARY_TIME);
    ASSERT_EQ(2u, args.size());
    EXPECT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_POINTER_UP |
                                       1 << AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT),
                      WithMotionClassification(MotionClassification::PINCH),
                      WithGesturePinchScaleFactor(1.0f, EPSILON), WithPointerCount(2u),
                      WithToolType(ToolType::FINGER), WithDisplayId(ADISPLAY_ID_DEFAULT)));
    args.pop_front();
    EXPECT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_UP),
                      WithMotionClassification(MotionClassification::PINCH),
                      WithGesturePinchScaleFactor(1.0f, EPSILON), WithPointerCount(1u),
                      WithToolType(ToolType::FINGER), WithDisplayId(ADISPLAY_ID_DEFAULT)));
}

TEST_F(GestureConverterTestWithChoreographer, FlingTapDown) {
    InputDeviceContext deviceContext(*mDevice, EVENTHUB_ID);
    GestureConverter converter(*mReader->getContext(), deviceContext, DEVICE_ID);
    converter.setDisplayId(ADISPLAY_ID_DEFAULT);

    Gesture tapDownGesture(kGestureFling, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME,
                           /*vx=*/0.f, /*vy=*/0.f, GESTURES_FLING_TAP_DOWN);
    std::list<NotifyArgs> args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, tapDownGesture);

    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_HOVER_MOVE), WithCoords(0, 0),
                      WithRelativeMotion(0.f, 0.f), WithToolType(ToolType::FINGER),
                      WithButtonState(0), WithPressure(0.0f), WithDisplayId(ADISPLAY_ID_DEFAULT)));
}

TEST_F(GestureConverterTestWithChoreographer, Tap) {
    // Tap should produce button press/release events
    InputDeviceContext deviceContext(*mDevice, EVENTHUB_ID);
    GestureConverter converter(*mReader->getContext(), deviceContext, DEVICE_ID);
    converter.setDisplayId(ADISPLAY_ID_DEFAULT);

    Gesture flingGesture(kGestureFling, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME, /* vx= */ 0,
                         /* vy= */ 0, GESTURES_FLING_TAP_DOWN);
    std::list<NotifyArgs> args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, flingGesture);

    ASSERT_EQ(1u, args.size());
    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_HOVER_MOVE), WithCoords(0, 0),
                      WithRelativeMotion(0, 0), WithToolType(ToolType::FINGER), WithButtonState(0),
                      WithPressure(0.0f), WithDisplayId(ADISPLAY_ID_DEFAULT)));

    Gesture tapGesture(kGestureButtonsChange, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME,
                       /* down= */ GESTURES_BUTTON_LEFT,
                       /* up= */ GESTURES_BUTTON_LEFT, /* is_tap= */ true);
    args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, tapGesture);

    ASSERT_EQ(5u, args.size());
    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_DOWN), WithCoords(0, 0),
                      WithRelativeMotion(0.f, 0.f), WithToolType(ToolType::FINGER),
                      WithButtonState(AMOTION_EVENT_BUTTON_PRIMARY), WithPressure(1.0f),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));
    args.pop_front();
    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_BUTTON_PRESS),
                      WithActionButton(AMOTION_EVENT_BUTTON_PRIMARY),
                      WithButtonState(AMOTION_EVENT_BUTTON_PRIMARY), WithCoords(0, 0),
                      WithRelativeMotion(0.f, 0.f), WithToolType(ToolType::FINGER),
                      WithButtonState(AMOTION_EVENT_BUTTON_PRIMARY), WithPressure(1.0f),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));
    args.pop_front();
    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_BUTTON_RELEASE),
                      WithActionButton(AMOTION_EVENT_BUTTON_PRIMARY), WithButtonState(0),
                      WithCoords(0, 0), WithRelativeMotion(0.f, 0.f),
                      WithToolType(ToolType::FINGER), WithButtonState(0), WithPressure(1.0f),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));
    args.pop_front();
    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_UP), WithCoords(0, 0),
                      WithRelativeMotion(0.f, 0.f), WithToolType(ToolType::FINGER),
                      WithButtonState(0), WithPressure(0.0f), WithDisplayId(ADISPLAY_ID_DEFAULT)));
    args.pop_front();
    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_HOVER_MOVE), WithCoords(0, 0),
                      WithRelativeMotion(0, 0), WithToolType(ToolType::FINGER), WithButtonState(0),
                      WithPressure(0.0f), WithDisplayId(ADISPLAY_ID_DEFAULT)));
}

TEST_F(GestureConverterTestWithChoreographer, Click) {
    // Click should produce button press/release events
    InputDeviceContext deviceContext(*mDevice, EVENTHUB_ID);
    GestureConverter converter(*mReader->getContext(), deviceContext, DEVICE_ID);
    converter.setDisplayId(ADISPLAY_ID_DEFAULT);

    Gesture flingGesture(kGestureFling, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME, /* vx= */ 0,
                         /* vy= */ 0, GESTURES_FLING_TAP_DOWN);
    std::list<NotifyArgs> args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, flingGesture);

    ASSERT_EQ(1u, args.size());
    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_HOVER_MOVE), WithCoords(0, 0),
                      WithRelativeMotion(0, 0), WithToolType(ToolType::FINGER), WithButtonState(0),
                      WithPressure(0.0f), WithDisplayId(ADISPLAY_ID_DEFAULT)));

    Gesture buttonDownGesture(kGestureButtonsChange, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME,
                              /* down= */ GESTURES_BUTTON_LEFT,
                              /* up= */ GESTURES_BUTTON_NONE, /* is_tap= */ false);
    args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, buttonDownGesture);

    ASSERT_EQ(2u, args.size());
    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_DOWN), WithCoords(0, 0),
                      WithRelativeMotion(0.f, 0.f), WithToolType(ToolType::FINGER),
                      WithButtonState(AMOTION_EVENT_BUTTON_PRIMARY), WithPressure(1.0f),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));
    args.pop_front();
    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_BUTTON_PRESS),
                      WithActionButton(AMOTION_EVENT_BUTTON_PRIMARY),
                      WithButtonState(AMOTION_EVENT_BUTTON_PRIMARY), WithCoords(0, 0),
                      WithRelativeMotion(0.f, 0.f), WithToolType(ToolType::FINGER),
                      WithButtonState(AMOTION_EVENT_BUTTON_PRIMARY), WithPressure(1.0f),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));

    Gesture buttonUpGesture(kGestureButtonsChange, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME,
                            /* down= */ GESTURES_BUTTON_NONE,
                            /* up= */ GESTURES_BUTTON_LEFT, /* is_tap= */ false);
    args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, buttonUpGesture);

    ASSERT_EQ(3u, args.size());
    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_BUTTON_RELEASE),
                      WithActionButton(AMOTION_EVENT_BUTTON_PRIMARY), WithButtonState(0),
                      WithCoords(0, 0), WithRelativeMotion(0.f, 0.f),
                      WithToolType(ToolType::FINGER), WithButtonState(0), WithPressure(1.0f),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));
    args.pop_front();
    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_UP), WithCoords(0, 0),
                      WithRelativeMotion(0.f, 0.f), WithToolType(ToolType::FINGER),
                      WithButtonState(0), WithPressure(0.0f), WithDisplayId(ADISPLAY_ID_DEFAULT)));
    args.pop_front();
    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_HOVER_MOVE), WithCoords(0, 0),
                      WithRelativeMotion(0, 0), WithToolType(ToolType::FINGER), WithButtonState(0),
                      WithPressure(0.0f), WithDisplayId(ADISPLAY_ID_DEFAULT)));
}

TEST_F_WITH_FLAGS(GestureConverterTestWithChoreographer, TapWithTapToClickDisabled,
                  REQUIRES_FLAGS_ENABLED(TOUCHPAD_PALM_REJECTION)) {
    // Tap should be ignored when disabled
    mReader->getContext()->setPreventingTouchpadTaps(true);

    InputDeviceContext deviceContext(*mDevice, EVENTHUB_ID);
    GestureConverter converter(*mReader->getContext(), deviceContext, DEVICE_ID);
    converter.setDisplayId(ADISPLAY_ID_DEFAULT);

    Gesture flingGesture(kGestureFling, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME, /* vx= */ 0,
                         /* vy= */ 0, GESTURES_FLING_TAP_DOWN);
    std::list<NotifyArgs> args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, flingGesture);

    ASSERT_EQ(1u, args.size());
    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_HOVER_MOVE), WithCoords(0, 0),
                      WithRelativeMotion(0, 0), WithToolType(ToolType::FINGER), WithButtonState(0),
                      WithPressure(0.0f), WithDisplayId(ADISPLAY_ID_DEFAULT)));
    args.pop_front();

    Gesture tapGesture(kGestureButtonsChange, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME,
                       /* down= */ GESTURES_BUTTON_LEFT,
                       /* up= */ GESTURES_BUTTON_LEFT, /* is_tap= */ true);
    args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, tapGesture);

    // no events should be generated
    ASSERT_EQ(0u, args.size());

    // Future taps should be re-enabled
    ASSERT_FALSE(mReader->getContext()->isPreventingTouchpadTaps());
}

TEST_F_WITH_FLAGS(GestureConverterTestWithChoreographer, ClickWithTapToClickDisabled,
                  REQUIRES_FLAGS_ENABLED(TOUCHPAD_PALM_REJECTION)) {
    // Click should still produce button press/release events
    mReader->getContext()->setPreventingTouchpadTaps(true);

    InputDeviceContext deviceContext(*mDevice, EVENTHUB_ID);
    GestureConverter converter(*mReader->getContext(), deviceContext, DEVICE_ID);
    converter.setDisplayId(ADISPLAY_ID_DEFAULT);

    Gesture flingGesture(kGestureFling, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME, /* vx= */ 0,
                         /* vy= */ 0, GESTURES_FLING_TAP_DOWN);
    std::list<NotifyArgs> args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, flingGesture);

    ASSERT_EQ(1u, args.size());
    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_HOVER_MOVE), WithCoords(0, 0),
                      WithRelativeMotion(0, 0), WithToolType(ToolType::FINGER), WithButtonState(0),
                      WithPressure(0.0f), WithDisplayId(ADISPLAY_ID_DEFAULT)));

    Gesture buttonDownGesture(kGestureButtonsChange, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME,
                              /* down= */ GESTURES_BUTTON_LEFT,
                              /* up= */ GESTURES_BUTTON_NONE, /* is_tap= */ false);
    args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, buttonDownGesture);
    ASSERT_EQ(2u, args.size());

    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_DOWN), WithCoords(0, 0),
                      WithRelativeMotion(0.f, 0.f), WithToolType(ToolType::FINGER),
                      WithButtonState(AMOTION_EVENT_BUTTON_PRIMARY), WithPressure(1.0f),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));
    args.pop_front();
    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_BUTTON_PRESS),
                      WithActionButton(AMOTION_EVENT_BUTTON_PRIMARY),
                      WithButtonState(AMOTION_EVENT_BUTTON_PRIMARY), WithCoords(0, 0),
                      WithRelativeMotion(0.f, 0.f), WithToolType(ToolType::FINGER),
                      WithButtonState(AMOTION_EVENT_BUTTON_PRIMARY), WithPressure(1.0f),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));

    Gesture buttonUpGesture(kGestureButtonsChange, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME,
                            /* down= */ GESTURES_BUTTON_NONE,
                            /* up= */ GESTURES_BUTTON_LEFT, /* is_tap= */ false);
    args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, buttonUpGesture);

    ASSERT_EQ(3u, args.size());
    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_BUTTON_RELEASE),
                      WithActionButton(AMOTION_EVENT_BUTTON_PRIMARY), WithButtonState(0),
                      WithCoords(0, 0), WithRelativeMotion(0.f, 0.f),
                      WithToolType(ToolType::FINGER), WithButtonState(0), WithPressure(1.0f),
                      WithDisplayId(ADISPLAY_ID_DEFAULT)));
    args.pop_front();
    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_UP), WithCoords(0, 0),
                      WithRelativeMotion(0.f, 0.f), WithToolType(ToolType::FINGER),
                      WithButtonState(0), WithPressure(0.0f), WithDisplayId(ADISPLAY_ID_DEFAULT)));
    args.pop_front();
    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_HOVER_MOVE), WithCoords(0, 0),
                      WithRelativeMotion(0, 0), WithToolType(ToolType::FINGER), WithButtonState(0),
                      WithPressure(0.0f), WithDisplayId(ADISPLAY_ID_DEFAULT)));

    // Future taps should be re-enabled
    ASSERT_FALSE(mReader->getContext()->isPreventingTouchpadTaps());
}

TEST_F_WITH_FLAGS(GestureConverterTestWithChoreographer, MoveEnablesTapToClick,
                  REQUIRES_FLAGS_ENABLED(TOUCHPAD_PALM_REJECTION)) {
    // initially disable tap-to-click
    mReader->getContext()->setPreventingTouchpadTaps(true);

    InputDeviceContext deviceContext(*mDevice, EVENTHUB_ID);
    GestureConverter converter(*mReader->getContext(), deviceContext, DEVICE_ID);
    converter.setDisplayId(ADISPLAY_ID_DEFAULT);

    Gesture moveGesture(kGestureMove, ARBITRARY_GESTURE_TIME, ARBITRARY_GESTURE_TIME, -5, 10);
    std::list<NotifyArgs> args = converter.handleGesture(ARBITRARY_TIME, READ_TIME, moveGesture);
    ASSERT_EQ(1u, args.size());

    ASSERT_THAT(std::get<NotifyMotionArgs>(args.front()),
                AllOf(WithMotionAction(AMOTION_EVENT_ACTION_HOVER_MOVE), WithCoords(0, 0),
                      WithRelativeMotion(-5, 10), WithToolType(ToolType::FINGER),
                      WithButtonState(0), WithPressure(0.0f), WithDisplayId(ADISPLAY_ID_DEFAULT)));

    // Future taps should be re-enabled
    ASSERT_FALSE(mReader->getContext()->isPreventingTouchpadTaps());
}

} // namespace android
