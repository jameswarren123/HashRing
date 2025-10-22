#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <string.h>
#include <pthread.h>
#include <netdb.h>

#define BUFFER_SIZE 2048
#define HASH_SPACE 1024

// Range[1] will be the id. range 0 will be predecessor id + 1
int range[2];

// Port this server is on
int port;

// FD of socket to accept connections on.
int socketFD;

// The hash space to store all the values.
char *values[HASH_SPACE];

// Predecessor Information
char predecessorAddress[INET_ADDRSTRLEN];
int predecessorPort;
int predecessorFD;

// Sucessor Information
char successorAddress[INET_ADDRSTRLEN];
int successorPort;
int successorFD;

// Bootstrap Information [Used by normal nameservers]
char bootstrapAddress[INET_ADDRSTRLEN];
int bootstrapPort;
int bootstrapFD;

typedef struct
{
    int fd;
} clientDataStruct;

// copies local ip to the passed in char*
// Retrieves the local machine's IP address and stores it in ip_buffer.
// ip_buffer must be at least INET_ADDRSTRLEN bytes.
// Returns 0 on success, -1 on failure.
int get_local_ip(char *ip_buffer)
{
    char hostname[1024];
    hostname[1023] = '\0';

    if (gethostname(hostname, 1023) != 0)
    {
        perror("gethostname");
        return -1;
    }

    struct hostent *host_entry = gethostbyname(hostname);
    if (host_entry == NULL)
    {
        perror("gethostbyname");
        return -1;
    }

    struct in_addr *addr = (struct in_addr *)host_entry->h_addr_list[0];
    if (addr == NULL)
    {
        fprintf(stderr, "No IP address found\n");
        return -1;
    }

    strcpy(ip_buffer, inet_ntoa(*addr));
    return 0;
}

// Opens the port for connections.
int open_socket(int port)
{
    int openSocketFD = socket(AF_INET, SOCK_STREAM, 0);
    if (openSocketFD < 0)
    {
        perror("Socket Creation Failed");
        exit(EXIT_FAILURE);
    }
    setsockopt(openSocketFD, SOL_SOCKET, SO_REUSEPORT, &(int){1}, sizeof(int));
    setsockopt(openSocketFD, SOL_SOCKET, SO_REUSEADDR, &(int){1}, sizeof(int));

    struct sockaddr_in sockaddr;
    memset(&sockaddr, '\0', sizeof(sockaddr));
    sockaddr.sin_family = AF_INET;
    sockaddr.sin_port = htons(port);
    sockaddr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(openSocketFD, (struct sockaddr *)&sockaddr, sizeof(sockaddr)) < 0)
    {
        perror("Bind Failed");
        exit(EXIT_FAILURE);
    }

    if (listen(openSocketFD, 1) < 0)
    {
        perror("Listen Failed");
        exit(EXIT_FAILURE);
    }
    return openSocketFD;
}

// Connects to the port at address and returns the connectionFD.
int create_connection(char *address, int port)
{
    int connectionFD = socket(AF_INET, SOCK_STREAM, 0);
    if (connectionFD < 0)
    {
        perror("Socket Creation Failed");
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in sockaddr;
    memset(&sockaddr, '\0', sizeof(sockaddr));
    sockaddr.sin_family = AF_INET;
    sockaddr.sin_port = htons(port);
    inet_pton(AF_INET, address, &sockaddr.sin_addr);

    if (connect(connectionFD, (struct sockaddr *)&sockaddr, sizeof(sockaddr)) < 0)
    {
        perror("Create Connection Fail");
        exit(EXIT_FAILURE);
    }
    return connectionFD;
}

// Inserts the key value pair iff key in range
void insert(int key, char *value)
{
    values[key] = (char *)calloc(BUFFER_SIZE, sizeof(char));
    if (values[key] == NULL)
    {
        perror("Memory allocation failed");
        exit(EXIT_FAILURE);
    }
    strcpy(values[key], value);
}

// Deletes the value at key if key in range AND there is a value associated to key.
// Frees the memory at key and marks key as empty
void delete(int key)
{
    if (values[key] == NULL)
    {
        return;
    }
    free(values[key]);
    values[key] = NULL;
}

// handles all non bootstrap messages passed to nodes in the ring
void *messageHandler(void *arg)
{
    // printf("Here in message handler\n");
    clientDataStruct *newClientStruct = (clientDataStruct *)arg;

    // clientFD is the PREDECESSOR FD
    int clientFD = newClientStruct->fd;
    char inputBuffer[BUFFER_SIZE];
    while (1)
    {
        int readAmount = 0;
        // WE CAN'T READ CLIENT FD ONCE WE CHANGE PREDECESSOR
        readAmount = read(clientFD, inputBuffer, BUFFER_SIZE);
        inputBuffer[readAmount] = '\0';
        if (readAmount == 0)
        {
            printf("in readAmount = 0\n");
            pthread_exit(0);
        }
        char command[BUFFER_SIZE];
        sscanf(inputBuffer, "%s", command);
        printf("%s\n", inputBuffer);
        
        if (strcmp("enter", command) == 0)
        {
            // THE BOOTSTRAP SERVER IS THE ONLY RECEIVER OF THIS COMMAND.
            //  gets id, port, address
            int id, port2;
            char address[INET_ADDRSTRLEN];
            sscanf(inputBuffer, "%*s %d %d %s", &id, &port2, address);
            if (id == 0 || id >= range[0])
            {
                printf("Id %d in range %d %d\n", id, range[0], range[1]);
                
                // if in range send successor, predecessor, range info, traversed list, and key values in range
                char traversedList[BUFFER_SIZE] = "0";
                char message[BUFFER_SIZE];
                char myIP[INET_ADDRSTRLEN]; // INET_ADDRSTRLEN = 16 bytes, enough for IPv4 string
                get_local_ip(myIP);
                snprintf(message, sizeof(message), "entered %d %s %d %s %d %s ", successorPort, successorAddress, port, myIP, range[0], traversedList);

                // update predecessor and successor info

                // Writes to the current PREDECESSOR it needs to update its SUCCESSOR to the NEW NODE
                char m2[BUFFER_SIZE];
                snprintf(m2, sizeof(m2), "%s %d %s", "updateSuccessor", port2, address);
                write(predecessorFD, m2, strlen(m2));

                predecessorPort = port2;
                strcpy(predecessorAddress, address);
                predecessorFD = create_connection(address, port2);

                char temp[10];
                read(predecessorFD, temp, sizeof(temp));
                
                if (strcmp(temp, "ack") != 0)
                {
                    printf("%s\n", temp);
                    printf("uh oh\n");
                    return NULL;
                }

                write(predecessorFD, message, strlen(message));

                // pass along all key values in CURRENT NODE'S RANGE
                for (int i = range[0]; i <= id; i++)
                {
                    if (values[i] != NULL)
                    {
                        char insertMessage[BUFFER_SIZE];
                        char checkBuf[BUFFER_SIZE];
                        memset(checkBuf, 0, BUFFER_SIZE);
                        snprintf(insertMessage, sizeof(insertMessage), "%d %s", i, values[i]);
                        write(predecessorFD, insertMessage, strlen(insertMessage));
                        read(predecessorFD, checkBuf, BUFFER_SIZE);
                        if(strcmp(checkBuf, "ack") != 0){
                            
                            printf("an error occured\n");
                        }
                        delete (i);
                    }
                }
                write(predecessorFD, "EOF", strlen("EOF"));
                
                range[0] = id + 1;  
                printf("RANGE: %d\n", range[0]);
            }
            else
            {
                // pass this message along to the successor
                // add current server id to the traversed server id list
                char traversedList[BUFFER_SIZE] = "0";
                char command2[BUFFER_SIZE] = "entering";
                char message[BUFFER_SIZE];
                // send entering
                snprintf(message, sizeof(message), "%s %d %d %s %s", command2, id, port2, address, traversedList);
                write(successorFD, message, strlen(message));
            }
        }
        else if (strcmp("entering", command) == 0)
        {
            //  gets id, port, address
            int id, port2;
            char address[INET_ADDRSTRLEN];
            char traversedList[BUFFER_SIZE];
            sscanf(inputBuffer, "%*s %d %d %s", &id, &port2, address, traversedList);
            char *traversedListStart = strstr(inputBuffer, address) + strlen(address) + 1;
            strncpy(traversedList, traversedListStart, BUFFER_SIZE - 1);
            traversedList[BUFFER_SIZE - 1] = '\0';
            snprintf(traversedList, BUFFER_SIZE, "%s,%d", traversedList, range[1]);
            if (range[0] <= id && id <= range[1])
            {
                // if in range send successor, predecessor, range info, traversed list, and key values in range
                char message[BUFFER_SIZE];
                char myIP[INET_ADDRSTRLEN]; // INET_ADDRSTRLEN = 16 bytes, enough for IPv4 string
                get_local_ip(myIP);
                snprintf(message, sizeof(message), "entered %d %s %d %s %d %s ", predecessorPort, predecessorAddress, port, myIP, range[0], traversedList);

                // update predecessor and successor info

                // Writes to the current predecessor to update its successor to the NEW NODE
                char m2[BUFFER_SIZE];
                snprintf(m2, sizeof(m2), "%s %d %s", "updateSuccessor", port2, address);
                write(predecessorFD, m2, strlen(m2));

                predecessorPort = port2;
                strcpy(predecessorAddress, address);
                predecessorFD = create_connection(address, port2);

                char temp[10];
                read(predecessorFD, temp, sizeof(temp));
                printf("read\n");
                
                if (strcmp(temp, "ack") != 0)
                {
                    printf("%s\n", temp);
                    printf("uh oh\n");
                    return (void *)NULL;
                }

                write(predecessorFD, message, strlen(message));
                printf("wrote message\n");
                // pass along all key values in successors range
                for (int i = range[0]; i <= id; i++)
                {
                    if (values[i] != NULL)
                    {
                        char insertMessage[BUFFER_SIZE];
                        snprintf(insertMessage, sizeof(insertMessage), "%d %s", i, values[i]);
                        write(predecessorFD, insertMessage, strlen(insertMessage));
                        delete (i);
                        read(predecessorFD, insertMessage, BUFFER_SIZE);
                        if(strcmp(insertMessage, "ack") != 0){
                            printf("an error occured\n");
                        }
                    }
                }
                write(predecessorFD, "EOF", strlen("EOF"));

                range[0] = id + 1;
            }
            else
            {
                // pass this message along to the successor
                // add current server id to the traversed server id list
                char command2[BUFFER_SIZE] = "entering";
                char message[BUFFER_SIZE];
                snprintf(message, sizeof(message), "%s %d %d %s %s", command2, id, port2, address, traversedList);
                write(successorFD, message, strlen(message));
            }
        } // entering
        else if (strcmp("entered", command) == 0)
        {
            int sPort, pPort, rStart;
            char sAddress[INET_ADDRSTRLEN], pAddress[INET_ADDRSTRLEN], traversedList[BUFFER_SIZE];
            sscanf(inputBuffer, "%*s %d %s %d %s %d %s", &pPort, pAddress, &sPort, sAddress, &rStart, traversedList);
            // update predecessor and successor info
            successorPort = sPort;
            strcpy(successorAddress, sAddress);
            predecessorPort = pPort;
            strcpy(predecessorAddress, pAddress);
            successorFD = create_connection(successorAddress, successorPort);
            range[0] = rStart;

            // insert all key values from SUCCESSOR
            int going = 1;
            readAmount = read(successorFD, inputBuffer, BUFFER_SIZE);
            if(inputBuffer[0] == 'E' && inputBuffer[1] == 'O' && inputBuffer[2] == 'F') going = 0;
            while (going == 1)
            {
                char valBuf[BUFFER_SIZE];
                int key;
                char value[BUFFER_SIZE];
                sscanf(valBuf, "%d %s", &key, value);
                printf("%d %s\n", key, value);
                insert(key, value);
                write(successorFD, "ack", strlen("ack"));
                memset(valBuf, 0, BUFFER_SIZE);
                readAmount = read(successorFD, valBuf, BUFFER_SIZE);
                if(strcmp("EOF", valBuf) == 0) going = 0;
            }

            // all prints
            printf("successful entry\n");
            printf("Range: [%d, %d]\n", range[0], range[1]);
            write(successorFD, "getID", strlen("getID"));
            readAmount = read(successorFD, inputBuffer, BUFFER_SIZE);
            inputBuffer[readAmount] = '\0';
            int id;
            sscanf(inputBuffer, "%d", &id);
            write(predecessorFD, "getID", strlen("getID"));
            readAmount = read(predecessorFD, inputBuffer, BUFFER_SIZE);
            inputBuffer[readAmount] = '\0';
            int id2;
            sscanf(inputBuffer, "%d", &id2);
            printf("Predecessor ID: %d\n", id2);
            printf("Sucessor ID: %d\n", id);
            printf("Traversed: %s\n", traversedList);
        } // entered
        else if (strcmp("getID", command) == 0)
        {
            char message[BUFFER_SIZE];
            snprintf(message, sizeof(message), "%d", range[1]);
            write(clientFD, message, strlen(message));
        }
        else if (strcmp("updatePredecessor", command) == 0)
        {
            int pPort;
            char pAddress[INET_ADDRSTRLEN];
            sscanf(inputBuffer, "%*s %d %s", &pPort, pAddress);
            predecessorPort = pPort;
            strcpy(predecessorAddress, pAddress);
            struct sockaddr_in clientAddr;
            socklen_t clientAddrlen = sizeof(clientAddr);
            if ((predecessorFD = accept(socketFD, (struct sockaddr *)&clientAddr, &clientAddrlen)) < 0)
            {
                perror("Accept failed");
                exit(EXIT_FAILURE);
            }
        }
        else if (strcmp("updateSuccessor", command) == 0)
        {
            int sPort;
            char sAddress[INET_ADDRSTRLEN];
            sscanf(inputBuffer, "%*s %d %s", &sPort, sAddress);
            successorPort = sPort;
            strcpy(successorAddress, sAddress);
            successorFD = create_connection(successorAddress, successorPort);
        }
        else if (strcmp("updateRange0", command) == 0)
        {
            sscanf(inputBuffer, "%*s %d", &range[0]);
            readAmount = read(predecessorFD, inputBuffer, BUFFER_SIZE);
            while (strcmp("EOF", inputBuffer) != 0)
            {
                int key;
                char value[BUFFER_SIZE];
                sscanf(inputBuffer, "%d %s ", &key, value);
                insert(key, value);
                readAmount = read(predecessorFD, inputBuffer, BUFFER_SIZE);
            }
        }
        else if (strcmp("lookupNext", command) == 0)
        {
            int key;
            char traversedList[BUFFER_SIZE];
            sscanf(inputBuffer, "%*s %d %s", &key, traversedList);

            // Perform lookup
            if (key >= range[0] && key <= range[1])
            {
                if (values[key] == NULL)
                {
                    char message[BUFFER_SIZE];
                    snprintf(message, sizeof(message), "PRINT Key not found\nTraversed: %s,%d\nFinal response obtained: %d\n", traversedList, range[1], range[1]);
                    write(bootstrapFD, message, strlen(message));
                }
                else
                {
                    char message[BUFFER_SIZE];
                    snprintf(message, sizeof(message), "PRINT Key: %d Value: %s\nTraversed: %s,%d\nFinal response obtained: %d\n", key, values[key], traversedList, range[1], range[1]);
                    write(bootstrapFD, message, strlen(message));
                }
            }
            else
            {
                // pass this message along to the successor non bootstrap server
                // add current server id to the traversed server id list
                write(successorFD, "getID", strlen("getID"));
                readAmount = read(successorFD, inputBuffer, BUFFER_SIZE);
                inputBuffer[readAmount] = '\0';
                int id;
                sscanf(inputBuffer, "%*s %d", &id);
                if (id == 0)
                {
                    char message[BUFFER_SIZE];
                    snprintf(message, sizeof(message), "PRINT Key not found\nTraversed: %s,%d\nFinal response obtained: %d\n", traversedList, range[1], range[1]);
                    write(bootstrapFD, message, strlen(message));
                }
                else
                {
                    snprintf(traversedList + strlen(traversedList), sizeof(traversedList) - strlen(traversedList), ",%d", range[1]);
                    char command2[BUFFER_SIZE] = "lookupNext";
                    char message[BUFFER_SIZE];
                    snprintf(message, sizeof(message), "%s %d %s", command2, key, traversedList);
                    write(successorFD, message, strlen(message));
                }
            }
        }
        else if (strcmp("PRINT", command) == 0)
        {
            char *message = strstr(inputBuffer, "PRINT ");
            if (message != NULL)
            {
                message += strlen("PRINT "); // Move the pointer past "PRINT "
                printf("%s\n", message);     // Print everything after "PRINT "
            }
        }

        else if (strcmp("inserting", command) == 0)
        {
            int key;
            char value[BUFFER_SIZE];
            char traversedList[BUFFER_SIZE];
            sscanf(inputBuffer, "%*s %d %s %s", &key, value, traversedList);
            // Perform insert
            if (key >= range[0] && key <= range[1])
            {
                insert(key, value);
                char message[BUFFER_SIZE];
                snprintf(message, sizeof(message), "PRINT Key: %d Value: %s Insert\nTraversed: %s,%d\nInserted at: %d\n", key, values[key], traversedList, range[1], range[1]);
                write(bootstrapFD, message, strlen(message));
            }
            else
            {
                write(successorFD, "getID", strlen("getID"));
                readAmount = read(successorFD, inputBuffer, BUFFER_SIZE);
                inputBuffer[readAmount] = '\0';
                int id;
                sscanf(inputBuffer, "%d", &id);
                if (id == 0)
                {
                    printf("Key not found\n");
                    printf("Traversed: %s,%d\n", traversedList, range[1]);
                    printf("Final response obtained: %d\n", range[1]);
                }
                // pass this message along to the successor
                snprintf(traversedList + strlen(traversedList), sizeof(traversedList) - strlen(traversedList), ",%d", range[1]);
                char command2[BUFFER_SIZE] = "inserting";
                char message[BUFFER_SIZE];
                snprintf(message, sizeof(message), "%s %d %s %s", command2, key, value, traversedList);
                write(successorFD, message, strlen(message));
            }
        }

        else if (strcmp("deleting", command) == 0)
        {
            int key;
            char traversedList[BUFFER_SIZE];
            sscanf(inputBuffer, "%*s %d %s", &key, traversedList);
            // Perform delete
            if (key >= range[0] && key <= range[1])
            {
                if (values[key] == NULL)
                {
                    char message[BUFFER_SIZE];
                    snprintf(message, sizeof(message), "PRINT Key not found\nTraversed: %s,%d\nFailed at: %d\n", traversedList, range[1], range[1]);
                    write(bootstrapFD, message, strlen(message));
                }
                else
                {
                    char message[BUFFER_SIZE];
                    snprintf(message, sizeof(message), "PRINT Key: %d Value: %s Successful Deletion\nTraversed: %s,%d\nDeleted at: %d\n", key, values[key], traversedList, range[1], range[1]);
                    delete (key);
                    write(bootstrapFD, message, strlen(message));
                }
            }
            else
            {
                write(successorFD, "getID", strlen("getID"));
                readAmount = read(successorFD, inputBuffer, BUFFER_SIZE);
                inputBuffer[readAmount] = '\0';
                int id;
                sscanf(inputBuffer, "%d", &id);
                if (id == 0)
                {
                    char message[BUFFER_SIZE];
                    snprintf(message, sizeof(message), "PRINT Key not found\nTraversed: %s,%d\nFinal response obtained: %d\n", traversedList, range[1], range[1]);
                    write(bootstrapFD, message, strlen(message));
                }
                // pass along to successor
                snprintf(traversedList + strlen(traversedList), sizeof(traversedList) - strlen(traversedList), ",%d", range[1]);
                char command2[BUFFER_SIZE] = "deleting";
                char message[BUFFER_SIZE];
                snprintf(message, sizeof(message), "%s %d %s", command2, key, traversedList);
                write(successorFD, message, strlen(message));
            }
        }
    }
}

// The user interaction thread method for the bootstrap server.
void bootstrapMain()
{
    // printf("Here in bootstrap main\n");
    char inputBuffer[BUFFER_SIZE];
    while (1)
    {
        int readAmount = 0;
        readAmount = read(0, inputBuffer, BUFFER_SIZE);
        inputBuffer[readAmount] = '\0';
        // printf(inputBuffer);
        if (readAmount == 0)
        {
            return;
        }

        char command[BUFFER_SIZE];
        sscanf(inputBuffer, "%s", command);
        // printf(inputBuffer);
        if (strcmp("lookup", command) == 0)
        {
            // printf("Here in lookup bs main\n");
            int key;
            sscanf(inputBuffer, "%*s %d", &key);
            // printf("Key after scan: %d\n", key);
            // Perform lookup
            if (key == 0 || key >= range[0])
            {
                if (values[key] == NULL)
                {
                    printf("Key not found\n");
                    printf("Traversed: 0\n");
                    printf("Final response obtained: 0\n");
                }
                else
                {
                    printf("Key: %d Value: %s\n", key, values[key]);
                    printf("Traversed: 0\n");
                    printf("Final response obtained: 0\n");
                }
            }
            else
            {
                // pass this message along to the successor non bootstrap server
                // add current server id to the traversed server id list
                write(successorFD, "getID", strlen("getID"));
                readAmount = read(successorFD, inputBuffer, BUFFER_SIZE);
                inputBuffer[readAmount] = '\0';
                int id;
                sscanf(inputBuffer, "%d", &id);
                if (id == 0)
                {
                    printf("Key not found\n");
                    printf("Traversed: 0\n");
                    printf("Final response obtained: 0\n");
                }
                else
                {
                    char traversedList[BUFFER_SIZE] = "0";
                    char command2[BUFFER_SIZE] = "lookupNext";
                    char message[BUFFER_SIZE];
                    snprintf(message, sizeof(message), "%s %d %s", command2, key, traversedList);
                    write(successorFD, message, strlen(message));
                }
            }
        } // lookup
        else if (strcmp("insert", command) == 0)
        {
            int key;
            char value[BUFFER_SIZE];
            sscanf(inputBuffer, "%*s %d %s", &key, value);
            // Perform insert
            if (key == 0 || key >= range[0])
            {
                insert(key, value);
                printf("Key: %d Value: %s Insert\n", key, values[key]);
                printf("Traversed: 0\n");
                printf("Inserted at: 0\n");
            }
            else
            {
                write(successorFD, "getID", strlen("getID"));
                readAmount = read(successorFD, inputBuffer, BUFFER_SIZE);
                inputBuffer[readAmount] = '\0';
                int id;
                sscanf(inputBuffer, "%d", &id);
                if (id == 0)
                {
                    printf("Key not found\n");
                    printf("Traversed: 0\n");
                    printf("Final response obtained: 0\n");
                }
                // pass this message along to the successor
                char traversedList[BUFFER_SIZE] = "0";
                char command2[BUFFER_SIZE] = "inserting";
                char message[BUFFER_SIZE];
                snprintf(message, sizeof(message), "%s %d %s %s", command2, key, value, traversedList);
                write(successorFD, message, strlen(message));
            }
        } // insert
        else if (strcmp("delete", command) == 0)
        {
            // TODO: IMPLEMENT PROPERLY
            int key;
            sscanf(inputBuffer, "%*s %d", &key);
            // Perform delete
            if (key == 0 || key >= range[0])
            {
                if (values[key] == NULL)
                {
                    printf("Key not found\n");
                }
                else
                {
                    printf("Key: %d Value: %s Successful Deletion\n", key, values[key]);
                    delete (key);
                }
                printf("Traversed: 0\n");
                printf("Deleted at: 0\n");
            }
            else
            {
                write(successorFD, "getID", strlen("getID"));
                readAmount = read(successorFD, inputBuffer, BUFFER_SIZE);
                inputBuffer[readAmount] = '\0';
                int id;
                sscanf(inputBuffer, "%d", &id);
                if (id == 0)
                {
                    printf("Key not found\n");
                    printf("Traversed: 0\n");
                    printf("Final response obtained: 0\n");
                }
                // pass along to successor
                char traversedList[BUFFER_SIZE] = "0";
                char command2[BUFFER_SIZE] = "deleting";
                char message[BUFFER_SIZE];
                snprintf(message, sizeof(message), "%s %d %s", command2, key, traversedList);
                write(successorFD, message, strlen(message));
            }
        } // delete
    }
}

// The user interaction thread method for the normal name servers.
void nameServerMain()
{
    printf("here in name server main\n");
    char inputBuffer[BUFFER_SIZE];
    pthread_t thread;
    int running = 1;
    while (running == 1)
    {
        int readAmount = 0;
        readAmount = read(0, inputBuffer, BUFFER_SIZE);
        inputBuffer[readAmount] = '\0';
        if (readAmount == 0)
        {
            return;
        }
        
        char command[BUFFER_SIZE];
        sscanf(inputBuffer, "%s", command);

        if (strcmp("enter", command) == 0)
        {
            // Create the connection to the bootstrap server
            bootstrapFD = create_connection(bootstrapAddress, bootstrapPort);
    
            // send info to bootstrap server
            // needs id, port, address
            char myIP[INET_ADDRSTRLEN]; // INET_ADDRSTRLEN = 16 bytes, enough for IPv4 string
            get_local_ip(myIP);

            char message[BUFFER_SIZE];
            snprintf(message, sizeof(message), "%s %d %d %s", command, range[1], port, myIP);
            // printf("%s\n", message);
            write(bootstrapFD, message, strlen(message));

            struct sockaddr_in clientAddr;
            socklen_t clientAddrlen = sizeof(clientAddr);
            if ((predecessorFD = accept(socketFD, (struct sockaddr *)&clientAddr, &clientAddrlen)) < 0)
            {
                perror("Accept failed");
                exit(EXIT_FAILURE);
            }
            // spin off thread to listen for communication
            clientDataStruct newClientStruct = {predecessorFD};
            pthread_create(&thread, NULL, messageHandler, (void *)&newClientStruct);
            pthread_detach(thread);
            write(predecessorFD, "ack", strlen("ack"));
        } // enter
        else if (strcmp("exit", command) == 0) // TODO: Currently CRASHES. Don't.
        {
            /*
            the name server will gracefully exit the system. The name server will inform its
            successor and predecessor name servers. It will hand over the key value pairs that it was
            maintaining to the successor. Upon successful exit, the server will print �Successful exit�
            message. It will also print out the ID of the successor and the key range that was handed over
            */

            running = 0;
            write(successorFD, "getID", strlen("getID"));
            readAmount = read(successorFD, inputBuffer, BUFFER_SIZE);
            inputBuffer[readAmount] = '\0';
            int id;
            sscanf(inputBuffer, "%d", &id);

            // tell successor to inherit my range
            char message02[BUFFER_SIZE];
            snprintf(message02, sizeof(message02), "updateRange0 %d", range[0]);
            write(successorFD, message02, strlen(message02));

            // give my key values to successor
            for (int i = range[0]; i <= range[1]; i++)
            {
                if (values[i] != NULL)
                {
                    char insertMessage[BUFFER_SIZE];
                    snprintf(insertMessage, sizeof(insertMessage), "%d %s ", i, values[i]);
                    write(successorFD, insertMessage, strlen(insertMessage));
                    delete(i);
                }
            }
            write(successorFD, "EOF", strlen("EOF"));

            // tell successor its new predecessor is my predecessor
            char message03[BUFFER_SIZE];
            snprintf(message03, sizeof(message03), "updatePredecessor %d %s", predecessorPort, predecessorAddress);
            write(successorFD, message03, strlen(message03));

            // tell predecessor its new successor is my successor
            char message01[BUFFER_SIZE];
            snprintf(message01, sizeof(message01), "updateSuccessor %d %s", successorPort, successorAddress);
            write(predecessorFD, message01, strlen(message01));

            close(predecessorFD);
            close(successorFD);
            close(bootstrapFD);
            // Stops the messageHandler thread.
            pthread_cancel(thread);

            // print id of my successor and range of keys handed over
            printf("Successful exit\n");
            printf("ID of successor: %d\n", id);
            printf("Range of keys handed over: [%d, %d]\n", range[0], range[1]);
        } // exit
    }
}

// Handles all incoming connections by creating a new thread to accept them.
void handle_connections()
{
    struct sockaddr_in clientAddr;
    socklen_t clientAddrlen = sizeof(clientAddr);

    int clientDataFD;

    // Loops forever.
    while (1)
    {
        // Waits for connections from the data and socket ports
        if ((clientDataFD = accept(socketFD, (struct sockaddr *)&clientAddr, &clientAddrlen)) < 0)
        {
            perror("Accept failed");
            exit(EXIT_FAILURE);
        }
        write(1, "Node Connected to Port\n", sizeof("Node Connected to Port\n"));

        // Spins off a new thread to handle all the client commands
        clientDataStruct newClientStruct = {clientDataFD};
        pthread_t thread;
        pthread_create(&thread, NULL, messageHandler, (void *)&newClientStruct);
        pthread_detach(thread);
    }
}

int main(int argc, char *argv[])
{
    // Disables buffering on stdout. Used to make printf print instantly.
    setbuf(stdout, NULL);

    // Makes sure that there are enough and not too many arguments
    if (argc != 2)
    {
        printf("Usage: ./nameserver <Config File>\n");
        return EXIT_FAILURE;
    }

    // Open the file for reading
    FILE *file = fopen(argv[1], "r");
    if (file == NULL)
    {
        printf("No File\n");
        return EXIT_FAILURE;
    }

    // Gets the id and port of the name server from the config file
    if (fscanf(file, "%d %d", &range[1], &port) != 2)
    {
        printf("Error reading file\n");
        return EXIT_FAILURE;
    }

    // More general nameserver setup stuff:
    socketFD = open_socket(port);

    // Initialize the space to hold all the values with NULL
    for (int i = 0; i < HASH_SPACE; i++)
    {
        values[i] = NULL;
    }

    if (range[1] != 0)
    {
        // Normal Name Server
        // We get the bootstrap details from the config
        fscanf(file, "%s %d", bootstrapAddress, &bootstrapPort);
        fclose(file);
        // We'll get range[0] later when we figure out our place.
        // Call the user thread handler for the nameserver
        nameServerMain();
    }
    else
    {
        // Bootstrap Server
        // printf("here\n");
        // Since the bootstrap is always the first, it's intitial range
        // is always from [1, 0]. ( It loops, 1,2,...,1023,0 )
        range[0] = 1;

        // bootstrap predecessor and successor start as itself
        char myIP[INET_ADDRSTRLEN]; // INET_ADDRSTRLEN = 16 bytes, enough for IPv4 string
        get_local_ip(myIP);
        strcpy(predecessorAddress, myIP);
        strcpy(successorAddress, myIP);
        predecessorPort = port;
        successorPort = port;
        
        snprintf(bootstrapAddress, sizeof(bootstrapAddress), "%s", myIP);
        bootstrapPort = port;
        
        int key;
        char value[BUFFER_SIZE];
        while (fscanf(file, "%d %s", &key, value) != EOF)
        {
            insert(key, value);
        }
        fclose(file);
        // This handles commands coming in from other name servers
        pthread_t thread;
        pthread_create(&thread, NULL, handle_connections, NULL);
        pthread_detach(thread);

        sleep(1);
        
        predecessorFD = create_connection(bootstrapAddress, bootstrapPort);
        
        successorFD = create_connection(bootstrapAddress, bootstrapPort);

        // This is the main user interaction thread
        bootstrapMain();
    }

    return EXIT_SUCCESS;
}