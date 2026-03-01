/*
 * Copyright 2026 Rockchip Electronics Co., Ltd
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gstmppfpsdrop.h"

#define GST_CAT_DEFAULT mpp_fps_drop_debug
GST_DEBUG_CATEGORY_STATIC (GST_CAT_DEFAULT);

#define GST_MPP_FPS_DROP(obj) (G_TYPE_CHECK_INSTANCE_CAST((obj), \
    GST_TYPE_MPP_FPS_DROP, GstMppFpsDrop))

#define DEFAULT_PROP_MAX_FPS 0

struct _GstMppFpsDrop
{
  GstElement parent;

  GstPad *sinkpad;
  GstPad *srcpad;

  guint max_fps;
  GstClockTime frame_interval_ns;
  gboolean have_last_out_ts;
  GstClockTime last_out_ts;
};

G_DEFINE_TYPE (GstMppFpsDrop, gst_mpp_fps_drop, GST_TYPE_ELEMENT);

enum
{
  PROP_0,
  PROP_MAX_FPS,
  PROP_LAST
};

static GstStaticPadTemplate gst_mpp_fps_drop_sink_template =
GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("video/x-raw; video/x-raw(memory:DMABuf)"));

static GstStaticPadTemplate gst_mpp_fps_drop_src_template =
GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("video/x-raw; video/x-raw(memory:DMABuf)"));

static inline void
gst_mpp_fps_drop_reset_timing (GstMppFpsDrop * self)
{
  self->have_last_out_ts = FALSE;
  self->last_out_ts = GST_CLOCK_TIME_NONE;
}

static inline void
gst_mpp_fps_drop_update_interval (GstMppFpsDrop * self)
{
  if (!self->max_fps) {
    self->frame_interval_ns = GST_CLOCK_TIME_NONE;
    return;
  }

  self->frame_interval_ns = gst_util_uint64_scale_int (GST_SECOND, 1,
      (gint) self->max_fps);
}

static void
gst_mpp_fps_drop_set_property (GObject * object,
    guint prop_id, const GValue * value, GParamSpec * pspec)
{
  GstMppFpsDrop *self = GST_MPP_FPS_DROP (object);

  switch (prop_id) {
    case PROP_MAX_FPS:
      GST_OBJECT_LOCK (self);
      self->max_fps = g_value_get_uint (value);
      gst_mpp_fps_drop_update_interval (self);
      gst_mpp_fps_drop_reset_timing (self);
      GST_OBJECT_UNLOCK (self);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_mpp_fps_drop_get_property (GObject * object,
    guint prop_id, GValue * value, GParamSpec * pspec)
{
  GstMppFpsDrop *self = GST_MPP_FPS_DROP (object);

  switch (prop_id) {
    case PROP_MAX_FPS:
      GST_OBJECT_LOCK (self);
      g_value_set_uint (value, self->max_fps);
      GST_OBJECT_UNLOCK (self);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static gboolean
gst_mpp_fps_drop_sink_event (GstPad * pad, GstObject * parent, GstEvent * event)
{
  GstMppFpsDrop *self = GST_MPP_FPS_DROP (parent);

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_FLUSH_STOP:
    case GST_EVENT_SEGMENT:
      GST_OBJECT_LOCK (self);
      gst_mpp_fps_drop_reset_timing (self);
      GST_OBJECT_UNLOCK (self);
      break;
    default:
      break;
  }

  return gst_pad_event_default (pad, parent, event);
}

static GstFlowReturn
gst_mpp_fps_drop_chain (GstPad * pad UNUSED, GstObject * parent,
    GstBuffer * inbuf)
{
  GstMppFpsDrop *self = GST_MPP_FPS_DROP (parent);
  GstClockTime interval;
  GstClockTime ts;
  gboolean keep = TRUE;

  GST_OBJECT_LOCK (self);
  interval = self->frame_interval_ns;
  GST_OBJECT_UNLOCK (self);

  if (interval == GST_CLOCK_TIME_NONE)
    return gst_pad_push (self->srcpad, inbuf);

  ts = GST_BUFFER_PTS (inbuf);
  if (!GST_CLOCK_TIME_IS_VALID (ts))
    ts = GST_BUFFER_DTS (inbuf);

  if (GST_CLOCK_TIME_IS_VALID (ts)) {
    GST_OBJECT_LOCK (self);
    if (!self->have_last_out_ts) {
      self->have_last_out_ts = TRUE;
      self->last_out_ts = ts;
    } else if (ts < self->last_out_ts) {
      self->last_out_ts = ts;
    } else {
      GstClockTimeDiff delta = GST_CLOCK_DIFF (self->last_out_ts, ts);
      if (delta < (GstClockTimeDiff) interval)
        keep = FALSE;
      else
        self->last_out_ts = ts;
    }
    GST_OBJECT_UNLOCK (self);
  }

  if (!keep) {
    GST_LOG_OBJECT (self, "dropping frame pts=%" GST_TIME_FORMAT,
        GST_TIME_ARGS (ts));
    gst_buffer_unref (inbuf);
    return GST_FLOW_OK;
  }

  return gst_pad_push (self->srcpad, inbuf);
}

static void
gst_mpp_fps_drop_dispose (GObject * object)
{
  GstMppFpsDrop *self = GST_MPP_FPS_DROP (object);

  gst_clear_object (&self->sinkpad);
  gst_clear_object (&self->srcpad);

  G_OBJECT_CLASS (gst_mpp_fps_drop_parent_class)->dispose (object);
}

static void
gst_mpp_fps_drop_class_init (GstMppFpsDropClass * klass)
{
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  GST_DEBUG_CATEGORY_INIT (GST_CAT_DEFAULT, "mppfpsdrop", 0,
      "MPP FPS Drop");

  gobject_class->set_property = gst_mpp_fps_drop_set_property;
  gobject_class->get_property = gst_mpp_fps_drop_get_property;
  gobject_class->dispose = gst_mpp_fps_drop_dispose;

  g_object_class_install_property (gobject_class, PROP_MAX_FPS,
      g_param_spec_uint ("max-fps", "Max FPS",
          "Limit output frame rate by dropping input frames (0 = unlimited)",
          0, G_MAXUINT, DEFAULT_PROP_MAX_FPS,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&gst_mpp_fps_drop_sink_template));
  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&gst_mpp_fps_drop_src_template));

  gst_element_class_set_static_metadata (element_class,
      "Rockchip MPP FPS Drop Filter", "Filter/Video",
      "Limits frame rate by dropping frames while preserving DMABuf passthrough",
      "Rockchip");
}

static void
gst_mpp_fps_drop_init (GstMppFpsDrop * self)
{
  self->max_fps = DEFAULT_PROP_MAX_FPS;
  gst_mpp_fps_drop_update_interval (self);
  gst_mpp_fps_drop_reset_timing (self);

  self->sinkpad =
      gst_pad_new_from_static_template (&gst_mpp_fps_drop_sink_template, "sink");
  self->srcpad =
      gst_pad_new_from_static_template (&gst_mpp_fps_drop_src_template, "src");

  gst_pad_set_chain_function (self->sinkpad, gst_mpp_fps_drop_chain);
  gst_pad_set_event_function (self->sinkpad, gst_mpp_fps_drop_sink_event);

  GST_PAD_SET_PROXY_CAPS (self->sinkpad);
  GST_PAD_SET_PROXY_CAPS (self->srcpad);
  GST_PAD_SET_PROXY_ALLOCATION (self->sinkpad);
  GST_PAD_SET_PROXY_ALLOCATION (self->srcpad);

  gst_element_add_pad (GST_ELEMENT (self), gst_object_ref (self->sinkpad));
  gst_element_add_pad (GST_ELEMENT (self), gst_object_ref (self->srcpad));
}

gboolean
gst_mpp_fps_drop_register (GstPlugin * plugin, guint rank)
{
  return gst_element_register (plugin, "mppfpsdrop", rank,
      GST_TYPE_MPP_FPS_DROP);
}
