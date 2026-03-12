/*
 * GStreamer filter element: laserdetect
 * Receives video/x-raw, format=BGR, runs laser detection and draws a red circle on the spot.
 * Use in pipeline: ... ! videoconvert ! video/x-raw,format=BGR ! laserdetect ! ...
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <gst/gst.h>
#include <gst/video/video.h>
#include <gst/video/gstvideofilter.h>
#include <string.h>

#include "laserdetect_process.h"

GST_DEBUG_CATEGORY_STATIC (gst_laserdetect_debug);
#define GST_CAT_DEFAULT gst_laserdetect_debug

typedef struct _GstLaserdetect GstLaserdetect;
typedef struct _GstLaserdetectClass GstLaserdetectClass;

struct _GstLaserdetect
{
    GstVideoFilter parent;
};

struct _GstLaserdetectClass
{
    GstVideoFilterClass parent_class;
};

#define gst_laserdetect_parent_class parent_class
G_DEFINE_TYPE (GstLaserdetect, gst_laserdetect, GST_TYPE_VIDEO_FILTER);

static GstFlowReturn
gst_laserdetect_transform_frame_ip (GstVideoFilter* filter, GstVideoFrame* frame)
{
    int stride = GST_VIDEO_FRAME_PLANE_STRIDE (frame, 0);

    laserdetect_process (
        (unsigned char*) GST_VIDEO_FRAME_PLANE_DATA (frame, 0),
        GST_VIDEO_FRAME_WIDTH (frame),
        GST_VIDEO_FRAME_HEIGHT (frame),
        stride
    );
    return GST_FLOW_OK;
}

static void
gst_laserdetect_class_init (GstLaserdetectClass* klass)
{
    GstElementClass* element_class = GST_ELEMENT_CLASS (klass);
    GstVideoFilterClass* filter_class = GST_VIDEO_FILTER_CLASS (klass);

    filter_class->transform_frame_ip = gst_laserdetect_transform_frame_ip;

    gst_element_class_set_static_metadata (element_class,
        "Laser spot detector",
        "Filter/Effect/Video",
        "Detect red laser spot and draw overlay",
        "rtsp_laser_demo");

    GstCaps* caps = gst_caps_new_simple ("video/x-raw",
        "format", G_TYPE_STRING, "BGR",
        NULL);
    GstPadTemplate* sink_templ = gst_pad_template_new ("sink",
        GST_PAD_SINK, GST_PAD_ALWAYS, caps);
    GstPadTemplate* src_templ = gst_pad_template_new ("src",
        GST_PAD_SRC, GST_PAD_ALWAYS, caps);
    gst_element_class_add_pad_template (element_class, sink_templ);
    gst_element_class_add_pad_template (element_class, src_templ);
    gst_caps_unref (caps);
}

static void
gst_laserdetect_init (GstLaserdetect* self)
{
    (void) self;
}

static gboolean
plugin_init (GstPlugin* plugin)
{
    GST_DEBUG_CATEGORY_INIT (gst_laserdetect_debug, "laserdetect", 0, "Laser detect filter");

    return gst_element_register (plugin, "laserdetect",
        GST_RANK_NONE, gst_laserdetect_get_type ());
}

#ifndef PACKAGE
#define PACKAGE "rtsp_laser_demo"
#endif
#ifndef PACKAGE_VERSION
#define PACKAGE_VERSION "1.0"
#endif

GST_PLUGIN_DEFINE (
    GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    laserdetect,
    "Laser spot detection filter",
    plugin_init,
    PACKAGE_VERSION,
    "LGPL",
    "rtsp_laser_demo",
    "https://github.com"
)
