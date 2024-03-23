/*
 * Copyright 2023 The Android Open Source Project
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

#include <scheduler/Time.h>

namespace android::scheduler {

struct IVsyncSource {
    virtual Period period() const = 0;
    virtual TimePoint vsyncDeadlineAfter(TimePoint) const = 0;
    virtual Period minFramePeriod() const = 0;

protected:
    ~IVsyncSource() = default;
};

} // namespace android::scheduler
