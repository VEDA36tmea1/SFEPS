#include <gst/gst.h>
#include <iostream>
#include "log.h"

#define RTSP_URL "rtsp://192.168.0.XX:8554/stream"

struct CustomData {
    GMainLoop *loop;
    DBLogger *logger; // 클래스명 변경 반영
};

static gboolean bus_call(GstBus *bus, GstMessage *msg, gpointer user_data) {
    CustomData *data = (CustomData *) user_data;

    switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_EOS:
            std::cout << "End of stream" << std::endl;
            data->logger->enqueue("INFO", "Stream Ended (EOS)");
            g_main_loop_quit(data->loop);
            break;

        case GST_MESSAGE_ERROR: {
            gchar *debug;
            GError *error;
            gst_message_parse_error(msg, &error, &debug);
            
            std::cerr << "Error: " << error->message << std::endl;
            data->logger->enqueue("ERROR", error->message);

            g_error_free(error);
            g_free(debug);
            g_main_loop_quit(data->loop);
            break;
        }
        
        case GST_MESSAGE_STATE_CHANGED: {
            GstState old_state, new_state, pending_state;
            gst_message_parse_state_changed(msg, &old_state, &new_state, &pending_state);
            
            if (GST_MESSAGE_SRC(msg) == GST_OBJECT_PARENT(bus) && new_state == GST_STATE_PLAYING) {
                data->logger->enqueue("STATUS", "RTSP Stream Started");
            }
            break;
        }
        default: break;
    }
    return TRUE;
}

int main(int argc, char *argv[]) {
    // 1. DBLogger 생성
    DBLogger myLogger;
    if (!myLogger.connect()) {
        std::cerr << "DB Init Failed." << std::endl;
        return -1;
    }
    
    myLogger.enqueue("SYSTEM", "SFEPS Server Started");

    // 2. GStreamer 설정
    GMainLoop *loop;
    GstElement *pipeline;
    GstBus *bus;
    guint bus_watch_id;

    gst_init(&argc, &argv);
    loop = g_main_loop_new(NULL, FALSE);

    CustomData data;
    data.loop = loop;
    data.logger = &myLogger;

    pipeline = gst_element_factory_make("playbin", "player");
    if (!pipeline) return -1;

    g_object_set(G_OBJECT(pipeline), "uri", RTSP_URL, NULL);

    bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline));
    bus_watch_id = gst_bus_add_watch(bus, bus_call, &data);
    gst_object_unref(bus);

    std::cout << "RTSP Streaming Start..." << std::endl;
    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    g_main_loop_run(loop);

    std::cout << "Stopping..." << std::endl;
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(GST_OBJECT(pipeline));
    g_source_remove(bus_watch_id);
    g_main_loop_unref(loop);

    return 0;
}