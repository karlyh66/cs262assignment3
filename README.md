# cs262assignment3

## Compiling files
Run `g++ -o server server.cc --std=c++11` and `g++ -o client client.cc --std=c++11`. This should provide the two execuable files for everything below.

## Setting Up the Servers
On one terminal window, run `./server ip port status` where `ip` is your machine ip, `port` is any four-digit number above 1024 (example: 6000), and `status` is 1 if that process is starting as the main server or 0 if starting as a backup server.

## Connecting Clients to the Server
From another terminal window, run `./client ip port username` where `ip` is the ip of the server, `port` is the one specified by the server, and `username` is your username. If the username has never been used before, an account will be created. If it has been used before, you will be logged into your account, and any messages received while logged out will be delivered. 

In other words, logging in and creating your account happen through the same process.

If you log in through the same username on two clients at the same time, the first will be logged out.

You can find the server's ip address by reading from its terminal (for example, mine reads like `karlyh@dhcp-10-250-200-218`, which means the ip is 10.2250.200.218). You can also run the terminal command `ipconfig getifaddr en0`.

## Client Interactions with the Server
After each action (and at the beginning of a session), the server will prompt you for an operation. You can choose:

- 1: send message
    - then you'll be prompted for a username to send to
    - lastly, you'll be prompted for the message
    - if the user doesn't exist, you'll have to try again from the operation step
- 2: list accounts
    - you'll be prompted for a wildcard
    - all accounts, both active and logged out, containing the wildcard (i.e. substring) will be listed
- 3: delete account
    - your account will automatically be deleted from the system and from server records
    - your process will terminate with a specified status (generally "OK status")
- 4: log out
    - you will be logged out, but your account stays in the system and in server records
    - the server will queue any messages sent to you while you were away, and send you these messages upon your next login
    - your process will terminate with a specified status (generally "OK status") If you do not specify 1, 2, 3, or 4 as an operation, you will be asked to try again

## Wire Protocol Information
- Up to 10 clients can connect to the server at a time
- A message can be up to 1500 chars long
- Messages can be sent to both active and logged out users
- Any messages sent to a logged out client will be queued...
- ...and upon the next login, any messages sent in a client's absence will be delivered to them
- If you delete your account, any messages you sent (to other clients) previously that have not yet been delivered will still be delivered to them
- It is up to the client to detect that you deleted your account, either through listing accounts or through erroneously sending a message (to your deleted username)
- Recall that account creation and login are the same process. However, usernames are unique, and if an existing client session already exists with the username I specify, that existing client session gets logged out, and my new session becomes active in that session's place. Any messages that the previous login sent to any logged out clients are still queued.
- When the server abnormally shuts down (e.g. through Ctrl+c), all remaining client programs also shut down after displaying the message "Server shut down permaturely, so logging you out."
- If the client connects before the server, or the client has the incorrect IP address or port, (or for whatever reason the socket does not bind successfully), then the client receives an "Error connecting to socket" from the server
- If we do 

## 2-Fault Tolerance
- Our system is 2-fault tolerant, meaning that 2 servers can go down and all commands should function as expected for all clients. Specifically, the backup server that stays running can take over and become the primary server in the following cases:
    * When both the primary server AND the new primary server (that replaces the first primary server) goes down (without any additional backups being replenished)
    * When both the primary server AND the backup (that stays as backup) goes down (without any additional backups being replenished)
- If a server that previously went down comes back up, it is able to rejoin the server cluster as a backup for the currently active server.
    * Note that we cannot run two server machines as primaries at the same time; the code will throw a socket binding error.
- When the currently active server goes down, one of its backups is randomly selected to become to the new active server, and the other backup reconnects to this server to continue serving as its backup. 
    * If there are two backups running, one of them is chosen to be the next primary via a coin flip. The other backup then connects to this new primary (through a fresh new socket connection).
    * If there is only one backup running, it is chosen to be the next primary.
- Any currently connected clients will be logged out and can re-connect to the new active server.
- There is no need to connect all the backup servers to the primary server before any clients go on--this is because the server socket can detect whether an incoming connection is from a client, or from a backup server.

## Persistence
- Whenever anything happens, the primary server communicates with backup servers so that they are up-to-date. This includes:
    - When any client sends a message to another, the primary server tells backup servers, which keep track of the messages. In case the primary server goes down, any backup servers are able to provide chat logs to the user.
    - When a new client connects to the primary server (PS), the PS lets backup servers know to add the account to their set of accounts.
    - When a client deletes their account, the PS lets backup servers know to delete the account from their set of accounts.

## Other engineering decisions
We used the select() Linux call to manage multiple client socket descriptors, so that multiple clients can connect to the server and talk to one another at the same time. select() is effective because it allows multiple processes to run concurrently (accessing shared memory) without the need for a new thread per client. Since select() operates on a fixed-size fd_set client_fds (a set of file descriptors), we limited the number of clients connecting at the same time to 10 to account for this. (If we wanted to, we could have increased this number to, say, 100+. The implementation does not change.)

In our array of client socket descriptors, we can listen for activity on all of the sockets at the same time with a combination of (1) FD_ISSET(server_sd, &client_fds); and (2) activity = select(max_fd + 1, &readfds, NULL, NULL, NULL); until one of the client sockets get activity (e.g. an incoming message). Since we have a blocking implementation, we use while loops to listen.

We still use threading, though, on top of select(): in our client code, there is a second thread that listens for incoming server activity (while the main program handles user input). If a client program is forced to quit (either because of a forced log out by another login of the same username, or by abnormal server exit), the listener thread receives that and is responsible for alerting the main thread and having that thread quit as well.

## Unit testing
We implemented unit tests... FINISH WRITING THIS.
