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
std::unordered_map<std::string, std::string> commit_log; // username : committed messages (in order)
std::unordered_map<std::string, std::string> pending_log; // username : pending messages (in order)
std::unordered_map<std::string, std::string> logged_out_users; // username : undelivered messages
std::set<std::string> account_set; // all usernames (both logged in AND not logged in)


// siginthandler for when this machine is the primary
void sigintHandlerPrimary( int signum ) {
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
// sigabrthandler for when this machine is the primary
void sigabrtHandlerPrimary(int signum) {
    cout << "Interrupt signal (" << signum << ") received.\n";
}

void primary(int primarySd) {
    char msg[1500];

    // register signal handler
    signal(SIGINT, sigintHandlerPrimary);
    signal(SIGABRT, sigabrtHandlerPrimary);

    // serverSd: master socket
    int serverSd, addrlen, new_socket , client_socket[10] , 
          max_clients = 12 , curr_clients = 0, activity, i , valread , sd;

    int max_sd; 
    
    //set of socket descriptors 
    fd_set clientfds;
    
    //initialize all client_socket[] to 0 so not checked 
    for (i = 0; i < max_clients; i++) {  
        client_socket[i] = 0;  
    }

    //open stream oriented socket with internet address
    //also keep track of the socket descriptor
    serverSd = socket(AF_INET, SOCK_STREAM, 0);
    if(serverSd < 0)
    {
        cerr << "Error establishing the server socket" << endl;
        exit(0);
    }

    //set master socket to allow multiple connections 
    int iSetOption = 1;
    if( setsockopt(serverSd, SOL_SOCKET, SO_REUSEADDR, (char *)&iSetOption, 
          sizeof(iSetOption)) < 0 )  
    {  
        perror("setsockopt");  
        exit(EXIT_FAILURE);  
    }

    //bind the socket to its local address
    int bindStatus = ::bind(serverSd, (sockaddr*) &servAddr, 
        sizeof(servAddr));
    if(bindStatus < 0)
    {
        cerr << "Error binding socket to local address" << endl;
        exit(0);
    }
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

    int primarySd = socket(AF_INET, SOCK_STREAM, 0);
    //try to connect...
    int status = connect(primarySd,(sockaddr*) &sendSockAddr, sizeof(sendSockAddr));
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
        bytesRead += recv(primarySd, (char*)&msg_recv, sizeof(msg_recv), 0);
        
        // parse this string for sender username, receipient username, and actual message body
        string msg_string(msg_recv);

        printf("full msg string: %s\n", msg_string.c_str());

        size_t pos1 = msg_string.find('\n');
        size_t pos2 = msg_string.find('\n', pos1 + 1);
        printf("pos1: %zu\n", pos1);
        printf("pos2: %zu\n", pos2);

        string sender = msg_string.substr(0, pos1);

        string recipient = msg_string.substr(pos1 + 1, pos2 - pos1 - 1);
        printf("sender username: %s\n", sender.c_str());
        printf("recipient username: %s\n", recipient.c_str());
        string message = msg_string.substr(pos2 + 1, msg_string.length() - pos2);
        printf("message: %s\n", message.c_str());
        printf("message length: %lu\n", strlen(message.c_str()));

        // TODO: store these pieces into the pending_log map through an internal update
        pending_log[recipient] = pending_log[recipient] + "From " + sender + ": " + message + "\n";
        pending_log[sender] = pending_log[sender] + "To " + recipient + ": " + message + "\n";
        // send acknowledgement to primary server
        sendAck(primarySd, selfId, bytesWritten);
    }

    cout << "Connection closed" << endl;
    return 0;    
}
