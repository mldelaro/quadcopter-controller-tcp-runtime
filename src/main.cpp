/*
 * Copyright (c) 2016 Intel Corporation.
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <iostream>
#include<stdio.h> //printf
#include<string.h> //memset
#include<stdlib.h> //exit(0);
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h> //close(s);
#include <boost/interprocess/shared_memory_object.hpp>
#include <boost/interprocess/mapped_region.hpp>
#include <netinet/tcp.h> // tcp_nodelay
#include <csignal> // for SIGNT

#include "./../include/json.hpp"

#define BUFLEN 512  //Max length of buffer
#define PORT 4444   //The port on which to listen for incoming data

using json = nlohmann::json;

int nSocket = -1;
char buffer[BUFLEN];
char buffer_old[BUFLEN];
boost::interprocess::mapped_region* regionRX;
boost::interprocess::mapped_region* regionTX;

void _SIG_HANDLER_ (int sig) {
	std::cout << "[TCP RUNTIME] Interrupt signal received " << sig << std::endl;

	if(nSocket != -1) {
		std::cout << "Closing socket..." << std::endl;
		close(nSocket);
	}

	std::cout << "Clearing buffer... " << std::endl;
	std::memset(buffer, '\0', BUFLEN); // clear buffer

	std::cout << "Clearing buffer copy..." << std::endl;
	std::memset(buffer_old, '\0', BUFLEN); // clear buffer

	if(regionRX) {
		std::cout << "Memset '0' for SHARED_RX_MEM..." << std::endl;
		std::memset(regionRX->get_address(), '\0', regionRX->get_size());
		std::string DISCONNECTED_MESSAGE = "{\"command\":\"neutral\"}\n";
		std::strcpy((char*)regionRX->get_address(), DISCONNECTED_MESSAGE.c_str());

		// Not deleted in case other processes (Flight Controller) are using RX Buffer Stream
		//delete regionRX;
	}

	if(regionTX) {
		std::cout << "Memset '0' for SHARED_TX_MEM..." << std::endl;
		std::memset(regionTX->get_address(), '\0', regionTX->get_size());
		delete regionTX;
	}

	exit(sig);
}

int main(void)
{
	// register SIGINT
	signal(SIGINT, _SIG_HANDLER_);
	signal(SIGTERM, _SIG_HANDLER_);

	std::cout << "Starting TCP Server..." << std::endl;
    struct sockaddr_in si_me, si_other;

    socklen_t slen = sizeof(si_other);

    // zero out the structure
    memset((char *) &si_me, 0, sizeof(si_me));

    si_me.sin_family = AF_INET;
    si_me.sin_port = htons(PORT);
    si_me.sin_addr.s_addr = htonl(INADDR_ANY);

	// create and clear space for shared memory - RECIEVER
	boost::interprocess::shared_memory_object sharedMemory_TcpRX(
			boost::interprocess::open_or_create,
			"shared_mem_tcp_RX",
			boost::interprocess::read_write
	);
	sharedMemory_TcpRX.truncate(BUFLEN);
	regionRX = new boost::interprocess::mapped_region(sharedMemory_TcpRX, boost::interprocess::read_write);
	std::memset(regionRX->get_address(), '\0', regionRX->get_size());

	// create and clear space for shared memory - SENDER
	boost::interprocess::shared_memory_object sharedMemory_TcpTX(
			boost::interprocess::open_or_create,
			"shared_mem_tcp_TX",
			boost::interprocess::read_write
	);
	sharedMemory_TcpTX.truncate(BUFLEN);
	regionTX = new boost::interprocess::mapped_region(sharedMemory_TcpTX, boost::interprocess::read_write);
	std::memset(regionTX->get_address(), '\0', regionTX->get_size());

	//create a TCP socket
	if ((nSocket=socket(AF_INET, SOCK_STREAM, 0)) == -1)
	{
		std::cout << "Failed to create TCP Socket";
		exit(-1);
	}

	//bind socket to port
	if( bind(nSocket , (struct sockaddr*)&si_me, sizeof(si_me) ) == -1)
	{
		std::cout << "Failed to bind socket to port";
		exit(-2);
	}
	while(true) {
		//set default values for RX/TX buffers when waiting on connection...
		std::memset(buffer, '\0', BUFLEN); // clear buffer
		std::memset(buffer_old, '\0', BUFLEN); // clear old buffer
		std::string firstTXMessage = "{\"status\":\"waiting\"}\n";
		strncpy((char*)regionTX->get_address(), firstTXMessage.c_str(), firstTXMessage.length()); // set TX to standby (avoid null message)

		std::string firstRXMessage = "{\"command\":\"neutral\"}\n";
		std::memset(regionRX->get_address(), '\0', regionRX->get_size()); // clear tx region
		strncpy((char*)regionRX->get_address(), firstRXMessage.c_str(), firstRXMessage.length()); // set TX to standby (avoid null message)
		sleep(1);

		// wait for client to make a connection...
		std::cout << "Waiting for data..." << std::endl;
		if(listen(nSocket, 3) < 0) {
			std::cout << "Failed to receive from socket";
			exit(-1);
		}

		int tcpRuntimeSocket = -1;

		if((tcpRuntimeSocket = accept(nSocket, (struct sockaddr*)&si_me, (socklen_t*)&slen)) < 0) {
			std::cout << "Failed to receive from TCP socket";
			exit(-1);
		}

		bool activeConnection = true;

		while(activeConnection)
		{
			//RX PHASE
			//fflush(stdout);
			std::memset(buffer, '\0', BUFLEN); // clear buffer
			if( recv(tcpRuntimeSocket, buffer, BUFLEN, 0) == 0) {
				std::cout << "Failed to receive message..." << std::endl;
				std::cout << "Connection to TCP client lost..." << std::endl;
				activeConnection = false;
				close(tcpRuntimeSocket);

				std::memset(buffer, '\0', BUFLEN); // clear buffer
				std::cout << "Setting inactive connection..." << std::endl;
				std::string DISCONNECTED_RX_MESSAGE = "{\"directive\":\"neutral\"}\n";
				std::strcpy((char*)regionRX->get_address(), DISCONNECTED_RX_MESSAGE.c_str());
				std::string DISCONNECTED_TX_MESSAGE = "{\"status\":\"waiting\"}\n";
				std::strcpy((char*)regionTX->get_address(), DISCONNECTED_TX_MESSAGE.c_str());
				break;
			}

			if(strcmp(buffer_old, buffer) != 0) {
				std::cout << "Received: " << buffer;
				strncpy(buffer_old, buffer, BUFLEN);
				strncpy((char*)regionRX->get_address(), buffer, BUFLEN); //copy RX buffered message to shared mem
				std::memset(buffer, '\0', BUFLEN); // clear buffer
			}

			strncpy(buffer, (char*)regionTX->get_address(), BUFLEN); //copy shared memory TX message to buffer

			//now reply the client with the same data
			//        if(send(tcpRuntimeSocket, buffer, send_len, 0) == -1) {
			//        	std::cout << "Connection to TCP client lost...";
			//        }

			if(send(tcpRuntimeSocket, buffer, BUFLEN, 0) == -1) {
				std::cout << "Failed to send message..." << std::endl;
				std::cout << "Connection to TCP client lost..." << std::endl;
				activeConnection = false;
				std::memset(buffer, '\0', BUFLEN); // clear buffer
				close(tcpRuntimeSocket);

				std::memset(buffer, '\0', BUFLEN); // clear buffer
				std::cout << "Setting inactive connection..." << std::endl;
				std::string DISCONNECTED_RX_MESSAGE = "{\"directive\":\"neutral\"}\n";
				std::strcpy((char*)regionRX->get_address(), DISCONNECTED_RX_MESSAGE.c_str());
				std::string DISCONNECTED_TX_MESSAGE = "{\"status\":\"waiting\"}\n";
				std::strcpy((char*)regionTX->get_address(), DISCONNECTED_TX_MESSAGE.c_str());
				std::cout << "Setting inactive connection..." << std::endl;
				break;
			}
		}

	}
	std::cout << "Closing TCP socket..." << std::endl;
	close(nSocket);

    return 0;
}
