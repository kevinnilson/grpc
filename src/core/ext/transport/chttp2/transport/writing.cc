//
//
// Copyright 2015 gRPC authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
//

#include <grpc/support/port_platform.h>

#include <inttypes.h>
#include <stddef.h>

#include <algorithm>
#include <memory>
#include <string>
#include <utility>

#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"
#include "absl/strings/string_view.h"
#include "absl/types/optional.h"

#include <grpc/event_engine/event_engine.h>
#include <grpc/slice.h>
#include <grpc/slice_buffer.h>
#include <grpc/support/log.h>

#include "src/core/ext/transport/chttp2/transport/chttp2_transport.h"
#include "src/core/ext/transport/chttp2/transport/context_list_entry.h"
#include "src/core/ext/transport/chttp2/transport/flow_control.h"
#include "src/core/ext/transport/chttp2/transport/frame_data.h"
#include "src/core/ext/transport/chttp2/transport/frame_ping.h"
#include "src/core/ext/transport/chttp2/transport/frame_rst_stream.h"
#include "src/core/ext/transport/chttp2/transport/frame_settings.h"
#include "src/core/ext/transport/chttp2/transport/frame_window_update.h"
#include "src/core/ext/transport/chttp2/transport/hpack_encoder.h"
#include "src/core/ext/transport/chttp2/transport/http2_settings.h"
#include "src/core/ext/transport/chttp2/transport/http_trace.h"
#include "src/core/ext/transport/chttp2/transport/internal.h"
#include "src/core/ext/transport/chttp2/transport/legacy_frame.h"
#include "src/core/ext/transport/chttp2/transport/ping_callbacks.h"
#include "src/core/ext/transport/chttp2/transport/ping_rate_policy.h"
#include "src/core/lib/channel/channelz.h"
#include "src/core/lib/debug/stats.h"
#include "src/core/lib/debug/stats_data.h"
#include "src/core/lib/debug/trace.h"
#include "src/core/lib/experiments/experiments.h"
#include "src/core/lib/gprpp/match.h"
#include "src/core/lib/gprpp/ref_counted.h"
#include "src/core/lib/gprpp/ref_counted_ptr.h"
#include "src/core/lib/gprpp/time.h"
#include "src/core/lib/iomgr/endpoint.h"
#include "src/core/lib/iomgr/error.h"
#include "src/core/lib/iomgr/exec_ctx.h"
#include "src/core/lib/slice/slice.h"
#include "src/core/lib/slice/slice_buffer.h"
#include "src/core/lib/transport/bdp_estimator.h"
#include "src/core/lib/transport/http2_errors.h"
#include "src/core/lib/transport/metadata_batch.h"
#include "src/core/lib/transport/transport.h"

// IWYU pragma: no_include "src/core/lib/gprpp/orphanable.h"

static void add_to_write_list(grpc_chttp2_write_cb** list,
                              grpc_chttp2_write_cb* cb) {
  cb->next = *list;
  *list = cb;
}

static void finish_write_cb(grpc_chttp2_transport* t, grpc_chttp2_stream* s,
                            grpc_chttp2_write_cb* cb, grpc_error_handle error) {
  grpc_chttp2_complete_closure_step(t, s, &cb->closure, error,
                                    "finish_write_cb");
  cb->next = t->write_cb_pool;
  t->write_cb_pool = cb;
}

static grpc_core::Duration NextAllowedPingInterval(grpc_chttp2_transport* t) {
  if (t->is_client) {
    return (t->keepalive_permit_without_calls == 0 && t->stream_map.empty())
               ? grpc_core::Duration::Hours(2)
               : grpc_core::Duration::Seconds(
                     1);  // A second is added to deal with
                          // network delays and timing imprecision
  }
  if (t->sent_goaway_state != GRPC_CHTTP2_GRACEFUL_GOAWAY) {
    // The gRPC keepalive spec doesn't call for any throttling on the server
    // side, but we are adding some throttling for protection anyway, unless
    // we are doing a graceful GOAWAY in which case we don't want to wait.
    if (grpc_core::IsMultipingEnabled()) {
      return grpc_core::Duration::Seconds(1);
    }
    return t->keepalive_time == grpc_core::Duration::Infinity()
               ? grpc_core::Duration::Seconds(20)
               : t->keepalive_time / 2;
  }
  return grpc_core::Duration::Zero();
}

static void maybe_initiate_ping(grpc_chttp2_transport* t) {
  if (!t->ping_callbacks.ping_requested()) {
    // no ping needed: wait
    return;
  }
  // InvalidateNow to avoid getting stuck re-initializing the ping timer
  // in a loop while draining the currently-held combiner. Also see
  // https://github.com/grpc/grpc/issues/26079.
  grpc_core::ExecCtx::Get()->InvalidateNow();
  Match(
      t->ping_rate_policy.RequestSendPing(NextAllowedPingInterval(t),
                                          t->ping_callbacks.pings_inflight()),
      [t](grpc_core::Chttp2PingRatePolicy::SendGranted) {
        t->ping_rate_policy.SentPing();
        const uint64_t id = t->ping_callbacks.StartPing(
            t->bitgen, t->keepalive_timeout,
            [t = t->Ref()] {
              grpc_core::ApplicationCallbackExecCtx callback_exec_ctx;
              grpc_core::ExecCtx exec_ctx;
              grpc_chttp2_ping_timeout(t);
            },
            t->event_engine.get());
        grpc_slice_buffer_add(t->outbuf.c_slice_buffer(),
                              grpc_chttp2_ping_create(false, id));
        if (t->channelz_socket != nullptr) {
          t->channelz_socket->RecordKeepaliveSent();
        }
        grpc_core::global_stats().IncrementHttp2PingsSent();
        if (GRPC_TRACE_FLAG_ENABLED(grpc_http_trace) ||
            GRPC_TRACE_FLAG_ENABLED(grpc_bdp_estimator_trace) ||
            GRPC_TRACE_FLAG_ENABLED(grpc_keepalive_trace)) {
          gpr_log(GPR_INFO, "%s: Ping sent [%s]: %s",
                  t->is_client ? "CLIENT" : "SERVER",
                  std::string(t->peer_string.as_string_view()).c_str(),
                  t->ping_rate_policy.GetDebugString().c_str());
        }
      },
      [t](grpc_core::Chttp2PingRatePolicy::TooManyRecentPings) {
        // need to receive something of substance before sending a ping again
        if (GRPC_TRACE_FLAG_ENABLED(grpc_http_trace) ||
            GRPC_TRACE_FLAG_ENABLED(grpc_bdp_estimator_trace) ||
            GRPC_TRACE_FLAG_ENABLED(grpc_keepalive_trace)) {
          gpr_log(GPR_INFO,
                  "CLIENT: Ping delayed [%s]: too many recent pings: %s",
                  std::string(t->peer_string.as_string_view()).c_str(),
                  t->ping_rate_policy.GetDebugString().c_str());
        }
      },
      [t](grpc_core::Chttp2PingRatePolicy::TooSoon too_soon) {
        // not enough elapsed time between successive pings
        if (GRPC_TRACE_FLAG_ENABLED(grpc_http_trace) ||
            GRPC_TRACE_FLAG_ENABLED(grpc_bdp_estimator_trace) ||
            GRPC_TRACE_FLAG_ENABLED(grpc_keepalive_trace)) {
          gpr_log(GPR_INFO,
                  "%s: Ping delayed [%s]: not enough time elapsed since last "
                  "ping. "
                  " Last ping:%s, minimum wait:%s need to wait:%s",
                  t->is_client ? "CLIENT" : "SERVER",
                  std::string(t->peer_string.as_string_view()).c_str(),
                  too_soon.last_ping.ToString().c_str(),
                  too_soon.next_allowed_ping_interval.ToString().c_str(),
                  too_soon.wait.ToString().c_str());
        }
        if (!t->delayed_ping_timer_handle.has_value()) {
          t->delayed_ping_timer_handle = t->event_engine->RunAfter(
              too_soon.wait, [t = t->Ref()]() mutable {
                grpc_core::ApplicationCallbackExecCtx callback_exec_ctx;
                grpc_core::ExecCtx exec_ctx;
                grpc_chttp2_retry_initiate_ping(std::move(t));
              });
        }
      });
}

static bool update_list(grpc_chttp2_transport* t, grpc_chttp2_stream* s,
                        int64_t send_bytes, grpc_chttp2_write_cb** list,
                        int64_t* ctr, grpc_error_handle error) {
  bool sched_any = false;
  grpc_chttp2_write_cb* cb = *list;
  *list = nullptr;
  *ctr += send_bytes;
  while (cb) {
    grpc_chttp2_write_cb* next = cb->next;
    if (cb->call_at_byte <= *ctr) {
      sched_any = true;
      finish_write_cb(t, s, cb, error);
    } else {
      add_to_write_list(list, cb);
    }
    cb = next;
  }
  return sched_any;
}

static void report_stall(grpc_chttp2_transport* t, grpc_chttp2_stream* s,
                         const char* staller) {
  if (GRPC_TRACE_FLAG_ENABLED(grpc_flowctl_trace)) {
    gpr_log(
        GPR_DEBUG,
        "%s:%p stream %d moved to stalled list by %s. This is FULLY expected "
        "to happen in a healthy program that is not seeing flow control stalls."
        " However, if you know that there are unwanted stalls, here is some "
        "helpful data: [fc:pending=%" PRIdPTR ":flowed=%" PRId64
        ":peer_initwin=%d:t_win=%" PRId64 ":s_win=%d:s_delta=%" PRId64 "]",
        std::string(t->peer_string.as_string_view()).c_str(), t, s->id, staller,
        s->flow_controlled_buffer.length, s->flow_controlled_bytes_flowed,
        t->settings[GRPC_ACKED_SETTINGS]
                   [GRPC_CHTTP2_SETTINGS_INITIAL_WINDOW_SIZE],
        t->flow_control.remote_window(),
        static_cast<uint32_t>(std::max(
            int64_t{0},
            s->flow_control.remote_window_delta() +
                static_cast<int64_t>(
                    t->settings[GRPC_PEER_SETTINGS]
                               [GRPC_CHTTP2_SETTINGS_INITIAL_WINDOW_SIZE]))),
        s->flow_control.remote_window_delta());
  }
}

// How many bytes would we like to put on the wire during a single syscall
static uint32_t target_write_size(grpc_chttp2_transport* /*t*/) {
  return 1024 * 1024;
}

namespace {

class CountDefaultMetadataEncoder {
 public:
  size_t count() const { return count_; }

  void Encode(const grpc_core::Slice&, const grpc_core::Slice&) {}

  template <typename Which>
  void Encode(Which, const typename Which::ValueType&) {
    count_++;
  }

 private:
  size_t count_ = 0;
};

}  // namespace

// Returns true if initial_metadata contains only default headers.
static bool is_default_initial_metadata(grpc_metadata_batch* initial_metadata) {
  CountDefaultMetadataEncoder enc;
  initial_metadata->Encode(&enc);
  return enc.count() == initial_metadata->count();
}

namespace {

class WriteContext {
 public:
  explicit WriteContext(grpc_chttp2_transport* t) : t_(t) {
    grpc_core::global_stats().IncrementHttp2WritesBegun();
  }

  void FlushSettings() {
    if (t_->dirtied_local_settings && !t_->sent_local_settings) {
      grpc_slice_buffer_add(
          t_->outbuf.c_slice_buffer(),
          grpc_chttp2_settings_create(t_->settings[GRPC_SENT_SETTINGS],
                                      t_->settings[GRPC_LOCAL_SETTINGS],
                                      t_->force_send_settings,
                                      GRPC_CHTTP2_NUM_SETTINGS));
      t_->force_send_settings = false;
      t_->dirtied_local_settings = false;
      t_->sent_local_settings = true;
      t_->flow_control.FlushedSettings();
      grpc_core::global_stats().IncrementHttp2SettingsWrites();
    }
  }

  void FlushQueuedBuffers() {
    // simple writes are queued to qbuf, and flushed here
    grpc_slice_buffer_move_into(&t_->qbuf, t_->outbuf.c_slice_buffer());
    t_->num_pending_induced_frames = 0;
    GPR_ASSERT(t_->qbuf.count == 0);
  }

  void FlushWindowUpdates() {
    uint32_t transport_announce = t_->flow_control.MaybeSendUpdate(
        t_->outbuf.c_slice_buffer()->count > 0);
    if (transport_announce) {
      grpc_transport_one_way_stats throwaway_stats;
      grpc_slice_buffer_add(t_->outbuf.c_slice_buffer(),
                            grpc_chttp2_window_update_create(
                                0, transport_announce, &throwaway_stats));
      grpc_chttp2_reset_ping_clock(t_);
    }
  }

  void FlushPingAcks() {
    for (size_t i = 0; i < t_->ping_ack_count; i++) {
      grpc_slice_buffer_add(t_->outbuf.c_slice_buffer(),
                            grpc_chttp2_ping_create(true, t_->ping_acks[i]));
    }
    t_->ping_ack_count = 0;
  }

  void EnactHpackSettings() {
    t_->hpack_compressor.SetMaxTableSize(
        t_->settings[GRPC_PEER_SETTINGS]
                    [GRPC_CHTTP2_SETTINGS_HEADER_TABLE_SIZE]);
  }

  void UpdateStreamsNoLongerStalled() {
    grpc_chttp2_stream* s;
    while (grpc_chttp2_list_pop_stalled_by_transport(t_, &s)) {
      if (t_->closed_with_error.ok() &&
          grpc_chttp2_list_add_writable_stream(t_, s)) {
        if (!s->refcount->refs.RefIfNonZero()) {
          grpc_chttp2_list_remove_writable_stream(t_, s);
        }
      }
    }
  }

  grpc_chttp2_stream* NextStream() {
    if (t_->outbuf.c_slice_buffer()->length > target_write_size(t_)) {
      result_.partial = true;
      return nullptr;
    }

    grpc_chttp2_stream* s;
    if (!grpc_chttp2_list_pop_writable_stream(t_, &s)) {
      return nullptr;
    }

    return s;
  }

  void IncInitialMetadataWrites() { ++initial_metadata_writes_; }
  void IncWindowUpdateWrites() { ++flow_control_writes_; }
  void IncMessageWrites() { ++message_writes_; }
  void IncTrailingMetadataWrites() { ++trailing_metadata_writes_; }

  void NoteScheduledResults() { result_.early_results_scheduled = true; }

  grpc_chttp2_transport* transport() const { return t_; }

  grpc_chttp2_begin_write_result Result() {
    result_.writing = t_->outbuf.c_slice_buffer()->count > 0;
    return result_;
  }

 private:
  grpc_chttp2_transport* const t_;

  // stats histogram counters: we increment these throughout this function,
  // and at the end publish to the central stats histograms
  int flow_control_writes_ = 0;
  int initial_metadata_writes_ = 0;
  int trailing_metadata_writes_ = 0;
  int message_writes_ = 0;
  grpc_chttp2_begin_write_result result_ = {false, false, false};
};

class DataSendContext {
 public:
  DataSendContext(WriteContext* write_context, grpc_chttp2_transport* t,
                  grpc_chttp2_stream* s)
      : write_context_(write_context),
        t_(t),
        s_(s),
        sending_bytes_before_(s_->sending_bytes) {}

  uint32_t stream_remote_window() const {
    return static_cast<uint32_t>(std::max(
        int64_t{0},
        s_->flow_control.remote_window_delta() +
            static_cast<int64_t>(
                t_->settings[GRPC_PEER_SETTINGS]
                            [GRPC_CHTTP2_SETTINGS_INITIAL_WINDOW_SIZE])));
  }

  uint32_t max_outgoing() const {
    return static_cast<uint32_t>(std::min(
        t_->settings[GRPC_PEER_SETTINGS][GRPC_CHTTP2_SETTINGS_MAX_FRAME_SIZE],
        static_cast<uint32_t>(
            std::min(static_cast<int64_t>(stream_remote_window()),
                     t_->flow_control.remote_window()))));
  }

  bool AnyOutgoing() const { return max_outgoing() > 0; }

  void FlushBytes() {
    uint32_t send_bytes =
        static_cast<uint32_t>(std::min(static_cast<size_t>(max_outgoing()),
                                       s_->flow_controlled_buffer.length));
    is_last_frame_ = send_bytes == s_->flow_controlled_buffer.length &&
                     s_->send_trailing_metadata != nullptr &&
                     s_->send_trailing_metadata->empty();
    grpc_chttp2_encode_data(s_->id, &s_->flow_controlled_buffer, send_bytes,
                            is_last_frame_, &s_->stats.outgoing,
                            t_->outbuf.c_slice_buffer());
    sfc_upd_.SentData(send_bytes);
    s_->sending_bytes += send_bytes;
  }

  bool is_last_frame() const { return is_last_frame_; }

  void CallCallbacks() {
    if (update_list(
            t_, s_,
            static_cast<int64_t>(s_->sending_bytes - sending_bytes_before_),
            &s_->on_flow_controlled_cbs, &s_->flow_controlled_bytes_flowed,
            absl::OkStatus())) {
      write_context_->NoteScheduledResults();
    }
  }

 private:
  WriteContext* write_context_;
  grpc_chttp2_transport* t_;
  grpc_chttp2_stream* s_;
  grpc_core::chttp2::StreamFlowControl::OutgoingUpdateContext sfc_upd_{
      &s_->flow_control};
  const size_t sending_bytes_before_;
  bool is_last_frame_ = false;
};

class StreamWriteContext {
 public:
  StreamWriteContext(WriteContext* write_context, grpc_chttp2_stream* s)
      : write_context_(write_context), t_(write_context->transport()), s_(s) {
    GRPC_CHTTP2_IF_TRACING(
        gpr_log(GPR_INFO, "W:%p %s[%d] im-(sent,send)=(%d,%d)", t_,
                t_->is_client ? "CLIENT" : "SERVER", s->id,
                s->sent_initial_metadata, s->send_initial_metadata != nullptr));
  }

  void FlushInitialMetadata() {
    // send initial metadata if it's available
    if (s_->sent_initial_metadata) return;
    if (s_->send_initial_metadata == nullptr) return;

    // We skip this on the server side if there is no custom initial
    // metadata, there are no messages to send, and we are also sending
    // trailing metadata.  This results in a Trailers-Only response,
    // which is required for retries, as per:
    // https://github.com/grpc/proposal/blob/master/A6-client-retries.md#when-retries-are-valid
    if (!t_->is_client && s_->flow_controlled_buffer.length == 0 &&
        s_->send_trailing_metadata != nullptr &&
        is_default_initial_metadata(s_->send_initial_metadata)) {
      ConvertInitialMetadataToTrailingMetadata();
    } else {
      t_->hpack_compressor.EncodeHeaders(
          grpc_core::HPackCompressor::EncodeHeaderOptions{
              s_->id,  // stream_id
              false,   // is_eof
              t_->settings
                      [GRPC_PEER_SETTINGS]
                      [GRPC_CHTTP2_SETTINGS_GRPC_ALLOW_TRUE_BINARY_METADATA] !=
                  0,  // use_true_binary_metadata
              t_->settings
                  [GRPC_PEER_SETTINGS]
                  [GRPC_CHTTP2_SETTINGS_MAX_FRAME_SIZE],  // max_frame_size
              &s_->stats.outgoing                         // stats
          },
          *s_->send_initial_metadata, t_->outbuf.c_slice_buffer());
      grpc_chttp2_reset_ping_clock(t_);
      write_context_->IncInitialMetadataWrites();
    }

    s_->send_initial_metadata = nullptr;
    s_->sent_initial_metadata = true;
    write_context_->NoteScheduledResults();
    grpc_chttp2_complete_closure_step(
        t_, s_, &s_->send_initial_metadata_finished, absl::OkStatus(),
        "send_initial_metadata_finished");
  }

  void FlushWindowUpdates() {
    if (s_->read_closed) return;

    // send any window updates
    const uint32_t stream_announce = s_->flow_control.MaybeSendUpdate();
    if (stream_announce == 0) return;

    grpc_slice_buffer_add(t_->outbuf.c_slice_buffer(),
                          grpc_chttp2_window_update_create(
                              s_->id, stream_announce, &s_->stats.outgoing));
    grpc_chttp2_reset_ping_clock(t_);
    write_context_->IncWindowUpdateWrites();
  }

  void FlushData() {
    if (!s_->sent_initial_metadata) return;

    if (s_->flow_controlled_buffer.length == 0) {
      return;  // early out: nothing to do
    }

    DataSendContext data_send_context(write_context_, t_, s_);

    if (!data_send_context.AnyOutgoing()) {
      if (t_->flow_control.remote_window() <= 0) {
        grpc_core::global_stats().IncrementHttp2TransportStalls();
        report_stall(t_, s_, "transport");
        grpc_chttp2_list_add_stalled_by_transport(t_, s_);
      } else if (data_send_context.stream_remote_window() <= 0) {
        grpc_core::global_stats().IncrementHttp2StreamStalls();
        report_stall(t_, s_, "stream");
        grpc_chttp2_list_add_stalled_by_stream(t_, s_);
      }
      return;  // early out: nothing to do
    }

    while (s_->flow_controlled_buffer.length > 0 &&
           data_send_context.max_outgoing() > 0) {
      data_send_context.FlushBytes();
    }
    grpc_chttp2_reset_ping_clock(t_);
    if (data_send_context.is_last_frame()) {
      SentLastFrame();
    }
    data_send_context.CallCallbacks();
    stream_became_writable_ = true;
    if (s_->flow_controlled_buffer.length > 0) {
      GRPC_CHTTP2_STREAM_REF(s_, "chttp2_writing:fork");
      grpc_chttp2_list_add_writable_stream(t_, s_);
    }
    write_context_->IncMessageWrites();
  }

  void FlushTrailingMetadata() {
    if (!s_->sent_initial_metadata) return;

    if (s_->send_trailing_metadata == nullptr) return;
    if (s_->flow_controlled_buffer.length != 0) return;

    GRPC_CHTTP2_IF_TRACING(gpr_log(GPR_INFO, "sending trailing_metadata"));
    if (s_->send_trailing_metadata->empty()) {
      grpc_chttp2_encode_data(s_->id, &s_->flow_controlled_buffer, 0, true,
                              &s_->stats.outgoing, t_->outbuf.c_slice_buffer());
    } else {
      if (send_status_.has_value()) {
        s_->send_trailing_metadata->Set(grpc_core::HttpStatusMetadata(),
                                        *send_status_);
      }
      if (send_content_type_.has_value()) {
        s_->send_trailing_metadata->Set(grpc_core::ContentTypeMetadata(),
                                        *send_content_type_);
      }
      t_->hpack_compressor.EncodeHeaders(
          grpc_core::HPackCompressor::EncodeHeaderOptions{
              s_->id, true,
              t_->settings
                      [GRPC_PEER_SETTINGS]
                      [GRPC_CHTTP2_SETTINGS_GRPC_ALLOW_TRUE_BINARY_METADATA] !=
                  0,
              t_->settings[GRPC_PEER_SETTINGS]
                          [GRPC_CHTTP2_SETTINGS_MAX_FRAME_SIZE],
              &s_->stats.outgoing},
          *s_->send_trailing_metadata, t_->outbuf.c_slice_buffer());
    }
    write_context_->IncTrailingMetadataWrites();
    grpc_chttp2_reset_ping_clock(t_);
    SentLastFrame();

    write_context_->NoteScheduledResults();
    grpc_chttp2_complete_closure_step(
        t_, s_, &s_->send_trailing_metadata_finished, absl::OkStatus(),
        "send_trailing_metadata_finished");
  }

  bool stream_became_writable() { return stream_became_writable_; }

 private:
  void ConvertInitialMetadataToTrailingMetadata() {
    GRPC_CHTTP2_IF_TRACING(
        gpr_log(GPR_INFO, "not sending initial_metadata (Trailers-Only)"));
    // When sending Trailers-Only, we need to move the :status and
    // content-type headers to the trailers.
    send_status_ =
        s_->send_initial_metadata->get(grpc_core::HttpStatusMetadata());
    send_content_type_ =
        s_->send_initial_metadata->get(grpc_core::ContentTypeMetadata());
  }

  void SentLastFrame() {
    s_->send_trailing_metadata = nullptr;
    if (s_->sent_trailing_metadata_op) {
      *s_->sent_trailing_metadata_op = true;
      s_->sent_trailing_metadata_op = nullptr;
    }
    s_->sent_trailing_metadata = true;
    s_->eos_sent = true;

    if (!t_->is_client && !s_->read_closed) {
      grpc_slice_buffer_add(
          t_->outbuf.c_slice_buffer(),
          grpc_chttp2_rst_stream_create(s_->id, GRPC_HTTP2_NO_ERROR,
                                        &s_->stats.outgoing));
    }
    grpc_chttp2_mark_stream_closed(t_, s_, !t_->is_client, true,
                                   absl::OkStatus());
  }

  WriteContext* const write_context_;
  grpc_chttp2_transport* const t_;
  grpc_chttp2_stream* const s_;
  bool stream_became_writable_ = false;
  absl::optional<uint32_t> send_status_;
  absl::optional<grpc_core::ContentTypeMetadata::ValueType> send_content_type_ =
      {};
};
}  // namespace

grpc_chttp2_begin_write_result grpc_chttp2_begin_write(
    grpc_chttp2_transport* t) {
  int64_t outbuf_relative_start_pos = 0;
  WriteContext ctx(t);
  ctx.FlushSettings();
  ctx.FlushPingAcks();
  ctx.FlushQueuedBuffers();
  ctx.EnactHpackSettings();

  if (t->flow_control.remote_window() > 0) {
    ctx.UpdateStreamsNoLongerStalled();
  }

  // for each grpc_chttp2_stream that's become writable, frame it's data
  // (according to available window sizes) and add to the output buffer
  while (grpc_chttp2_stream* s = ctx.NextStream()) {
    StreamWriteContext stream_ctx(&ctx, s);
    size_t orig_len = t->outbuf.c_slice_buffer()->length;
    int64_t num_stream_bytes = 0;
    stream_ctx.FlushInitialMetadata();
    stream_ctx.FlushWindowUpdates();
    stream_ctx.FlushData();
    stream_ctx.FlushTrailingMetadata();
    if (t->outbuf.c_slice_buffer()->length > orig_len) {
      // Add this stream to the list of the contexts to be traced at TCP
      num_stream_bytes = t->outbuf.c_slice_buffer()->length - orig_len;
      s->byte_counter += static_cast<size_t>(num_stream_bytes);
      if (s->traced && grpc_endpoint_can_track_err(t->ep)) {
        grpc_core::CopyContextFn copy_context_fn =
            grpc_core::GrpcHttp2GetCopyContextFn();
        if (copy_context_fn != nullptr &&
            grpc_core::GrpcHttp2GetWriteTimestampsCallback() != nullptr) {
          t->cl->emplace_back(copy_context_fn(s->context),
                              outbuf_relative_start_pos, num_stream_bytes,
                              s->byte_counter);
        }
      }
      outbuf_relative_start_pos += num_stream_bytes;
    }
    if (stream_ctx.stream_became_writable()) {
      if (!grpc_chttp2_list_add_writing_stream(t, s)) {
        // already in writing list: drop ref
        GRPC_CHTTP2_STREAM_UNREF(s, "chttp2_writing:already_writing");
      } else {
        // ref will be dropped at end of write
      }
    } else {
      GRPC_CHTTP2_STREAM_UNREF(s, "chttp2_writing:no_write");
    }
  }

  ctx.FlushWindowUpdates();

  maybe_initiate_ping(t);

  return ctx.Result();
}

void grpc_chttp2_end_write(grpc_chttp2_transport* t, grpc_error_handle error) {
  grpc_chttp2_stream* s;

  if (t->channelz_socket != nullptr) {
    t->channelz_socket->RecordMessagesSent(t->num_messages_in_next_write);
  }
  t->num_messages_in_next_write = 0;

  while (grpc_chttp2_list_pop_writing_stream(t, &s)) {
    if (s->sending_bytes != 0) {
      update_list(t, s, static_cast<int64_t>(s->sending_bytes),
                  &s->on_write_finished_cbs, &s->flow_controlled_bytes_written,
                  error);
      s->sending_bytes = 0;
    }
    GRPC_CHTTP2_STREAM_UNREF(s, "chttp2_writing:end");
  }
  grpc_slice_buffer_reset_and_unref(t->outbuf.c_slice_buffer());
}
