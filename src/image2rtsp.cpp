#include <chrono>
#include <string>
#include <stdio.h>
#include <nodelet/nodelet.h>
#include <pluginlib/class_list_macros.h>
#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <gst/rtsp-server/rtsp-server.h>
#include <ros/ros.h>
#include "std_msgs/Bool.h"
#include "sensor_msgs/Image.h"
#include <sensor_msgs/image_encodings.h>
#include <image2rtsp.h>

using namespace std;
using namespace image2rtsp;






// TODO: make a watchdog for subscribed topics incase they die in the middle of a stream to unregister the client, close the stream (if that's the last client),
// and lower the passive monitoring flag

//TODO: handle when there are issues with image subcription that causes client stream to close on their end, but the flag is still raised and the ros sub is still up



void Image2RTSPNodelet::timerCallback(const ros::TimerEvent&)
{
    // Check each timer in the map
    for (auto& kv : watchdog_timers)
    {
        if(num_of_clients[kv.first]<1) // no clients here
            continue;
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - kv.second).count();

        if (elapsed > TIMEOUT_THRESHOLD)
        {
            NODELET_ERROR( "Watchdog timer '%s' has timed out!",kv.first.c_str() );
            num_of_clients[kv.first] = 0;
            NODELET_INFO("Shutting down the topic for %s", kv.first.c_str());
            //Stop the subscription because the topic is dead.
            subs[kv.first].shutdown();
            appsrc[kv.first] = NULL;        
        }
    }

    std::string mountpoint;
    is_all_empty = true; // it's all empty until at least one stream proves it wrong 
    // Go through and parse each stream
    for (XmlRpc::XmlRpcValue::ValueStruct::const_iterator it = streams.begin(); it != streams.end(); ++it)
    {
        XmlRpc::XmlRpcValue stream = streams[it->first];
        mountpoint = static_cast<std::string>(stream["mountpoint"]);
        if (num_of_clients[mountpoint] > 0)
        {
            is_all_empty = false;
            break;
        }
    }

    if (is_all_empty)
    {
        // publishing a bool topic for AWS
        if (monitoring_state)
        {       
            NODELET_INFO("All streams have no subscribers Changing motnitoring flag to false");
            monitoring_state = false;
            std_msgs::Bool msg;
            msg.data = false;
            client_bool_pub.publish(msg);
        }
    }
        
}        

void Image2RTSPNodelet::onInit()
{
    string pipeline, mountpoint, bitrate, caps;
    string pipeline_tail = " key-int-max=30 ! video/x-h264, profile=baseline ! rtph264pay name=pay0 pt=96 )"; // Gets completed based on rosparams below

    NODELET_DEBUG("Initializing image2rtsp nodelet...");

    if (getenv((char *)"GST_DEBUG") == NULL)
    {
        // set GST_DEBUG to warning if unset
        putenv((char *)"GST_DEBUG=*:1");
    }

    ros::NodeHandle &nh = getPrivateNodeHandle();

    // Get the parameters from the rosparam server
    nh.getParam("streams", this->streams);
    ROS_ASSERT(streams.getType() == XmlRpc::XmlRpcValue::TypeStruct);
    ROS_DEBUG("Number of RTSP streams: %d", streams.size());
    nh.getParam("port", this->port);

    client_bool_pub = nh.advertise<std_msgs::Bool>("/autonomy/passive_monitoring", 10, true);

    std_msgs::Bool msg;
    msg.data = false;
    client_bool_pub.publish(msg);

    video_mainloop_start();
    rtsp_server = rtsp_server_create(port);

    // Go through and parse each stream
    for (XmlRpc::XmlRpcValue::ValueStruct::const_iterator it = streams.begin(); it != streams.end(); ++it)
    {
        XmlRpc::XmlRpcValue stream = streams[it->first];
        ROS_DEBUG_STREAM("Found stream: " << (std::string)(it->first) << " ==> " << stream);

        // Convert to string for ease of use
        mountpoint = static_cast<std::string>(stream["mountpoint"]);
        bitrate = std::to_string(static_cast<int>(stream["bitrate"]));

        // uvc camera?
        if (stream["type"] == "cam")
        {
            pipeline = "( " + static_cast<std::string>(stream["source"]) + " ! x264enc tune=zerolatency bitrate=" + bitrate + pipeline_tail;

            rtsp_server_add_url(mountpoint.c_str(), pipeline.c_str(), NULL);
        }
        // ROS Image topic?
        else if (stream["type"] == "topic")
        {
            /* Keep record of number of clients connected to each topic.
             * so we know to stop subscribing when no-one is connected. */
            num_of_clients[mountpoint] = 0;
            appsrc[mountpoint] = NULL;
            caps = static_cast<std::string>(stream["caps"]);

            // Setup the full pipeline
            pipeline = "( appsrc name=imagesrc do-timestamp=true min-latency=0 max-latency=0 max-bytes=1000 is-live=true ! videoconvert ! videoscale ! " + caps + " ! x264enc tune=zerolatency bitrate=" + bitrate + pipeline_tail;

            // Add the pipeline to the rtsp server
            rtsp_server_add_url(mountpoint.c_str(), pipeline.c_str(), (GstElement **)&(appsrc[mountpoint]));
        }
        else
        {
            ROS_ERROR("Undefined stream type. Check your stream_setup.yaml file.");
        }
        NODELET_INFO("Stream available at rtsp://%s:%s%s", gst_rtsp_server_get_address(rtsp_server), port.c_str(), mountpoint.c_str());
    }

    timer = getNodeHandle().createTimer(ros::Duration(1.0), &Image2RTSPNodelet::timerCallback, this);


    printStreamsClients();

}

/* Modified from https://github.com/ProjectArtemis/gst_video_server/blob/master/src/server_nodelet.cpp */
GstCaps *Image2RTSPNodelet::gst_caps_new_from_image(const sensor_msgs::Image::ConstPtr &msg)
{
    // http://gstreamer.freedesktop.org/data/doc/gstreamer/head/pwg/html/section-types-definitions.html
    static const ros::M_string known_formats = {{
        {sensor_msgs::image_encodings::RGB8, "RGB"},
        {sensor_msgs::image_encodings::RGB16, "RGB16"},
        {sensor_msgs::image_encodings::RGBA8, "RGBA"},
        {sensor_msgs::image_encodings::RGBA16, "RGBA16"},
        {sensor_msgs::image_encodings::BGR8, "BGR"},
        {sensor_msgs::image_encodings::BGR16, "BGR16"},
        {sensor_msgs::image_encodings::BGRA8, "BGRA"},
        {sensor_msgs::image_encodings::BGRA16, "BGRA16"},
        {sensor_msgs::image_encodings::MONO8, "GRAY8"},
        {sensor_msgs::image_encodings::MONO16, "GRAY16_LE"},
    }};

    if (msg->is_bigendian)
    {
        ROS_ERROR("GST: big endian image format is not supported");
        return nullptr;
    }

    auto format = known_formats.find(msg->encoding);
    if (format == known_formats.end())
    {
        ROS_ERROR_THROTTLE(5,"GST: image format '%s' unknown", msg->encoding.c_str());
        return nullptr;
    }

    return gst_caps_new_simple("video/x-raw",
                               "format", G_TYPE_STRING, format->second.c_str(),
                               "width", G_TYPE_INT, msg->width,
                               "height", G_TYPE_INT, msg->height,
                               "framerate", GST_TYPE_FRACTION, 10, 1,
                               nullptr);
}

void Image2RTSPNodelet::imageCallback(const sensor_msgs::Image::ConstPtr &msg, const std::string &topic)
{
    GstBuffer *buf;
    GstCaps *caps;
    char *gst_type, *gst_format = (char *)"";

    if (!monitoring_state)
    {
        monitoring_state = true;
        // publishing a bool topic for AWS
        NODELET_INFO("monitoring_state flag is raised");
        std_msgs::Bool msg;
        msg.data = true;
        client_bool_pub.publish(msg);
    }

    watchdog_timers[topic] = std::chrono::steady_clock::now(); //resetting the watchdog timer for this topic

    // g_print("Image encoding: %s\n", msg->encoding.c_str());
    if (appsrc[topic] != NULL)
    {
        // Set caps from message
        caps = gst_caps_new_from_image(msg);
        gst_app_src_set_caps(appsrc[topic], caps);

        buf = gst_buffer_new_allocate(nullptr, msg->data.size(), nullptr);
        gst_buffer_fill(buf, 0, msg->data.data(), msg->data.size());
        GST_BUFFER_FLAG_SET(buf, GST_BUFFER_FLAG_LIVE);

        gst_app_src_push_buffer(appsrc[topic], buf);
    }
}

void Image2RTSPNodelet::url_connected(string url)
{
    std::string mountpoint, source, type;

    NODELET_INFO("Client connected: %s", url.c_str());
    ros::NodeHandle &nh = getPrivateNodeHandle();

    // Go through and parse each stream
    for (XmlRpc::XmlRpcValue::ValueStruct::const_iterator it = streams.begin(); it != streams.end(); ++it)
    {
        XmlRpc::XmlRpcValue stream = streams[it->first];
        type = static_cast<std::string>(stream["type"]);
        mountpoint = static_cast<std::string>(stream["mountpoint"]);
        source = static_cast<std::string>(stream["source"]);

        // Check which stream the client has connected to
        if (type == "topic" && url == mountpoint)
        {

            if (num_of_clients[url] == 0)
            {
                // Subscribe to the ROS topic
                NODELET_INFO("First connection for stream %s",url.c_str());
                watchdog_timers[url] = std::chrono::steady_clock::now();

                subs[url] = nh.subscribe<sensor_msgs::Image>(source, 1, boost::bind(&Image2RTSPNodelet::imageCallback, this, _1, url));
            }
            num_of_clients[url]++;
            is_all_empty = false;
        }
    }

    printStreamsClients();
}


void Image2RTSPNodelet::printStreamsClients()
{
    std::string mountpoint;
    NODELET_INFO("Current clients for all streams:");
    for (XmlRpc::XmlRpcValue::ValueStruct::const_iterator it = streams.begin(); it != streams.end(); ++it)
    {
        XmlRpc::XmlRpcValue stream = streams[it->first];
        mountpoint = static_cast<std::string>(stream["mountpoint"]);        
        NODELET_INFO("%s: %i", mountpoint.c_str(), num_of_clients[mountpoint]);
    }

}

void Image2RTSPNodelet::url_disconnected(string url)
{
    std::string mountpoint;

    NODELET_INFO("Client disconnected: %s", url.c_str());
    ros::NodeHandle &nh = getPrivateNodeHandle();

    printStreamsClients();

    is_all_empty = true; // it's all empty until at least one stream proves it wrong 
    // Go through and parse each stream
    for (XmlRpc::XmlRpcValue::ValueStruct::const_iterator it = streams.begin(); it != streams.end(); ++it)
    {
        XmlRpc::XmlRpcValue stream = streams[it->first];
        mountpoint = static_cast<std::string>(stream["mountpoint"]);
        
        // Check which stream the client has disconnected from
        if (url == mountpoint)
        {
            if (num_of_clients[url] > 0)
            {
                num_of_clients[url]--;
            }
            if (num_of_clients[url] == 0)
            {
                NODELET_INFO("Shutting down the topic for %s", url.c_str());
                // No-one else is connected. Stop the subscription.
                subs[url].shutdown();
                appsrc[url] = NULL;
            }
        }
        if (num_of_clients[mountpoint] > 0)
            is_all_empty = false;
    }
}



void Image2RTSPNodelet::print_info(char *s)
{
    NODELET_INFO("%s", s);
}

void Image2RTSPNodelet::print_error(char *s)
{
    NODELET_ERROR("%s", s);
}

PLUGINLIB_EXPORT_CLASS(image2rtsp::Image2RTSPNodelet, nodelet::Nodelet)
