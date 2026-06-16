/*

TERMINAL1:

g++ device.cpp \
-std=c++17 \
-lmosquitto \
-o device

./device

TERMINAL2:

mosquitto_sub -t "iot/test"





*/

#include <mosquitto.h>

#include <iostream>
#include <thread>
#include <chrono>
#include <random>


int main()
{
    mosquitto_lib_init();


    mosquitto* client =
        mosquitto_new(
            "virtual_sensor",
            true,
            nullptr
        );


    if(!client)
    {
        std::cerr<<"MQTT init failed\n";
        return 1;
    }


    if(
        mosquitto_connect(
            client,
            "localhost",
            1883,
            60
        )
        != 0
    )
    {
        std::cerr<<"MQTT connection failed\n";
        return 1;
    }


    std::random_device rd;
    std::mt19937 gen(rd());

    std::uniform_real_distribution<float> temp(
        20.0,
        35.0
    );


    while(true)
    {
        float temperature = temp(gen);


        std::string payload =
            "{ \"device\":\"laptop01\", "
            "\"temperature\":"+
            std::to_string(temperature)+
            " }";


        mosquitto_publish(
            client,
            nullptr,
            "home/livingroom/temp",
            payload.size(),
            payload.c_str(),
            0,
            false
        );


        std::cout
            <<"Sent: "
            <<payload
            <<"\n";


        std::this_thread::sleep_for(
            std::chrono::seconds(2)
        );
    }


    mosquitto_destroy(client);
    mosquitto_lib_cleanup();
}
