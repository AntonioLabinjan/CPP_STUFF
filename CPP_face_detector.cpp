#include <opencv2/opencv.hpp>
#include <iostream>
#include <chrono>

int main()
{
    cv::VideoCapture cap(0);

    if (!cap.isOpened()) {
        std::cerr << "Cannot open camera\n";
        return -1;
    }

    cv::CascadeClassifier face_cascade;

    if (!face_cascade.load(
        "/usr/share/opencv4/haarcascades/haarcascade_frontalface_default.xml"))
    {
        std::cerr << "Cannot load Haar cascade\n";
        return -1;
    }

    cv::Mat frame;
    cv::Mat gray;

    auto last = std::chrono::high_resolution_clock::now();

    while (true)
    {
        cap >> frame;

        if (frame.empty())
            break;

        cv::cvtColor(frame, gray, cv::COLOR_BGR2GRAY);

        std::vector<cv::Rect> faces;

        face_cascade.detectMultiScale(
            gray,
            faces,
            1.1,
            5,
            0,
            cv::Size(40,40)
        );


        for (auto& face : faces)
        {
            cv::rectangle(
                frame,
                face,
                cv::Scalar(0,255,0),
                2
            );
        }


        auto now = std::chrono::high_resolution_clock::now();

        double fps =
            1.0 /
            std::chrono::duration<double>(now-last).count();

        last = now;


        cv::putText(
            frame,
            "FPS: " + std::to_string((int)fps),
            cv::Point(20,40),
            cv::FONT_HERSHEY_SIMPLEX,
            1,
            cv::Scalar(0,0,255),
            2
        );


        cv::imshow("C++ Computer Vision", frame);


        if (cv::waitKey(1) == 'q')
            break;
    }


    return 0;
}
