// SuedeServer.cpp : This file contains the 'main' function. Program execution begins and ends there.

#include <iostream>
#include <string>

// header file includes
#include "../SuedeServer/Server_Core/UseServer.h"

int main()
{
    bool use_public = false;
    int port = 8080;
    std::string err;
    std::cout << "Starting Suede Server on port " << port << std::endl;
    runSuedeServer(port, err, use_public);

}

