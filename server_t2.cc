// SERVER (PRIMARY AND BACKUP)

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
#include <map>
#include <vector>
using namespace std;

int selfId; // server id, given when started
bool is_primary = false;
int primarySd_backup = 0; // socket FD for backup server's connection to primary server
clientInfo *client_info;
std::unordered_map<std::string, int> active_users;                                              // username : socket id
std::unordered_map<int, int> backup_servers;                                                    // map of backup server socket id's. keys: [1, 2] (indices). values: backup server socket id's
std::map<int, std::string> commit_log;                                                          // commandID : commands (in order)
std::unordered_map<std::string, std::vector<std::pair<std::string, std::string>>> chat_history; // username : vector of messages history
std::unordered_map<std::string, std::string> logged_out_users;                                  // username : undelivered messages
std::set<std::string> account_set;                                                              // all usernames (both logged in AND not logged in)

void recoverState()
{
    // read commit log from file and update account_set, chat_history
    std::ifstream commit_log_file;
    commit_log_file.open("commit_log_" + std::to_string(selfId) + ".txt");

    std::string line;

    // find the last commandID in the commit log
    int last_commandID;
    if (commit_log.end() != commit_log.begin())
    {
        auto it = commit_log.end();
        it--;
        last_commandID = it->first;
    }
    else
        last_commandID = 0; 

    // read commit log file line by line
    while (std::getline(commit_log_file, line))
    {

        std::string commandID = line.substr(0, line.find(":"));
        line = line.substr(0, line.find("\n")); // remove new line character from end of line
        line = line.substr(line.find(":") + 1); // remove commandID

        if (std::stoi(commandID) <= last_commandID) // if commandID in file is less than or equal to last commandID in state, skip
            continue;
            
        commit_log[std::stoi(commandID)] = line; // update commit log object

        std::string command = line.substr(0, 1); // first character of line is the command

        if (command == "C")
        {
            std::string username = line.substr(line.find("|") + 1);
            account_set.insert(username);
        }

        else if (command == "D")
        {
            std::string username = line.substr(line.find("|") + 1);
            // if user had chat history, remove it
            if (chat_history.find(username) != chat_history.end())
                chat_history.erase(username);
            account_set.erase(username); // assuming that by sequence of commands, an account is created before it is deleted
        }

        else if (command == "M")
        {
            size_t pos1 = line.find("|") + 1;
            size_t pos2 = line.find("|", pos1) + 1;
            size_t pos3 = line.find("|", pos2) + 1;
            std::string sender = line.substr(pos1, pos2 - pos1 - 1);
            std::string receiver = line.substr(pos2, pos3 - pos2 - 1);
            std::string message = line.substr(pos3);

            // Update chat history
            std::pair<std::string, std::string> message_pair = std::make_pair(sender, message); // sender : message

            if (chat_history.find(receiver) == chat_history.end()) // if receiver has no chat history
            {
                std::vector<std::pair<std::string, std::string>> message_history; // vector of sender : message pairs
                message_history.push_back(message_pair);                         // add message to message history
                chat_history.insert(std::make_pair(receiver, message_history));  // add message history for receiver
            }
            else 
            {
                chat_history[receiver].push_back(message_pair);
            }
        }
    }
}

// write commit log to file
void writeCommitLogToFile(int sd)
{
    ofstream commit_log_file;
    commit_log_file.open("commit_log_" + std::to_string(sd) + ".txt"); // , std::ios_base::app); // open file in append mode
    for (auto it = commit_log.begin(); it != commit_log.end(); ++it)
    {
        commit_log_file << it->first << ":" << it->second << endl;
    }
    commit_log_file.close();
}

// adds command to log, with key being the commandID
void insertIntoLog(std::map<int, std::string> &log, std::string command)
{
    // if log is empty, insert command with commandID = 1
    if (log.empty())
    {
        log.insert(std::pair<int, std::string>(1, command));
        // for (auto it = log.begin(); it != log.end(); ++it)
        // {
        //     printf("key: %d, value: %s\n", it->first, it->second.c_str());
        // }
        // printf("First command inserted into log: %s\n", command.c_str());
    }
    // else, insert command with sequential commandID
    else
    {
        auto it = log.end();
        it--;
        int last_command_id = it->first;
        // printf("last command id: %d", last_command_id);
        log.insert(std::pair<int, std::string>(last_command_id + 1, command));
    }
}

// siginthandler
void sigintHandler(int signum)
{
    if (is_primary)
    {
        // if the primary server fails, flip a coin to choose the backup server that assumes the primary server's role
        cout << "Interrupt signal (" << signum << ") received.\n";
        char msg[1500];

        // log out all clients that were logged in
        for (auto it = active_users.begin(); it != active_users.end(); ++it)
        {
            int sd = it->second;
            memset(&msg, 0, sizeof(msg));
            const char *server_shutdown_msg = "Server shut down permaturely, so logging you out.";
            strcpy(msg, server_shutdown_msg);
            send(sd, (char *)&msg, sizeof(msg), 0);
        }
        const char *to_new_primary_message = "primary died, you are now primary";
        const char *to_backup_message = "primary died, but you are still backup";

        int num_primary_keys = 0;
        for (int i = 1; i <= 2; i++)
        {
            if (backup_servers[i] != 0)
            {
                num_primary_keys++;
            }
        }

        // choose the next primary server
        if (num_primary_keys == 2)
        {
            // if there are two backup servers left:
            // flip a coin to determine the next primary server
            // (if the existing primary server crashes or fails)
            int new_primary_key = (rand() % 2) + 1;
            // tell the randomly chosen backup server that they are now the primary server
            memset(&msg, 0, sizeof(msg));
            strcpy(msg, to_new_primary_message);
            send(backup_servers[new_primary_key], (char *)&msg, sizeof(msg), 0);

            // tell the other backup server that they are stil a backup server,
            // but that they must close the socket and then reconnect to the (new) primary server
            memset(&msg, 0, sizeof(msg));
            strcpy(msg, to_backup_message);
            send(backup_servers[num_primary_keys + 1 - new_primary_key], (char *)&msg, sizeof(msg), 0);
        }
        else
        {
            // if there is only one backup left:
            // tell backup server 1 that they are now the primary server
            memset(&msg, 0, sizeof(msg));
            strcpy(msg, to_new_primary_message);
            send(backup_servers[1], (char *)&msg, sizeof(msg), 0);
        }

        exit(signum);
    }
    else
    {
        // if a backup server fails or crashes, close the socket connection
        cout << "Interrupt signal (" << signum << ") received.\n";
        printf("primarySd_backup: %d\n", primarySd_backup);
        close(primarySd_backup);
        primarySd_backup = 0;
        exit(signum);
    }
}

void sigintHandlerBackup(int signum)
{
    // if a backup server fails or crashes, close the socket connection
    cout << "Interrupt signal (" << signum << ") received.\n";
    printf("primarySd_backup: %d\n", primarySd_backup);
    // tell the primary server that a backup server died, so decrement the number of backup server connections
    char msg[1500];
    memset(&msg, 0, sizeof(msg)); // clear the buffer

    // send to the primary server an indication that this backup server is about to die
    string backup_crash_msg = "backup died. please decrement num_backup_connections";
    strcpy(msg, backup_crash_msg.c_str());

    // send client username to server
    int backup_crash_msg_bytes = send(primarySd_backup, (char *)&msg, strlen(msg), 0);
    memset(&msg, 0, sizeof(msg)); // clear the buffer again

    close(primarySd_backup);
    primarySd_backup = 0;

    exit(signum);
}

// sigabrthandler for when this machine is the primary server
void sigabrtHandlerPrimary(int signum)
{
    cout << "Interrupt signal (" << signum << ") received.\n";
}

// function that is run (through a thread) when this machine is a backup server
void backup(hostent *host, int port)
{

    printf("Entered backup thread\n");
    char msg[1500];

    // register signal handlers
    signal(SIGINT, sigintHandlerBackup);

    // backup server socket address
    sockaddr_in sendSockAddr;
    bzero((char *)&sendSockAddr, sizeof(sendSockAddr));
    sendSockAddr.sin_family = AF_INET;
    sendSockAddr.sin_addr.s_addr = inet_addr(inet_ntoa(*(struct in_addr *)*host->h_addr_list));
    sendSockAddr.sin_port = htons(port);

    // socket FD for backup server's connection to primary server
    primarySd_backup = socket(AF_INET, SOCK_STREAM, 0);

    // set master socket to allow multiple connections,
    // and so that the port frees up if this machine goes down
    int iSetOption = 1;
    if (setsockopt(primarySd_backup, SOL_SOCKET, SO_REUSEADDR, (char *)&iSetOption,
                   sizeof(iSetOption)) < 0)
    {
        perror("setsockopt");
        exit(EXIT_FAILURE);
    }

    // connect to the primary server, as if this backup machine were a client
    int status = connect(primarySd_backup, (sockaddr *)&sendSockAddr, sizeof(sendSockAddr));
    if (status < 0)
    {
        cout << "[Backup] Error connecting to socket" << endl;
        exit(0);
    }

    memset(&msg, 0, sizeof(msg)); // clear the buffer

    cout << "Connected to the server!" << endl;

    // send to the primary server an indication that this is a backup server, NOT a client
    // clients are not allowed to specify "\n" as their username, so this is a safe guard
    string username = "\n";
    strcpy(msg, username.c_str());

    // send client username to server
    int usernameBytes = send(primarySd_backup, (char *)&msg, strlen(msg), 0);
    memset(&msg, 0, sizeof(msg)); // clear the buffer again

    int bytesRead, bytesWritten = 0;

    // listener for any state updates from the primary server, and update internal data structures accordingly
    while (1)
    {
        // create a message buffer for the message to be received
        char msg_recv[1500];

        // read from primary server
        memset(&msg_recv, 0, sizeof(msg_recv)); // clear the buffer
        bytesRead += recv(primarySd_backup, (char *)&msg_recv, sizeof(msg_recv), 0);

        // cast response bytes to string
        string msg_string(msg_recv);

        // update internal accounts set with the account creation
        if (!strcmp(msg_string.substr(0, 1).c_str(), "0"))
        {
            printf("\nAccount created\n");
            string username = msg_string.substr(2, msg_string.length() - 3);
            printf("username: %s\n", username.c_str());
            insertIntoLog(commit_log, "C|" + username);
            account_set.insert(username);
            continue;
        }

        // do nothing with existing user log in
        if (!strcmp(msg_string.substr(0, 1).c_str(), "1"))
        {
            printf("\nAccount logged in\n");
            string username = msg_string.substr(2, msg_string.length() - 3);
            printf("username: %s\n", username.c_str());
            continue;
        }

        // update internal accounts set with the account deletion
        if (!strcmp(msg_string.substr(0, 1).c_str(), "3"))
        {
            printf("\nAccount deleted\n");
            string username = msg_string.substr(2, msg_string.length() - 3);
            printf("username: %s\n", username.c_str());
            insertIntoLog(commit_log, "D|" + username);
            account_set.erase(username);
            continue;
        }

        // primary died, and this backup server is now the primary
        if (!strcmp(msg_recv, "primary died, you are now primary"))
        {
            is_primary = 1;
            printf("\nfrom backup to primary\n");
            return;
        }

        // primary died. This backup server is still primary,
        // but it must socket-connect to the new primary
        if (!strcmp(msg_recv, "primary died, but you are still backup"))
        {
            // reconnect to the primary
            close(primarySd_backup);
            sleep(2);                                           // sleep for 2 seconds to give the new primary time to start up
            primarySd_backup = socket(AF_INET, SOCK_STREAM, 0); // initialize new socket object
            int status = connect(primarySd_backup, (sockaddr *)&sendSockAddr, sizeof(sendSockAddr));
            if (status < 0)
            {
                cout << "[Backup] Error connecting to primary server socket" << endl;
                is_primary = 1;
                return;
            }
            // tell the new primary server that this backup is connected to them now
            memset(&msg, 0, sizeof(msg)); // clear the buffer

            // send to the primary server an indication that this is a backup server, NOT a client
            // clients are not allowed to specify "\n" as their username, so this is a safe guard
            string username = "\n";
            strcpy(msg, username.c_str());

            // send client username to server
            int usernameBytes = send(primarySd_backup, (char *)&msg, strlen(msg), 0);
            memset(&msg, 0, sizeof(msg)); // clear the buffer again
        }

        // parse message (incoming from primary server) for
        // sender username, recipient username, and actual message body
        printf("full msg string: %s\n", msg_string.c_str());

        size_t pos1 = msg_string.find('\n');
        size_t pos2 = msg_string.find('\n', pos1 + 1);

        string sender = msg_string.substr(0, pos1);
        string recipient = msg_string.substr(pos1 + 1, pos2 - pos1 - 1);
        printf("sender username: %s\n", sender.c_str());
        printf("recipient username: %s\n", recipient.c_str());
        string message = msg_string.substr(pos2 + 1, msg_string.length() - pos2);
        printf("message: %s\n", message.c_str());
        printf("message length: %lu\n", strlen(message.c_str()));

        // update internal chat history with the new message
        pair<string, string> p1 = make_pair(sender, message);
        chat_history[recipient].push_back(p1);

        insertIntoLog(commit_log, "M|" + sender + "|" + recipient + "|" + message);

        // send acknowledgement to primary server
        // sendAck(primarySd, selfId, bytesWritten);
    }
}

int main(int argc, char *argv[])
{
    // we need 4 things: ip address, port number, id, is_primary indicator, in that order
    // ip address is only important if we are running a backup server
    if (argc != 4)
    {
        cerr << "Usage: ip_address port is_primary (1/0)" << endl;
        exit(0);
    }
    // grab the port number
    int port = atoi(argv[2]);
    is_primary = atoi(argv[3]);
    printf("is_primary: %d\n", is_primary);
    // selfId = atoi(argv[3]);
    selfId = 0;

    // create a message buffer
    char msg[1500];

    // thread off into backup server, and guard this thread with the is_primary boolean
    if (!is_primary)
    {
        char *serverIp = argv[1];
        // setup a socket and connection tools
        struct hostent *host = gethostbyname(serverIp);
        std::thread t(backup, host, port);
        t.join();
        backup_servers[1] = 0;
        backup_servers[2] = 0;
        sleep(1);
    }

    printf("THIS SERVER IS NOW THE PRIMARY\n");

    // register signal handler
    signal(SIGINT, sigintHandler);
    signal(SIGABRT, sigabrtHandlerPrimary);

    sockaddr_in servAddr;
    bzero((char *)&servAddr, sizeof(servAddr));
    servAddr.sin_family = AF_INET;
    servAddr.sin_addr.s_addr = htonl(INADDR_ANY);
    servAddr.sin_port = htons(port);

    // serverSd: master socket
    int serverSd, addrlen, new_socket, client_socket[12],
        max_clients = 12, curr_clients = 0, activity, valread, sd;

    int max_sd;

    // set of client socket descriptors
    fd_set clientfds;

    // initialize all client_socket[] to 0 so not checked
    for (int i = 0; i < max_clients; i++)
    {
        client_socket[i] = 0;
    }

    // open stream oriented socket with internet address
    // also keep track of the socket descriptor
    serverSd = socket(AF_INET, SOCK_STREAM, 0);
    if (serverSd < 0)
    {
        cerr << "[Primary] Error establishing the server socket" << endl;
        exit(0);
    }

    // set master socket to allow multiple connections
    int iSetOption = 1;
    if (setsockopt(serverSd, SOL_SOCKET, SO_REUSEADDR, (char *)&iSetOption,
                   sizeof(iSetOption)) < 0)
    {
        perror("setsockopt");
        exit(EXIT_FAILURE);
    }

    // bind the listener socket to its local address
    int bindStatus = ::bind(serverSd, (sockaddr *)&servAddr,
                            sizeof(servAddr));
    if (bindStatus < 0)
    {
        cerr << "[Primary] Error binding socket to local address" << endl;
        exit(0);
    }

    // recover state from log file
    recoverState();

    // listen for up to 10 requests at a time
    listen(serverSd, 12);
    // receive a request from client using accept
    // we need a new address to connect with the client
    sockaddr_in newSockAddr;
    socklen_t newSockAddrSize = sizeof(newSockAddr);

    int num_backup_connections = 0;

    while (1)
    {
        // clear the socket set
        FD_ZERO(&clientfds);

        // add master socket to set
        FD_SET(serverSd, &clientfds);
        max_sd = serverSd;

        // add child sockets to set
        for (int i = 0; i < max_clients; i++)
        {
            sd = client_socket[i];

            // if valid socket descriptor then add to read list
            if (sd > 0)
                FD_SET(sd, &clientfds);

            // highest file descriptor number, need it for the select function
            if (sd > max_sd)
                max_sd = sd;
        }

        // wait for an activity on one of the sockets , timeout is NULL, so wait indefinitely
        activity = select(max_sd + 1, &clientfds, NULL, NULL, NULL);

        if ((activity < 0) && (errno != EINTR))
        {
            printf("select error");
        }

        // listen for accepting a new client
        if (FD_ISSET(serverSd, &clientfds))
        {
            // accept a new client
            if ((new_socket = accept(serverSd,
                                     (sockaddr *)&newSockAddr, (socklen_t *)&newSockAddrSize)) < 0)
            {
                cerr << "Error accepting request from client!" << endl;
                exit(EXIT_FAILURE);
            }

            // ask client for username (client will send upon connecting to the server)
            // if username is "\n", then we have a backup server connection, NOT a client connection
            memset(&msg, 0, sizeof(msg)); // clear the buffer
            recv(new_socket, (char *)&msg, sizeof(msg), 0);
            std::string new_client_username(msg);

            if (num_backup_connections <= 1 && !strcmp(new_client_username.c_str(), "\n"))
            {
                backup_servers[num_backup_connections + 1] = new_socket;
                // inform server of backup server socket number
                // used in send and receive commands
                num_backup_connections++;
                printf("New backup server connection , socket fd is %d, ip is: %s, port: %d, num_backup_connections: %d\n",
                       new_socket, inet_ntoa(newSockAddr.sin_addr), ntohs(newSockAddr.sin_port), num_backup_connections);
            }

            // if we have a CLIENT connection, NOT a backup server connection
            if (strcmp(new_client_username.c_str(), "\n") != 0)
            {

                if (active_users.find(new_client_username) != active_users.end())
                {
                    int existing_login_sd = active_users[new_client_username];
                    memset(&msg, 0, sizeof(msg));
                    const char *force_logout_msg = "Another user logged in as your name, so logging you out.";
                    strcpy(msg, force_logout_msg);
                    send(existing_login_sd, (char *)&msg, sizeof(msg), 0);
                }
                
                string existing_user = "1";
                // if new account is being created, add commit to log
                if (account_set.find(new_client_username) == account_set.end())
                {
                    // add new account to set of accounts
                    insertIntoLog(commit_log, "C|" + new_client_username);
                    writeCommitLogToFile(selfId);
                    account_set.insert(new_client_username);
                    existing_user = "0";
                }
                active_users[new_client_username] = new_socket;

                // send information about this new account creation to the backup servers
                if (backup_servers[1])
                {
                    sendAccountCreation(new_client_username, backup_servers[1], existing_user);
                }
                if (backup_servers[2])
                {
                    sendAccountCreation(new_client_username, backup_servers[2], existing_user);
                }

                // display chat history for this user, if it exists
                auto it = chat_history.find(new_client_username);
                if (it != chat_history.end())
                {
                    string msg_history = "chat history for " + it->first + ":\n";
                    for (auto it2 = it->second.begin(); it2 != it->second.end(); ++it2)
                    {
                        msg_history += "<" + it2->first + ">: " + it2->second + "\n";
                    }
                    memset(&msg, 0, sizeof(msg)); // clear the buffer
                    strcpy(msg, msg_history.c_str());
                    send(new_socket, msg_history.c_str(), strlen(msg_history.c_str()), 0);
                }

                // check whether this user has any undelivered messages to it
                // if so, send these messages, and remove user from mapping of logged-out users
                // this means the user has logged in previously
                auto it_check_undelivered = logged_out_users.find(new_client_username);
                if (it_check_undelivered != logged_out_users.end())
                {
                    string undelivered_messages = it_check_undelivered->second;
                    memset(&msg, 0, sizeof(msg)); // clear the buffer
                    strcpy(msg, undelivered_messages.c_str());
                    send(new_socket, undelivered_messages.c_str(), strlen(undelivered_messages.c_str()), 0);
                }

                // inform server of socket number - used in send and receive commands
                printf("New client connection , socket fd is %d, username is %s, ip is: %s, port: %d\n",
                       new_socket, msg, inet_ntoa(newSockAddr.sin_addr), ntohs(newSockAddr.sin_port));
            }

            // add new socket to array of sockets
            // this code now applies to ALL connections - backup server and client
            // we add backup server connections to the client_socket set as well,
            //    so that the primary server can hear when a backup server crashes
            for (int i = 0; i < max_clients; i++)
            {
                // if position is empty
                if (client_socket[i] == 0)
                {
                    client_socket[i] = new_socket;
                    printf("Adding to list of sockets as %d\n", i);

                    break;
                }
            }
        }

        int bytesRead, bytesWritten = 0;

        // clear the buffer
        memset(&msg, 0, sizeof(msg));
        for (int i = 0; i < max_clients; i++)
        {

            sd = client_socket[i];

            if (FD_ISSET(sd, &clientfds))
            {
                // check if it was for closing, and also read the incoming message
                bytesRead = recv(sd, (char *)&msg, sizeof(msg), 0);
                string msg_string(msg);
                printf("msg string: %s\n", msg_string.c_str());

                // check if this  message is meant to tell primary server that a backup has died
                if (!strcmp(msg_string.c_str(), "backup died. please decrement num_backup_connections"))
                {
                    num_backup_connections--;
                    printf("A backup has died. num_backup_connections: %d\n", num_backup_connections);
                    close(sd);
                    client_socket[i] = 0;
                    continue;
                }

                char operation = msg_string[0];

                // handle finding the username of the sender (of message or operation)
                string sender_username;
                for (auto it_find_sender = active_users.begin(); it_find_sender != active_users.end(); ++it_find_sender)
                {
                    // if the username's corresponding socketfd is the same as sd
                    if (it_find_sender->second == sd)
                    {
                        sender_username = it_find_sender->first;
                    }
                }

                if (operation == '4')
                { // quit / log out
                    quitUser(sd, client_socket, newSockAddr, newSockAddrSize, sender_username, active_users, logged_out_users, i);
                    continue;
                }
                else if (operation == '3')
                { // delete account
                    insertIntoLog(commit_log, "D|" + sender_username);
                    writeCommitLogToFile(selfId);
                    if (chat_history.find(sender_username) != chat_history.end())
                        chat_history.erase(sender_username);
                    deleteAccount(sd, client_socket, sender_username, active_users, account_set, logged_out_users, i);
                    // send information about this account deletion to the backup servers
                    if (backup_servers[1])
                    {
                        sendAccountDeletion(sender_username, backup_servers[1]);
                    }
                    if (backup_servers[2])
                    {
                        sendAccountDeletion(sender_username, backup_servers[2]);
                    }
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

                if (operation == '2')
                { // list accounts
                    printf("List accounts request received. Below are the accounts:\n");
                    for (std::string a : account_set)
                    {
                        printf("%s\n", a.c_str());
                    }
                    listAccounts(message, client_socket, bytesWritten, account_set, i);
                }
                else if (operation == '1')
                { // send message

                    insertIntoLog(commit_log, "M|" + sender_username + "|" + username + "|" + message);
                    writeCommitLogToFile(selfId);

                    pair<string, string> p1 = make_pair(sender_username, message);
                    chat_history[username].push_back(p1);

                    for (auto it = chat_history.begin(); it != chat_history.end(); ++it)
                    {
                        printf("chat history for %s:\n", it->first.c_str());
                        for (auto it2 = it->second.begin(); it2 != it->second.end(); ++it2)
                        {
                            printf("<%s>: %s\n", it2->first.c_str(), it2->second.c_str());
                        }
                        printf("\n");
                    }

                    sendMessage(username, message, sender_username, client_socket, bytesWritten, active_users, logged_out_users, i);
                    // "username" here is recipient username
                    if (backup_servers[1])
                    {
                        sendBackupMessage(username, message, sender_username, backup_servers[1], bytesWritten);
                    }
                    if (backup_servers[2])
                    {
                        sendBackupMessage(username, message, sender_username, backup_servers[2], bytesWritten);
                    }
                    // TODO: implement waiting on acknowledgments from both backup machines
                }
            }
        }
    }
}
