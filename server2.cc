// FIRST BACKUP SERVER IF SERVER.CC FAILS

#include <iostream>
#include <string>
#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <netdb.h>
#include <sys/uio.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <fstream>
#include "server.h"
using namespace std;

int selfId; // server id, given when started
clientInfo* client_info;
std::unordered_map<std::string, int> active_users; // username : id
std::unordered_map<int, int> backup_servers; // map of backup servers' socket id's
std::unordered_map<std::string, std::string> all_messages; // username : all messages (in order)
std::unordered_map<std::string, std::string> logged_out_users; // username : undelivered messages
std::set<std::string> account_set; // all usernames (both logged in AND not logged in)

void sigintHandler( int signum ) {
    cout << "Interrupt signal (" << signum << ") received.\n";
    char msg[1500];
    for (auto it = active_users.begin(); it != active_users.end(); ++it) {
        int sd = it->second;
        memset(&msg, 0, sizeof(msg));
        const char* server_shutdown_msg = "Server shut down permaturely, so logging you out.";
        strcpy(msg, server_shutdown_msg);
        send(sd, (char*)&msg, sizeof(msg), 0);
    }
   exit(signum);
}

// this should be handled when a client Ctrl+C's
void sigabrtHandler(int signum) {
    cout << "Interrupt signal (" << signum << ") received.\n";
}

int main(int argc, char *argv[]) {
    //we need 3 things: ip address, port number, id, in that order
    if(argc != 3){
        cerr << "Usage: ip_address port" << endl; exit(0); 
    } //grab the IP address and port number 
    char *serverIp = argv[1]; 
    int port = atoi(argv[2]); 
    selfId = atoi(argv[3]);
    //create a message buffer 
    char msg[1500]; 
    //setup a socket and connection tools 
    struct hostent* host = gethostbyname(serverIp); 
    // server address
    sockaddr_in sendSockAddr;   
    bzero((char*)&sendSockAddr, sizeof(sendSockAddr)); 
    sendSockAddr.sin_family = AF_INET; 
    sendSockAddr.sin_addr.s_addr = inet_addr(inet_ntoa(*(struct in_addr*)*host->h_addr_list));
    sendSockAddr.sin_port = htons(port);

    int clientSd = socket(AF_INET, SOCK_STREAM, 0);
    //try to connect...
    int status = connect(clientSd,(sockaddr*) &sendSockAddr, sizeof(sendSockAddr));
    if(status < 0)
    {
        cout<<"Error connecting to socket"<<endl;
        exit(0);
    }

    memset(&msg, 0, sizeof(msg)); //clear the buffer

    cout << "Connected to the server!" << endl;

    int bytesRead, bytesWritten = 0;

    // listener portion
    // TODO: change the code so that server2 listens to server1
    while(1) {
        // create a message buffer for the message to be received
        char msg_recv[1500]; 
        // reading from server
        memset(&msg_recv, 0, sizeof(msg_recv)); // clear the buffer
        bytesRead += recv(clientSd, (char*)&msg_recv, sizeof(msg_recv), 0);
        printf("\n%s\n", msg_recv);

        // send acknowledgement to primary server
        sendAck(clientSd, selfId, bytesWritten);

        // TODO: parse this string for sender username, receipient username, and actual message body
        // and store these pieces into the all_messages map
    }

    cout << "Connection closed" << endl;
    return 0;    
}
