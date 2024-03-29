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
#include <thread>
#include <sys/uio.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <fstream>
#include "server.h"
using namespace std;

int selfId; // server id, given when started
bool is_primary = false;
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
    cout << "ok good\n";
    char msg[1500];
    for (auto it = active_users.begin(); it != active_users.end(); ++it) {
        int sd = it->second;
        memset(&msg, 0, sizeof(msg));
        const char* server_shutdown_msg = "Server shut down permaturely, so logging you out.";
        strcpy(msg, server_shutdown_msg);
        // send(new_socket, (char*)&msg, strlen(msg), 0);
        send(sd, (char*)&msg, sizeof(msg), 0);
    }
    const char* to_new_primary_message = "primary died, you are now primary";
    const char* to_backup_message = "primary died, but you are still backup";

    // flip a coin to determine next backup
    int new_primary_key = (rand() % 2) + 1;

    memset(&msg, 0, sizeof(msg));
    strcpy(msg, to_new_primary_message);
    send(backup_servers[1], (char*)&msg, sizeof(msg), 0);
    
    // memset(&msg, 0, sizeof(msg));
    // strcpy(msg, to_new_primary_message);
    // send(backup_servers[new_primary_key], (char*)&msg, sizeof(msg), 0);

    // memset(&msg, 0, sizeof(msg));
    // strcpy(msg, to_backup_message);
    // send(backup_servers[3 - new_primary_key], (char*)&msg, sizeof(msg), 0);
    exit(signum);
}

// this should be handled when a client Ctrl+C's
// sigabrthandler for when this machine is the primary
void sigabrtHandlerPrimary(int signum) {
    cout << "Interrupt signal (" << signum << ") received.\n";
}

// function that handles going from primary to backup
void backup(sockaddr_in sendSockAddr, char msg[1500]) {

    printf("entered backup thread\n");
    // connect to the server, like we would with a client

    int primarySd_backup = socket(AF_INET, SOCK_STREAM, 0);
    //set master socket to allow multiple connections 
    int iSetOption = 1;
    if( setsockopt(primarySd_backup, SOL_SOCKET, SO_REUSEADDR, (char *)&iSetOption, 
          sizeof(iSetOption)) < 0 )  
    {  
        perror("setsockopt");  
        exit(EXIT_FAILURE);  
    }
    //try to connect...
    int status = connect(primarySd_backup,(sockaddr*) &sendSockAddr, sizeof(sendSockAddr));
    if(status < 0)
    {
        cout<<"[Backup] Error connecting to socket"<<endl;
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
        bytesRead += recv(primarySd_backup, (char*)&msg_recv, sizeof(msg_recv), 0);

        if (!strcmp(msg_recv, "primary died, you are now primary")) {
            printf("\nfrom backup to primary\n");
            return;
        }

        if (!strcmp(msg_recv, "primary died, but you are still backup")) {
            // reconnect to the primary
            close(primarySd_backup);
            int status = connect(primarySd_backup,(sockaddr*) &sendSockAddr, sizeof(sendSockAddr));
            if(status < 0)
            {
                cout<<"[Backup] Error connecting to primary server socket"<<endl;
                exit(0);
            }
        }

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
        // sendAck(primarySd, selfId, bytesWritten);
    }

}

int main(int argc, char *argv[]) {
    //we need 3 things: ip address, port number, id, in that order
    if(argc != 4) {
        cerr << "Usage: ip_address port is_primary (1/0)" << endl; exit(0); 
    } //grab the IP address and port number 
    char *serverIp = argv[1]; 
    int port = atoi(argv[2]); 
    is_primary = atoi(argv[3]);
    printf("is_primary: %d\n", is_primary);
    // selfId = atoi(argv[3]);

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

    // THREAD OFF INTO BACKUP
    // TODO: guard this with the is_primary boolean

    if (!is_primary) {
        std::thread t(backup, sendSockAddr, msg);
        t.join();
        sleep(1);
    }

    printf("THIS SERVER IS NOW THE PRIMARY\n");

    // register signal handler
    signal(SIGINT, sigintHandlerPrimary);
    signal(SIGABRT, sigabrtHandlerPrimary);

    sockaddr_in servAddr;
    bzero((char*)&servAddr, sizeof(servAddr));
    servAddr.sin_family = AF_INET;
    servAddr.sin_addr.s_addr = htonl(INADDR_ANY);
    servAddr.sin_port = htons(port);

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
        cerr << "[Primary] Error establishing the server socket" << endl;
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
        cerr << "[Primary] Error binding socket to local address" << endl;
        exit(0);
    }


    //listen for up to 10 requests at a time
    listen(serverSd, 10);
    //receive a request from client using accept
    //we need a new address to connect with the client
    sockaddr_in newSockAddr;
    socklen_t newSockAddrSize = sizeof(newSockAddr);

    int num_connections = 0;

    while (1) {
        //clear the socket set 
        FD_ZERO(&clientfds);  
     
        //add master socket to set 
        FD_SET(serverSd, &clientfds);  
        max_sd = serverSd;  

        //add child sockets to set 
        for ( i = 0 ; i < max_clients ; i++)  
        {  
            sd = client_socket[i];  
                 
            //if valid socket descriptor then add to read list 
            if(sd > 0)  
                FD_SET( sd , &clientfds);  
                 
            //highest file descriptor number, need it for the select function 
            if(sd > max_sd)  
                max_sd = sd;  
        }

        //wait for an activity on one of the sockets , timeout is NULL, so wait indefinitely 
        activity = select( max_sd + 1 , &clientfds , NULL , NULL , NULL);  
       
        if ((activity < 0) && (errno!=EINTR))  
        {  
            printf("select error");  
        }

        // listen for accepting a new client
        if (FD_ISSET(serverSd, &clientfds)) {
            // accept a new client
            if ((new_socket = accept(serverSd, 
                        (sockaddr *)&newSockAddr, (socklen_t*)&newSockAddrSize))<0)  
            {  
                cerr << "Error accepting request from client!" << endl;
                exit(EXIT_FAILURE);  
            }

            if (num_connections == 0) {
                backup_servers[1] = new_socket;
                //inform server of socket number - used in send and receive commands 
                printf("New backup server connection , socket fd is %d, ip is: %s, port: %d\n",
                new_socket, inet_ntoa(newSockAddr.sin_addr), ntohs
                  (newSockAddr.sin_port));
                num_connections++;
                continue;
            }

            // ask client for username (client will send upon connecting to the server)
            memset(&msg, 0, sizeof(msg)); //clear the buffer
            recv(new_socket, (char*)&msg, sizeof(msg), 0);
            std::string new_client_username(msg);

            if (active_users.find(new_client_username) != active_users.end()) {
                int existing_login_sd = active_users[new_client_username];
                memset(&msg, 0, sizeof(msg));
                const char* force_logout_msg = "Another user logged in as your name, so logging you out.";
                strcpy(msg, force_logout_msg);
                send(existing_login_sd, (char*)&msg, sizeof(msg), 0);
            }
            active_users[new_client_username] = new_socket;
            account_set.insert(new_client_username);

            // check whether this user has any undelivered messages to it
            // if so, send these messages, and remove user from mapping of logged-out users
            // this means the user has logged in previously
            auto it_check_undelivered = logged_out_users.find(new_client_username);
            if (it_check_undelivered != logged_out_users.end()) {
                string undelivered_messages = it_check_undelivered->second;
                memset(&msg, 0, sizeof(msg)); //clear the buffer
                strcpy(msg, undelivered_messages.c_str());
                send(new_socket, undelivered_messages.c_str(), strlen(undelivered_messages.c_str()), 0);
            }

            //inform server of socket number - used in send and receive commands 
            printf("New connection , socket fd is %d, username is %s, ip is: %s, port: %d\n",
            new_socket, msg, inet_ntoa(newSockAddr.sin_addr), ntohs
                  (newSockAddr.sin_port));

            //add new socket to array of sockets 
            for (i = 0; i < max_clients; i++)  
            {  
                //if position is empty 
                if( client_socket[i] == 0 )  
                {  
                    client_socket[i] = new_socket;  
                    printf("Adding to list of sockets as %d\n" , i);  
                         
                    break;
                }  
            }
        }

        int bytesRead, bytesWritten = 0;

        // clear the buffer
        memset(&msg, 0, sizeof(msg));
        for (i = 0; i < max_clients; i++) {

            sd = client_socket[i];

            if (FD_ISSET( sd , &clientfds))  {
                //Check if it was for closing , and also read the incoming message 
                bytesRead = recv(sd, (char*)&msg, sizeof(msg), 0);
                string msg_string(msg);
                printf("msg string: %s\n", msg_string.c_str());
                
                char operation = msg_string[0];

                // handle finding the username of the sender (of message or operation)
                string sender_username;
                for (auto it_find_sender = active_users.begin(); it_find_sender != active_users.end(); ++it_find_sender)  {
                    // if the username's corresponding socketfd is the same as sd
                    if (it_find_sender->second == sd) {
                        sender_username = it_find_sender->first;
                    }
                }

                if (operation == '4'){ //quit 
                    quitUser(sd, client_socket, newSockAddr, newSockAddrSize, sender_username, active_users, logged_out_users, i);
                    continue;
                } else if (operation == '3') { //delete account
                    deleteAccount(sd, client_socket, sender_username, active_users, account_set, logged_out_users, i);
                    continue;
                }

                // operation, username, message
                size_t pos2 = msg_string.find('\n', 2);
                printf("pos2: %zu\n", pos2);

                string username = msg_string.substr(2, pos2 - 2);
                printf("username: %s\n", username.c_str());
                printf("username length: %lu\n", strlen(username.c_str()));
                string message = msg_string.substr(pos2 + 1, msg_string.length() - pos2);
                printf("message: %s\n", message.c_str());
                printf("message length: %lu\n", strlen(message.c_str()));

                // updating within the backup (internal)
                pending_log[username] = pending_log[username] + "From " + sender_username + ": " + message + "\n";
                pending_log[sender_username] = pending_log[sender_username] + "To " + username + ": " + message + "\n";

                if (operation == '2') { // list accounts
                    listAccounts(message, client_socket, bytesWritten, account_set, i);
                } else if (operation == '1') { // send message
                    sendMessage(username, message, sender_username, client_socket, bytesWritten, active_users, logged_out_users, i);
                    // "username" here is recipient username
                    sendBackupMessage(username, message, sender_username, backup_servers[1], bytesWritten);

                    // TODO: implement waiting on acknowledgments from both backup machines
                }
            }
        }
    }


    // int primarySd = socket(AF_INET, SOCK_STREAM, 0);
    // //try to connect...
    // int status = connect(primarySd,(sockaddr*) &sendSockAddr, sizeof(sendSockAddr));
    // if(status < 0)
    // {
    //     cout<<"Error connecting to socket"<<endl;
    //     exit(0);
    // }

    // memset(&msg, 0, sizeof(msg)); //clear the buffer

    // cout << "Connected to the server!" << endl;

    // int bytesRead, bytesWritten = 0;

    // // listener portion
    // // TODO: change the code so that server2 listens to server1
    // while(1) {
    //     // create a message buffer for the message to be received
    //     char msg_recv[1500]; 
    //     // reading from server
    //     memset(&msg_recv, 0, sizeof(msg_recv)); // clear the buffer
    //     bytesRead += recv(primarySd, (char*)&msg_recv, sizeof(msg_recv), 0);
        
    //     // parse this string for sender username, receipient username, and actual message body
    //     string msg_string(msg_recv);

    //     printf("full msg string: %s\n", msg_string.c_str());

    //     size_t pos1 = msg_string.find('\n');
    //     size_t pos2 = msg_string.find('\n', pos1 + 1);
    //     printf("pos1: %zu\n", pos1);
    //     printf("pos2: %zu\n", pos2);

    //     string sender = msg_string.substr(0, pos1);

    //     string recipient = msg_string.substr(pos1 + 1, pos2 - pos1 - 1);
    //     printf("sender username: %s\n", sender.c_str());
    //     printf("recipient username: %s\n", recipient.c_str());
    //     string message = msg_string.substr(pos2 + 1, msg_string.length() - pos2);
    //     printf("message: %s\n", message.c_str());
    //     printf("message length: %lu\n", strlen(message.c_str()));

    //     // TODO: store these pieces into the pending_log map through an internal update
    //     pending_log[recipient] = pending_log[recipient] + "From " + sender + ": " + message + "\n";
    //     pending_log[sender] = pending_log[sender] + "To " + recipient + ": " + message + "\n";
    //     // send acknowledgement to primary server
    //     sendAck(primarySd, selfId, bytesWritten);
    // }

    cout << "Connection closed" << endl;
    return 0;    
}
