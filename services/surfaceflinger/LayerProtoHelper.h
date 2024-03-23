/*
 * Copyright (C) 2017 The Android Open Source Project
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

#include <layerproto/LayerProtoHeader.h>
#include <renderengine/ExternalTexture.h>

#include <Layer.h>
#include <gui/WindowInfo.h>
#include <math/vec4.h>
#include <ui/BlurRegion.h>
#include <ui/GraphicBuffer.h>
#include <ui/Rect.h>
#include <ui/Region.h>
#include <ui/Transform.h>
#include <cstdint>

#include "FrontEnd/DisplayInfo.h"
#include "FrontEnd/LayerHierarchy.h"
#include "FrontEnd/LayerSnapshot.h"

namespace android {
namespace surfaceflinger {
class LayerProtoHelper {
public:
    static void writePositionToProto(
            const float x, const float y,
            std::function<perfetto::protos::PositionProto*()> getPositionProto);
    static void writeSizeToProto(const uint32_t w, const uint32_t h,
                                 std::function<perfetto::protos::SizeProto*()> getSizeProto);
    static void writeToProto(const Rect& rect,
                             std::function<perfetto::protos::RectProto*()> getRectProto);
    static void writeToProto(const Rect& rect, perfetto::protos::RectProto* rectProto);
    static void readFromProto(const perfetto::protos::RectProto& proto, Rect& outRect);
    static void writeToProto(const FloatRect& rect,
                             std::function<perfetto::protos::FloatRectProto*()> getFloatRectProto);
    static void writeToProto(const Region& region,
                             std::function<perfetto::protos::RegionProto*()> getRegionProto);
    static void writeToProto(const Region& region, perfetto::protos::RegionProto* regionProto);
    static void readFromProto(const perfetto::protos::RegionProto& regionProto, Region& outRegion);
    static void writeToProto(const half4 color,
                             std::function<perfetto::protos::ColorProto*()> getColorProto);
    // This writeToProto for transform is incorrect, but due to backwards compatibility, we can't
    // update Layers to use it. Use writeTransformToProto for any new transform proto data.
    static void writeToProtoDeprecated(const ui::Transform& transform,
                                       perfetto::protos::TransformProto* transformProto);
    static void writeTransformToProto(const ui::Transform& transform,
                                      perfetto::protos::TransformProto* transformProto);
    static void writeToProto(
            const renderengine::ExternalTexture& buffer,
            std::function<perfetto::protos::ActiveBufferProto*()> getActiveBufferProto);
    static void writeToProto(
            const gui::WindowInfo& inputInfo, const wp<Layer>& touchableRegionBounds,
            std::function<perfetto::protos::InputWindowInfoProto*()> getInputWindowInfoProto);
    static void writeToProto(const mat4 matrix,
                             perfetto::protos::ColorTransformProto* colorTransformProto);
    static void readFromProto(const perfetto::protos::ColorTransformProto& colorTransformProto,
                              mat4& matrix);
    static void writeToProto(const android::BlurRegion region, perfetto::protos::BlurRegion*);
    static void readFromProto(const perfetto::protos::BlurRegion& proto,
                              android::BlurRegion& outRegion);
    static void writeSnapshotToProto(perfetto::protos::LayerProto* outProto,
                                     const frontend::RequestedLayerState& requestedState,
                                     const frontend::LayerSnapshot& snapshot, uint32_t traceFlags);
    static google::protobuf::RepeatedPtrField<perfetto::protos::DisplayProto>
    writeDisplayInfoToProto(const frontend::DisplayInfos&);
};

class LayerProtoFromSnapshotGenerator {
public:
    LayerProtoFromSnapshotGenerator(const frontend::LayerSnapshotBuilder& snapshotBuilder,
                                    const frontend::DisplayInfos& displayInfos,
                                    const std::unordered_map<uint32_t, sp<Layer>>& legacyLayers,
                                    uint32_t traceFlags)
          : mSnapshotBuilder(snapshotBuilder),
            mLegacyLayers(legacyLayers),
            mDisplayInfos(displayInfos),
            mTraceFlags(traceFlags) {}
    perfetto::protos::LayersProto generate(const frontend::LayerHierarchy& root);

private:
    void writeHierarchyToProto(const frontend::LayerHierarchy& root,
                               frontend::LayerHierarchy::TraversalPath& path);
    frontend::LayerSnapshot* getSnapshot(frontend::LayerHierarchy::TraversalPath& path,
                                         const frontend::RequestedLayerState& layer);

    const frontend::LayerSnapshotBuilder& mSnapshotBuilder;
    const std::unordered_map<uint32_t, sp<Layer>>& mLegacyLayers;
    const frontend::DisplayInfos& mDisplayInfos;
    uint32_t mTraceFlags;
    perfetto::protos::LayersProto mLayersProto;
    // winscope expects all the layers, so provide a snapshot even if it not currently drawing
    std::unordered_map<frontend::LayerHierarchy::TraversalPath, frontend::LayerSnapshot,
                       frontend::LayerHierarchy::TraversalPathHash>
            mDefaultSnapshots;
    std::unordered_map<uint32_t /* child unique seq*/, uint32_t /* relative parent unique seq*/>
            mChildToRelativeParent;
    std::unordered_map<uint32_t /* child unique seq*/, uint32_t /* parent unique seq*/>
            mChildToParent;
};

} // namespace surfaceflinger
} // namespace android
