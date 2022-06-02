#include <stdio.h>
#include <sys/types.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "DLL/dll.h"
#include "Mac-List/mac-list.h"
#include "Routing-Table/routing-table.h"
#include "Sync/sync.h"


#define MAX_CLIENTS 32
#define OP_LEN 128
#define MAX_MASK 32

//extern int store_IP(const char *mac, const char *ip);



int connection_socket;
/* Server's copies of network data structures*/
dll_t *routing_table;
dll_t *mac_list;

/* An array of File descriptors which the server process is maintaining in order to talk with the connected clients. Master skt FD is also a member of this array*/
int monitored_fd_set[MAX_CLIENTS];
pid_t client_pid_set[MAX_CLIENTS]; // array of client process id's


/*Remove all the FDs and client pid's, if any, from the the array*/
void intitiaze_monitor_fd_and_client_pid_set(){
    int i = 0;
    for(; i < MAX_CLIENTS; i++) {
        monitored_fd_set[i] = -1;
        client_pid_set[i] = -1;
    }
}
/*Add a new FD to the monitored_fd_set array*/
void add_to_monitored_fd_set(int skt_fd){
    int i = 0;
    for(; i < MAX_CLIENTS; i++){
        if(monitored_fd_set[i] != -1)
            continue;
        monitored_fd_set[i] = skt_fd;
        break;
    }
}

/*Add a new pid to the client_pid_set array*/
void add_to_client_pid_set(int pid){
    int i = 0;
    for(; i < MAX_CLIENTS; i++){
        if(client_pid_set[i] != -1)
            continue;
        client_pid_set[i] = pid;
        break;
    }
}
/*Remove the FD from monitored_fd_set array*/
void remove_from_monitored_fd_set(int skt_fd){
    int i = 0;
    for(; i < MAX_CLIENTS; i++){
        if(monitored_fd_set[i] != skt_fd)
            continue;
        monitored_fd_set[i] = -1;
        break;
    }
}

/*Remove the pid from client_pid_set array*/
void remove_from_client_pid_set(int pid){
    int i = 0;
    for(; i < MAX_CLIENTS; i++){
        if(monitored_fd_set[i] != pid)
            continue;
        client_pid_set[i] = -1;
        break;
    }
}
/* Clone all the FDs in monitored_fd_set array into fd_set Data structure*/
void refresh_fd_set(fd_set *fd_set_ptr){
    FD_ZERO(fd_set_ptr);
    int i = 0;
    for(; i < MAX_CLIENTS; i++){
        if(monitored_fd_set[i] != -1){
            FD_SET(monitored_fd_set[i], fd_set_ptr);
        }
    }
}

/*Get the numerical max value among all FDs which server is monitoring*/
int get_max_fd(){
    int i = 0;
    int max = -1;

    for(; i < MAX_CLIENTS; i++){
        if(monitored_fd_set[i] > max)
            max = monitored_fd_set[i];
    }
    return max;
}

/* Parses a string command, in the format <Opcode, Dest, Mask, GW, OIF> or <Opcode, Mac> with each field separated by a space, to create a sync message for clients, instructing them on how to update their copies of the routing table. The silent parameter indicates whether the server is actively inputting a command for MAC list via stdin or a client is replicating a command sent by the server. Returns 0 on success and -1 on any failure. */
int create_sync_message(char *operation, sync_msg_t *sync_msg) {
    char *token = strtok(operation," ");
    if(token){
        switch (token[0]) {
            case 'C':
                sync_msg->op_code=CREATE;
                break;
            case 'U':
                sync_msg->op_code=UPDATE;
                break;
            case 'D':
                sync_msg->op_code=DELETE;
                break;
            case 'S':
                sync_msg->op_code=NONE;
                display_routing_table(routing_table);
                return 0;
        }
    }
    token = strtok(NULL," ");
    memcpy(sync_msg->msg_body.routing_table_entry.dest, token, strlen(token));

    token = strtok(NULL," ");
    sync_msg->msg_body.routing_table_entry.mask=atoi(token);

    /* Only CREATE and UPDATE require a gw and oif*/
    if(sync_msg->op_code==CREATE || sync_msg->op_code==UPDATE){
        token = strtok(NULL," ");
        memcpy(sync_msg->msg_body.routing_table_entry.gw, token, strlen(token));

        token = strtok(NULL," ");
        memcpy(sync_msg->msg_body.routing_table_entry.oif, token, strlen(token));
    }
    return 0;
}

/* Send newly client all necessary CREATE commands to replicate the server's copies of the current routing table or MAC list. */
void update_new_client(int data_socket, char *op, sync_msg_t *sync_msg) {
    dll_node_t *head = routing_table->head;
    dll_node_t *curr = head->next;

    while(curr!=head){
        routing_table_entry_t entry = *((routing_table_entry_t*)curr->data);
//        sync_msg->op_code=CREATE;
        sprintf(op,"CREATE %s %u %s %s",entry.dest,entry.mask,entry.gw,entry.gw);
        create_sync_message(op,sync_msg);
        write(data_socket,sync_msg, sizeof(sync_msg_t));
        curr = curr->next;
    }
}


int main() {
    struct sockaddr_un name;
    int ret;
    int data_socket;
    fd_set readfds;

    routing_table = init_dll();
    intitiaze_monitor_fd_and_client_pid_set();
    add_to_monitored_fd_set(0);


    unlink(SOCKET_NAME);
    /* master socket for accepting connections from client */
    connection_socket = socket(AF_UNIX, SOCK_STREAM, 0);
    if (connection_socket == -1) {
        perror("socket");
        exit(1);
    }

    /* initialize*/
    memset(&name,0, sizeof(struct sockaddr_un));

    /*Specify the socket Cridentials*/
    name.sun_family=AF_UNIX;
    strncpy(name.sun_path,SOCKET_NAME,strlen(name.sun_path));

    /* Bind socket to socket name.*/
    ret = bind(connection_socket,(const struct sockaddr*)&name, sizeof(struct sockaddr_un));
    if(ret == -1){
        perror("Bind");
        exit(EXIT_FAILURE);
    }

    /* Prepare for accepting connections.  */
    ret = listen(connection_socket,20);
    if(ret == -1){
        perror("Listen");
        exit(EXIT_FAILURE);
    }

    add_to_monitored_fd_set(connection_socket);

    while(1){
        char op[OP_LEN];
        sync_msg_t *sync_msg = calloc(1, sizeof(sync_msg_t));

        refresh_fd_set(&readfds);

        printf("Please select from the following options:\n");
        printf("1.CREATE <Destination IP> <Mask (0-32)> <Gateway IP> <OIF>\n");
        printf("2.UPDATE <Destination IP> <Mask (0-32)> <New Gateway IP> <New OIF>\n");
        printf("3.DELETE <Destination IP> <Mask (0-32)>\n");
        printf("4.CREATE <MAC>\n");
        printf("5.DELETE <MAC>\n");
        printf("6.SHOW\n");
        printf("7.FLUSH\n");

        select(get_max_fd()+1,&readfds,NULL,NULL,NULL);

        /* New connection: send entire routing table and mac list states to newly connected client. */
        if (FD_ISSET(connection_socket,&readfds)){
            data_socket = accept(connection_socket,NULL,NULL);
            if (data_socket==-1){
                perror("accept");
                exit(EXIT_FAILURE);
            }
            add_to_monitored_fd_set(data_socket);
            update_new_client(data_socket,op,sync_msg);
        }
        else if(FD_ISSET(0,&readfds)) {
            ret = read(0, op, OP_LEN - 1);
            op[strcspn(op, "\r\n")] = 0; // flush new line
            if (ret == -1) {
                perror("read");
                return 1;
            }
            op[ret] = 0;

            if (!create_sync_message(op, sync_msg)) {
                process_sync_mesg(routing_table, sync_msg);
            }
            int i, comm_socket_fd;
            for (i = 2; i < MAX_CLIENTS; i++) { // start at 2 since 0 and 1 are for server's stdin and stdout
                comm_socket_fd = monitored_fd_set[i];
                if (comm_socket_fd != -1) {
                    write(comm_socket_fd, sync_msg, sizeof(sync_msg_t));
                }
            }
        }
//        else{
//            pass
//        }
    }
    return 0;
}
