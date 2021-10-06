// Copyright (c) 2020 The Orbit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ThreadTrack.h"

#include <absl/strings/str_format.h>
#include <absl/synchronization/mutex.h>
#include <absl/time/time.h>

#include <algorithm>
#include <atomic>
#include <optional>

#include "ApiInterface/Orbit.h"
#include "App.h"
#include "Batcher.h"
#include "ClientData/CaptureData.h"
#include "ClientData/FunctionUtils.h"
#include "ClientData/TimerChain.h"
#include "DisplayFormats/DisplayFormats.h"
#include "GlUtils.h"
#include "Introspection/Introspection.h"
#include "OrbitBase/Logging.h"
#include "OrbitBase/ThreadConstants.h"
#include "OrbitBase/ThreadUtils.h"
#include "TextRenderer.h"
#include "TimeGraph.h"
#include "TimeGraphLayout.h"
#include "TriangleToggle.h"
#include "Viewport.h"
#include "capture_data.pb.h"

using orbit_client_data::CaptureData;
using orbit_client_data::TimerChain;

using orbit_client_protos::TimerInfo;

using orbit_grpc_protos::InstrumentedFunction;

ThreadTrack::ThreadTrack(CaptureViewElement* parent, TimeGraph* time_graph,
                         orbit_gl::Viewport* viewport, TimeGraphLayout* layout, uint32_t thread_id,
                         OrbitApp* app, const CaptureData* capture_data,
                         orbit_client_data::TimerData* timer_data,
                         ScopeTreeUpdateType scope_tree_update_type)
    : TimerTrack(parent, time_graph, viewport, layout, app, capture_data, timer_data),
      thread_id_{thread_id},
      scope_tree_update_type_{scope_tree_update_type} {
  Color color = TimeGraph::GetThreadColor(thread_id);
  thread_state_bar_ = std::make_shared<orbit_gl::ThreadStateBar>(
      this, app_, time_graph, viewport, layout, capture_data, thread_id, color);

  event_bar_ = std::make_shared<orbit_gl::CallstackThreadBar>(
      this, app_, time_graph, viewport, layout, capture_data, thread_id, color);

  tracepoint_bar_ = std::make_shared<orbit_gl::TracepointThreadBar>(
      this, app_, time_graph, viewport, layout, capture_data, thread_id, color);
}

std::string ThreadTrack::GetName() const {
  auto thread_id = GetThreadId();
  if (thread_id == orbit_base::kAllThreadsOfAllProcessesTid) {
    return "All tracepoint events";
  } else if (thread_id == orbit_base::kAllProcessThreadsTid) {
    return "All Threads";
  }
  return capture_data_->GetThreadName(thread_id);
}

constexpr std::string_view kAllThreads = " (all_threads)";
std::string ThreadTrack::GetLabel() const {
  auto thread_id = GetThreadId();
  auto name = GetName();
  if (thread_id == orbit_base::kAllThreadsOfAllProcessesTid) {
    return name;
  } else if (thread_id == orbit_base::kAllProcessThreadsTid) {
    std::string process_name = capture_data_->process_name();
    return process_name.append(kAllThreads);
  }
  return absl::StrFormat("%s [%d]", name, thread_id);
}

// Make sure some trailing characters have higher priority if there is no space for the full label.
int ThreadTrack::GetNumberOfPrioritizedTrailingCharacters() const {
  auto thread_id = GetThreadId();
  if (GetThreadId() == orbit_base::kAllThreadsOfAllProcessesTid) {
    return 0;
  } else if (GetThreadId() == orbit_base::kAllProcessThreadsTid) {
    // Example: proc... all_threads)
    return kAllThreads.size() - 1;
  }
  // Example: thread_na... [85023]
  return std::to_string(thread_id).size() + 2;
}

const TimerInfo* ThreadTrack::GetLeft(const TimerInfo& timer_info) const {
  return scope_tree_.FindPreviousScopeAtDepth(timer_info);
}

const TimerInfo* ThreadTrack::GetRight(const TimerInfo& timer_info) const {
  return scope_tree_.FindNextScopeAtDepth(timer_info);
}

const TimerInfo* ThreadTrack::GetUp(const TimerInfo& timer_info) const {
  return scope_tree_.FindParent(timer_info);
}

const TimerInfo* ThreadTrack::GetDown(const TimerInfo& timer_info) const {
  return scope_tree_.FindFirstChild(timer_info);
}

std::string ThreadTrack::GetBoxTooltip(const Batcher& batcher, PickingId id) const {
  const TimerInfo* timer_info = batcher.GetTimerInfo(id);
  if (timer_info == nullptr || timer_info->type() == TimerInfo::kCoreActivity) {
    return "";
  }

  const InstrumentedFunction* func =
      capture_data_->GetInstrumentedFunctionById(timer_info->function_id());

  std::string label;
  bool is_manual = timer_info->type() == TimerInfo::kApiScope;

  if (func == nullptr && !is_manual) {
    return GetTimesliceText(*timer_info);
  }

  if (is_manual) {
    label = timer_info->api_scope_name();
  } else {
    label = func->function_name();
  }

  std::string module_name = orbit_client_data::CaptureData::kUnknownFunctionOrModuleName;
  std::string function_name = orbit_client_data::CaptureData::kUnknownFunctionOrModuleName;
  if (func != nullptr) {
    module_name = orbit_client_data::function_utils::GetLoadedModuleNameByPath(func->file_path());
    function_name = label;
  } else if (timer_info->address_in_function() != 0) {
    const auto* module = capture_data_->FindModuleByAddress(timer_info->address_in_function());
    if (module != nullptr) {
      module_name = module->name();
    }

    function_name = capture_data_->GetFunctionNameByAddress(timer_info->address_in_function());
  }

  std::string result = absl::StrFormat(
      "<b>%s</b><br/>"
      "<i>Timing measured through %s instrumentation</i>"
      "<br/><br/>"
      "<b>Function:</b> %s<br/>"
      "<b>Module:</b> %s<br/>"
      "<b>Time:</b> %s",
      label, is_manual ? "manual" : "dynamic", function_name, module_name,
      orbit_display_formats::GetDisplayTime(
          TicksToDuration(timer_info->start(), timer_info->end())));

  if (timer_info->group_id() != kOrbitDefaultGroupId) {
    result += absl::StrFormat("<br/><b>Group Id:</b> %lu", timer_info->group_id());
  }

  return result;
}

bool ThreadTrack::IsTimerActive(const TimerInfo& timer_info) const {
  // TODO(b/179225487): Filtering for manually instrumented scopes is not yet supported.
  return timer_info.type() == TimerInfo::kApiScope ||
         app_->IsFunctionVisible(timer_info.function_id());
}

bool ThreadTrack::IsTrackSelected() const {
  return GetThreadId() != orbit_base::kAllProcessThreadsTid &&
         app_->selected_thread_id() == GetThreadId();
}

[[nodiscard]] static std::optional<Color> GetUserColor(const TimerInfo& timer_info) {
  if (timer_info.type() == TimerInfo::kApiScope) {
    if (!timer_info.has_color()) {
      return std::nullopt;
    }
    CHECK(timer_info.color().red() < 256);
    CHECK(timer_info.color().green() < 256);
    CHECK(timer_info.color().blue() < 256);
    CHECK(timer_info.color().alpha() < 256);
    return Color(static_cast<uint8_t>(timer_info.color().red()),
                 static_cast<uint8_t>(timer_info.color().green()),
                 static_cast<uint8_t>(timer_info.color().blue()),
                 static_cast<uint8_t>(timer_info.color().alpha()));
  }

  return std::nullopt;
}

float ThreadTrack::GetDefaultBoxHeight() const {
  auto box_height = layout_->GetTextBoxHeight();
  if (collapse_toggle_->IsCollapsed() && timer_data_->GetMaxDepth() > 0) {
    return box_height / static_cast<float>(timer_data_->GetMaxDepth());
  }
  return box_height;
}

Color ThreadTrack::GetTimerColor(const TimerInfo& timer_info, const internal::DrawData& draw_data) {
  uint64_t function_id = timer_info.function_id();
  uint64_t group_id = timer_info.group_id();
  bool is_selected = &timer_info == draw_data.selected_timer;
  bool is_function_id_highlighted = function_id != orbit_grpc_protos::kInvalidFunctionId &&
                                    function_id == draw_data.highlighted_function_id;
  bool is_group_id_highlighted =
      group_id != kOrbitDefaultGroupId && group_id == draw_data.highlighted_group_id;
  bool is_highlighted = !is_selected && (is_function_id_highlighted || is_group_id_highlighted);
  return GetTimerColor(timer_info, is_selected, is_highlighted, draw_data);
}

Color ThreadTrack::GetTimerColor(const TimerInfo& timer_info, bool is_selected, bool is_highlighted,
                                 const internal::DrawData& /*draw_data*/) const {
  const Color kInactiveColor(100, 100, 100, 255);
  const Color kSelectionColor(0, 128, 255, 255);
  if (is_highlighted) {
    return TimerTrack::kHighlightColor;
  }
  if (is_selected) {
    return kSelectionColor;
  }
  if (!IsTimerActive(timer_info)) {
    return kInactiveColor;
  }

  std::optional<Color> user_color = GetUserColor(timer_info);

  Color color;
  if (user_color.has_value()) {
    color = user_color.value();
  } else {
    color = TimeGraph::GetThreadColor(timer_info.thread_id());
  }

  constexpr uint8_t kOddAlpha = 210;
  if ((timer_info.depth() & 0x1) == 0) {
    color[3] = kOddAlpha;
  }

  return color;
}

bool ThreadTrack::IsEmpty() const {
  return thread_state_bar_->IsEmpty() && event_bar_->IsEmpty() && tracepoint_bar_->IsEmpty() &&
         timer_data_->IsEmpty();
}

void ThreadTrack::UpdatePositionOfSubtracks() {
  const float thread_state_track_height = layout_->GetThreadStateTrackHeight();
  const float event_track_height = layout_->GetEventTrackHeightFromTid(GetThreadId());
  const float space_between_subtracks = layout_->GetSpaceBetweenTracksAndThread();

  const Vec2 pos = GetPos();
  float current_y = pos[1] + layout_->GetTrackTabHeight() + layout_->GetTrackContentTopMargin();

  thread_state_bar_->SetPos(pos[0], current_y);
  if (thread_state_bar_->ShouldBeRendered()) {
    current_y += (space_between_subtracks + thread_state_track_height);
  }

  event_bar_->SetPos(pos[0], current_y);
  if (event_bar_->ShouldBeRendered()) {
    current_y += (space_between_subtracks + event_track_height);
  }

  tracepoint_bar_->SetPos(pos[0], current_y);
}

void ThreadTrack::OnPick(int x, int y) {
  Track::OnPick(x, y);
  app_->set_selected_thread_id(GetThreadId());
}

std::vector<orbit_gl::CaptureViewElement*> ThreadTrack::GetVisibleChildren() {
  std::vector<CaptureViewElement*> result;
  if (!thread_state_bar_->IsEmpty()) {
    result.push_back(thread_state_bar_.get());
  }

  if (!event_bar_->IsEmpty()) {
    result.push_back(event_bar_.get());
  }

  if (!tracepoint_bar_->IsEmpty()) {
    result.push_back(tracepoint_bar_.get());
  }

  return result;
}

std::vector<orbit_gl::CaptureViewElement*> ThreadTrack::GetChildren() const {
  return {thread_state_bar_.get(), event_bar_.get(), tracepoint_bar_.get()};
}

std::string ThreadTrack::GetTimesliceText(const TimerInfo& timer_info) const {
  std::string time = GetDisplayTime(timer_info);

  const InstrumentedFunction* func = app_->GetInstrumentedFunction(timer_info.function_id());
  if (func != nullptr) {
    std::string extra_info = GetExtraInfo(timer_info);
    const std::string& name = func->function_name();
    return absl::StrFormat("%s %s %s", name, extra_info.c_str(), time);
  }
  if (timer_info.type() == TimerInfo::kApiScope) {
    std::string extra_info = GetExtraInfo(timer_info);
    return absl::StrFormat("%s %s %s", timer_info.api_scope_name(), extra_info.c_str(), time);
  }

  ERROR("Unexpected case in ThreadTrack::SetTimesliceText: function=\"%s\", type=%d",
        func->function_name(), static_cast<int>(timer_info.type()));
  return "";
}

std::string ThreadTrack::GetTooltip() const {
  if (GetThreadId() == orbit_base::kAllProcessThreadsTid) {
    return "Shows collected samples for all threads of process " + capture_data_->process_name() +
           " " + std::to_string(capture_data_->process_id());
  }
  return "Shows collected samples and timings from dynamically instrumented "
         "functions";
}

float ThreadTrack::GetHeight() const {
  const uint32_t depth = collapse_toggle_->IsCollapsed()
                             ? std::min<uint32_t>(1, timer_data_->GetMaxDepth())
                             : timer_data_->GetMaxDepth();

  bool gap_between_tracks_and_timers =
      (!thread_state_bar_->IsEmpty() || !event_bar_->IsEmpty() || !tracepoint_bar_->IsEmpty()) &&
      (depth > 0);
  return GetHeaderHeight() +
         (gap_between_tracks_and_timers ? layout_->GetSpaceBetweenTracksAndThread() : 0) +
         layout_->GetTextBoxHeight() * depth + layout_->GetTrackContentBottomMargin();
}

float ThreadTrack::GetHeaderHeight() const {
  const float thread_state_track_height = layout_->GetThreadStateTrackHeight();
  const float event_track_height = layout_->GetEventTrackHeightFromTid(GetThreadId());
  const float tracepoint_track_height = layout_->GetEventTrackHeightFromTid(GetThreadId());
  const float space_between_subtracks = layout_->GetSpaceBetweenTracksAndThread();

  float header_height = layout_->GetTrackTabHeight() + layout_->GetTrackContentTopMargin();
  int track_count = 0;
  if (!thread_state_bar_->IsEmpty()) {
    header_height += thread_state_track_height;
    ++track_count;
  }

  if (!event_bar_->IsEmpty()) {
    header_height += event_track_height;
    ++track_count;
  }

  if (!tracepoint_bar_->IsEmpty()) {
    header_height += tracepoint_track_height;
    ++track_count;
  }

  header_height += std::max(0, track_count - 1) * space_between_subtracks;
  return header_height;
}

float ThreadTrack::GetYFromDepth(uint32_t depth) const {
  bool gap_between_tracks_and_timers =
      !thread_state_bar_->IsEmpty() || !event_bar_->IsEmpty() || !tracepoint_bar_->IsEmpty();
  return GetPos()[1] + GetHeaderHeight() +
         (gap_between_tracks_and_timers ? layout_->GetSpaceBetweenTracksAndThread() : 0) +
         GetDefaultBoxHeight() * static_cast<float>(depth);
}

void ThreadTrack::OnTimer(const TimerInfo& timer_info) {
  // Thread tracks use a ScopeTree so we don't need to create one TimerChain per depth.
  // Allocate a single TimerChain into which all timers will be appended.

  // Pass ownership to timer_chain. TODO(b/194268477): Pass timer_info as a value instead of
  // reference to be able to move it.
  const auto& timer_info_chain_ref = timer_data_->AddTimer(/*depth=*/0, timer_info);

  if (scope_tree_update_type_ == ScopeTreeUpdateType::kAlways) {
    absl::MutexLock lock(&scope_tree_mutex_);
    scope_tree_.Insert(&timer_info_chain_ref);
  }
}

void ThreadTrack::OnCaptureComplete() {
  if (scope_tree_update_type_ != ScopeTreeUpdateType::kOnCaptureComplete) {
    return;
  }
  // Build ScopeTree from timer chains.
  std::vector<const TimerChain*> timer_chains = timer_data_->GetChains();
  for (const TimerChain* timer_chain : timer_chains) {
    CHECK(timer_chain != nullptr);
    absl::MutexLock lock(&scope_tree_mutex_);
    for (const auto& block : *timer_chain) {
      for (size_t k = 0; k < block.size(); ++k) {
        scope_tree_.Insert(&block[k]);
      }
    }
  }
}

[[nodiscard]] static std::pair<float, float> GetBoxPosXAndWidth(const internal::DrawData& draw_data,
                                                                const TimeGraph* time_graph,
                                                                const TimerInfo& timer_info) {
  double start_us = time_graph->GetUsFromTick(timer_info.start());
  double end_us = time_graph->GetUsFromTick(timer_info.end());
  double elapsed_us = end_us - start_us;
  double normalized_start = start_us * draw_data.inv_time_window;
  double normalized_length = elapsed_us * draw_data.inv_time_window;
  float world_timer_width = static_cast<float>(normalized_length * draw_data.track_width);
  float world_timer_x =
      static_cast<float>(draw_data.track_start_x + normalized_start * draw_data.track_width);
  return {world_timer_x, world_timer_width};
}

[[nodiscard]] static inline uint64_t GetNextPixelBoundaryTimeNs(
    uint64_t current_timestamp, const internal::DrawData& draw_data) {
  uint64_t current_ns_from_min = current_timestamp - draw_data.min_tick;
  uint64_t total_ns_in_screen = draw_data.max_tick - draw_data.min_tick;
  uint64_t num_pixels_on_track = draw_data.viewport->WorldToScreenWidth(draw_data.track_width);

  // Given a track width of 4000 pixels, we can capture for 53 days without overflowing.
  uint64_t current_pixel = (current_ns_from_min * num_pixels_on_track) / total_ns_in_screen;
  uint64_t next_pixel = current_pixel + 1;

  // To calculate the timestamp of a pixel boundary, we round to the left similar to how it works in
  // other parts of Orbit.
  uint64_t next_pixel_ns_from_min = total_ns_in_screen * next_pixel / num_pixels_on_track;

  // Border case when we have a lot of pixels who have the same timestamp (because the number of
  // pixels is less than the nanoseconds in screen). In this case, as we've already drawn in the
  // current_timestamp, the next pixel to draw should have the next timestamp.
  if (next_pixel_ns_from_min == current_ns_from_min) {
    next_pixel_ns_from_min = current_ns_from_min + 1;
  }

  return draw_data.min_tick + next_pixel_ns_from_min;
}

// We minimize overdraw when drawing lines for small events by discarding events that would just
// draw over an already drawn pixel line. When zoomed in enough that all events are drawn as boxes,
// this has no effect. When zoomed  out, many events will be discarded quickly.
void ThreadTrack::DoUpdatePrimitives(Batcher* batcher, uint64_t min_tick, uint64_t max_tick,
                                     PickingMode /*picking_mode*/, float z_offset) {
  CHECK(batcher);
  visible_timer_count_ = 0;

  const internal::DrawData draw_data =
      GetDrawData(min_tick, max_tick, GetPos()[0], GetWidth(), z_offset, batcher, time_graph_,
                  viewport_, collapse_toggle_->IsCollapsed(), app_->selected_timer(),
                  app_->GetFunctionIdToHighlight(), app_->GetGroupIdToHighlight());

  absl::MutexLock lock(&scope_tree_mutex_);
  for (const auto& [depth, ordered_nodes] : scope_tree_.GetOrderedNodesByDepth()) {
    if (ordered_nodes.empty()) {
      continue;
    }
    timer_data_->UpdateMaxDepth(depth);

    float world_timer_y = GetYFromDepth(depth - 1);

    const orbit_client_protos::TimerInfo* timer_info =
        scope_tree_.FindFirstScopeAtOrAfterTime(depth, min_tick);

    while (timer_info != nullptr && timer_info->start() < max_tick) {
      ++visible_timer_count_;

      Color color = GetTimerColor(*timer_info, draw_data);
      std::unique_ptr<PickingUserData> user_data = CreatePickingUserData(*batcher, *timer_info);

      auto box_height = GetDefaultBoxHeight();
      const auto [pos_x, size_x] = GetBoxPosXAndWidth(draw_data, time_graph_, *timer_info);
      const Vec2 pos = {pos_x, world_timer_y};
      const Vec2 size = {size_x, box_height};

      auto timer_duration = timer_info->end() - timer_info->start();
      if (timer_duration > draw_data.ns_per_pixel) {
        if (!collapse_toggle_->IsCollapsed() && BoxHasRoomForText(size[0])) {
          DrawTimesliceText(*timer_info, draw_data.track_start_x, z_offset, pos, size);
        }
        batcher->AddShadedBox(pos, size, draw_data.z, color, std::move(user_data));
      } else {
        batcher->AddVerticalLine(pos, box_height, draw_data.z, color, std::move(user_data));
      }

      // Use the time at boundary of the next pixel as a threshold to avoid overdraw.
      uint64_t next_pixel_start_time_ns = GetNextPixelBoundaryTimeNs(timer_info->end(), draw_data);
      timer_info = scope_tree_.FindFirstScopeAtOrAfterTime(depth, next_pixel_start_time_ns);
    }
  }
}