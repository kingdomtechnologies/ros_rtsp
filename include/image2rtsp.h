#ifndef IMAGE_TO_RTSP_H
#define IMAGE_TO_RTSP_H

namespace image2rtsp
{
    class Image2RTSPNodelet : public nodelet::Nodelet
    {
    public:
        GstRTSPServer *rtsp_server;
        void onInit();
        void url_connected(std::string url);
        void url_disconnected(std::string url);
        void print_info(char *s);
        void print_error(char *s);

    private:
        bool is_all_empty = true; // to know when all streams (ros topics for now) has no subscribers
        bool monitoring_state = false;
        
        ros::Timer timer;
        std::string port;
        const int TIMEOUT_THRESHOLD = 5000; // 5 seconds TODO: make this a ROS param
        XmlRpc::XmlRpcValue streams;
        ros::Publisher client_bool_pub;
        std::map<std::string, ros::Subscriber> subs;
        std::map<std::string, GstAppSrc *> appsrc;
        std::map<std::string, int> num_of_clients;
        std::map<std::string, std::chrono::time_point<std::chrono::steady_clock>> watchdog_timers;
        GstCaps *gst_caps_new_from_image(const sensor_msgs::Image::ConstPtr &msg);
        void imageCallback(const sensor_msgs::Image::ConstPtr &msg, const std::string &topic);
        void video_mainloop_start();
        void printStreamsClients();
        void timerCallback(const ros::TimerEvent&);        

        void rtsp_server_add_url(const char *url, const char *sPipeline, GstElement **appsrc);

        GstRTSPServer *rtsp_server_create(const std::string &port);
    };
}

#endif
