#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define MAXNAME 80  /* maximum permitted name size, not including \0 */
#define NPITS 6  /* number of pits on a side, not including the end pit */
#define NPEBBLES 4 /* initial number of pebbles per pit */
#define MAXMESSAGE (MAXNAME + 50) /* initial number of pebbles per pit */

int port = 3000;
int listenfd;
struct player {
    int fd;
    char name[MAXNAME+1]; 
    int pits[NPITS+1];  // pits[0..NPITS-1] are the regular pits 
                        // pits[NPITS] is the end pit
    //other stuff undoubtedly needed here
    struct player *next;
    int player_num;
    int in_game; // 0 if they haven't yet been added to the game, 1 otherwise
    int my_turn; // 0 if it this players turn, 1 otherwise
};
struct player *playerlist = NULL;


extern void parseargs(int argc, char **argv);
extern void makelistener();
extern int compute_average_pebbles();
extern int game_is_over();  /* boolean */
extern void broadcast(char *s, struct player *exclusion, int prompt);  /* you need to write this one */

// CONNECT/DISCONNECT PROCESS
int new_conn_request(int fd);
void set_client_name(struct player *new_client, char *name, int newline_idx);
void add_user_to_game(struct player *client);
void disconnect_player(struct player *quitter, int close_fd);

// LINKEDLIST OPS
void add_player_to_head(struct player *player_ptr, int reset_vals);
struct player *remove_from_list(int client_fd, char *msg, int disconnect);
struct player *node_with_fd(int client_fd);
void free_players();

// GAMEPLAY
void prompt_for_move(int broadcast_prompt);
void process_move(struct player *client, int pit_to_move);
void make_move(struct player *player_side, int start_pit, int pebbles, int use_endpit);
int set_next_mover(struct player *current_mover);
void print_game_state(int fd);

// DATA PROCESSING
void handle_received_data(int client_fd);
int read_and_parse(int client_fd, int max_bytes, struct player *new_client);

// UTILITY FUNCTIONS 
void init_pebbles(struct player *player_ptr);
int find_newline_idx(const char *read_buf, int num_read);
void write_to_client(int client_fd, char *msg);

fd_set monitored_fds;

int main(int argc, char **argv) {
    char msg[MAXMESSAGE];
    
    parseargs(argc, argv);
    makelistener();     

    int max_fd = listenfd;
    FD_ZERO(&monitored_fds);
    FD_SET(listenfd, &monitored_fds);
    while (!game_is_over()) {
        fd_set monitored_fds_cpy = monitored_fds;
        int num_set = select(max_fd+1, &monitored_fds_cpy, NULL, NULL, NULL);
        if (num_set == -1){
            perror("server: select");
            exit(1);
        }
        if (FD_ISSET(listenfd, &monitored_fds_cpy)){
            // New connection request received
            int new_client_fd;
            new_client_fd = new_conn_request(listenfd);
            if (new_client_fd > max_fd){
                max_fd = new_client_fd; 
            }
            FD_SET(new_client_fd, &monitored_fds); // add it to our watch-pool

        }
        // Check the clients to see if any of them sent data
        int clients_found = 0;
        struct player *p = playerlist;
        while (p != NULL && clients_found < num_set){
            // is this a client that we're monitoring, and if so, did it just send us data?
            if (p->fd > -1 && FD_ISSET(p->fd, &monitored_fds_cpy)){
                // server received new data!
                handle_received_data(p->fd);
                clients_found += 1;
            }
            p = p->next;
        }
    }

    broadcast("Game over!", NULL, 0);
    printf("Game over!\n");
    for (struct player *p = playerlist; p; p = p->next) {
        if (p->in_game){
            int points = 0;
            for (int i = 0; i <= NPITS; i++) {
                points += p->pits[i];
            }
            printf("%s has %d points\r\n", p->name, points);
            snprintf(msg, MAXMESSAGE, "%s has %d points", p->name, points);
            broadcast(msg, NULL, 0);
        }
    }
    return 0;
}


void parseargs(int argc, char **argv) {
    int c, status = 0;
    while ((c = getopt(argc, argv, "p:")) != EOF) {
        switch (c) {
        case 'p':
            port = strtol(optarg, NULL, 0);
            break;
        default:
            status++;
        }
    }
    if (status || optind != argc) {
        fprintf(stderr, "usage: %s [-p port]\n", argv[0]);
        exit(1);
    }
}


void makelistener() {
    struct sockaddr_in r;

    if ((listenfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("socket");
        exit(1);
    }

    int on = 1;
    if (setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, 
               (const char *) &on, sizeof(on)) == -1) {
        perror("setsockopt");
        exit(1);
    }

    memset(&r, '\0', sizeof(r));
    r.sin_family = AF_INET;
    r.sin_addr.s_addr = INADDR_ANY;
    r.sin_port = htons(port);
    if (bind(listenfd, (struct sockaddr *)&r, sizeof(r))) {
        perror("bind");
        exit(1);
    }

    if (listen(listenfd, 5)) {
        perror("listen");
        exit(1);
    }
}


/* call this BEFORE linking the new player in to the list */
int compute_average_pebbles() { 
    struct player *p;
    int i;

    if (playerlist == NULL) {
        return NPEBBLES;
    }

    int nplayers = 0, npebbles = 0;
    for (p = playerlist; p; p = p->next) {
        nplayers++;
        for (i = 0; i < NPITS; i++) {
            npebbles += p->pits[i];
        }
    }
    return ((npebbles - 1) / nplayers / NPITS + 1);  /* round up */
}


int game_is_over() { /* boolean */
    int i;
    if (!playerlist) {
       return 0;  /* we haven't even started yet! */
    }

    for (struct player *p = playerlist; p; p = p->next) {
        int is_all_empty = 1;
        for (i = 0; i < NPITS; i++) {
            if (p->pits[i]) {
                is_all_empty = 0;
            }
        }
        if (is_all_empty) {
            return 1;
        }
    }
    return 0;
}


/**
 * Take the steps necessary to initialize a new player for the client
 * requesting to connect
 *
 * @param fd the file descriptor through which the connection request was received
 * @return the communication file descriptor for the client
 */
int new_conn_request(int fd){
    // initialize player
    struct player *player_ptr = malloc(sizeof(struct player));
    add_player_to_head(player_ptr, 1);

    int new_client_fd = accept(fd, NULL, NULL);
    if (new_client_fd < 0){
        perror("server: accept");
        fprintf(stderr, "There was an error connecting to new client\n");
        close(fd);
        free_players();
        exit(1);
    }else{
        char *welcome_str = "Welcome to Mancala. What is your name?";
        write_to_client(new_client_fd, welcome_str);
    }
    printf("Accepted a new connection\n");
    player_ptr->fd = new_client_fd;
    return new_client_fd;
}


/**
 * Validate and set a name for a client. Notify them if invalid.
 *
 * @param new_client the client
 * @param name the name to set
 * @param newline_idx the index at which the newline character was found in the name.
 *        -1 if no newline character was there
 */
void set_client_name(struct player *new_client, char *name, int newline_idx) {
    int name_len = (int) strlen(name);
    if ((strlen(new_client->name) + name_len) > MAXNAME){
        char *err = "The name you entered is too long. Disconnecting.";
        remove_from_list(new_client->fd, err, 2);
        return;
    }
    strncat(new_client->name, name, name_len);
    if (newline_idx != -1){ // if this is false, we only read part of a name. player not added til rest of name comes
        if (strlen(name) == 0){
            char *invalid_name = "Your username can't be empty. Please try again.";
            write_to_client(new_client->fd, invalid_name);
        }else{
            add_user_to_game(new_client);
        }
    }
}


/**
 * If the client's chosen name doesn't exist already, take the steps necessary to add them to the game. If someone
 * else joined between the time that this client connected and entered their name, then remove this client from
 * the playerlist, and re-add it to the head.
 *
 * @param client the user to add
 */
void add_user_to_game(struct player *client){
    struct player *p = playerlist;
    int name_exists = 0;
    // check if name exists
    while (p != NULL){
        if (p != client && strcmp(client->name, p->name) == 0){
            name_exists = 1;
            break;
        }
        p = p->next;
    }
    if (name_exists){
        char *name_err = "The username you chose already exists. Try again.";
        client->name[0] = '\0'; // Remove existing name
        write_to_client(client->fd, name_err);
    }else{
        // username is valid
        char *new_user = client->name;
        char *server_msg = malloc(MAXMESSAGE+1);
        snprintf(server_msg, MAXMESSAGE+1, "%s has joined the game.", new_user);
        client->in_game = 1;
        /*if (playerlist != client){ // check if someone else connected and joined before they entered their name
            remove_from_list(client->fd, NULL, 0);
            add_player_to_head(client, 0);
        }*/
        printf("%s\n", server_msg);
        broadcast(server_msg, client, 0);
        print_game_state(-1);

        free(server_msg);
    }
}


/**
 * Remove quitter from our monitored_fds fd_set, tell everyone this person is leaving, free all the memory
 * they are using, and (optionally) close their file descriptor
 *
 * @param quitter the person to disconnect
 * @param close_fd the persons file descriptor
 */
void disconnect_player(struct player *quitter, int close_fd) {
    if (quitter != NULL){
        FD_CLR(quitter->fd, &monitored_fds);
        if (quitter->in_game){
            char *leave_msg = malloc(MAXMESSAGE+1);
            snprintf(leave_msg, MAXMESSAGE+1, "%s has left the game.", quitter->name);
            broadcast(leave_msg, NULL, 1);
            printf("%s\n", leave_msg);
            free(leave_msg);
        }
        printf("A client has disconnected.\n");
        if (close_fd){
            close(quitter->fd);
        }
        free(quitter);
    }
}


/**
 * Add player_ptr to the head of playerlist
 *
 * @param player_ptr
 * @param reset_vals 1 if the attributes of player_ptr should be reset to default
 */
void add_player_to_head(struct player *player_ptr, int reset_vals){
    if (playerlist != NULL){
        player_ptr->player_num = (1 + playerlist->player_num);
        player_ptr->my_turn = 0;
    }else{
        player_ptr->player_num = 1;
        player_ptr->my_turn = 1;
    }
    if (reset_vals){
        player_ptr->name[0] = '\0';
        player_ptr->fd = -1;
        player_ptr->in_game = 0;
        init_pebbles(player_ptr);
    }
    player_ptr->next = playerlist;
    playerlist = player_ptr;
}


/**
 * Given client_fd and (possibly NULL) msg, take the steps necessary to safely remove
 * the corresponding client from the playerlist
 *
 * Precondition: client_fd must be the file descriptor of a client in playerlist
 *
 * @param client_fd clients file descriptor
 * @param msg: the message to send client before removal (optional)
 * @param disconnect: 0 if the player being removed should be left in the game, 1 if they should be removed and
 *                    2 if they should be removed AND have their file descriptor closed
 */
struct player *remove_from_list(int client_fd, char *msg, int disconnect) {
    struct player *removed_player = NULL;

    write_to_client(client_fd, msg);

    struct player *p = playerlist;
    if (p != NULL && p->fd == client_fd) { //  see if the last player that connected/last player in game is quitting
        removed_player = playerlist;
        playerlist = playerlist->next;
    }else {
        while (p != NULL) {
            p->player_num -= 1;
            if (p->next != NULL && p->next->fd == client_fd) {
                removed_player = p->next;
                p->next = removed_player->next;
                break;
            }
            p = p->next;
        }
    }
    if (removed_player != NULL && removed_player->my_turn){
        set_next_mover(removed_player);
    }
    if (disconnect) {
        if (disconnect == 2){
            disconnect_player(removed_player, 1);
        }else{
            disconnect_player(removed_player, 0);
        }
    }
    return removed_player;
}


/**
 * Free all the memory allocated by playerlist
 */
void free_players(){
    struct player *p = playerlist;
    while (p != NULL){
        struct player *free_player = p;
        p = p->next;
        free(free_player);
    }
}


/**
 * Compute whos turn it is, and tell them.
 *
 * @param broadcast_prompt: 1 if the next turn should be announced to everyone else in the game, 0 otherwise
 */
void prompt_for_move(int broadcast_prompt) {
    struct player *p = playerlist;
    while (p != NULL){
        // prompt current player for move
        if (p->in_game && p->my_turn){
            char *move_prompt = "Your move?";
            write_to_client(p->fd, move_prompt);
            if (broadcast_prompt){
                // tell everyone whos move it is
                char *move_msg = malloc(sizeof(char) * (MAXMESSAGE +1));
                snprintf(move_msg, MAXMESSAGE+1, "It is %s's move.", p->name);
                broadcast(move_msg, p, 0);
                printf("%s\n", move_msg);
                free(move_msg);
            }
            break;
        }
        p = p->next;
    }
}


/**
 * Validate the move requested by the client, and if valid, make the move. Show an error message otherwise.
 * After making the move, change the turn to the next player
 *
 * @param client the client requesting the move
 * @param pit_to_move the move they are requesting
 */
void process_move(struct player *client, int pit_to_move) {
    if (pit_to_move < 0){
        return;
    }else if (!client->my_turn){
        char *msg = "It is not your move.";
        write_to_client(client->fd, msg);
    }else{
        if (pit_to_move >= NPITS || (client->pits[pit_to_move] == 0)){
            if (pit_to_move >= NPITS){
                char *err = "Invalid move: You must enter a number that is within the bounds of your pits. Try again.";
                write_to_client(client->fd, err);
            }else{
                char *err = "Invalid move: The pit you chose is empty. Try again.";
                write_to_client(client->fd, err);
            }
            prompt_for_move(0);
        }else{ // Make the move
            int pebbles = client->pits[pit_to_move];
            client->pits[pit_to_move] = 0;
            make_move(client, pit_to_move+1, pebbles, 1);
            if (client->my_turn == 2){ // see if the player gets another turn
                // change it back
                client->my_turn = 1;
            } else{
                set_next_mover(client);
            }
            print_game_state(-1);
        }
    }
}

/**
 * Recursively place pebbles to the right one by one
 *
 * @param player_side: the player whos pits we're placing pebbles in
 * @param start_pit: the pit to start placing in
 * @param pebbles: the number of pebbles to place
 * @param use_endpit: 1 if a pebble should be put in the end pit, 0 otherwise
 */
void make_move(struct player *player_side, int start_pit, int pebbles, int use_endpit){
    if (player_side != NULL && player_side->in_game && pebbles > 0){
        for (int i = start_pit; i <= NPITS; i++){
            if (player_side->my_turn && i == NPITS && use_endpit && pebbles == 1){
                // worked out exactly. this player gets another turn
                player_side->my_turn = 2;
            }
            if (pebbles > 0 && ((i < NPITS) || use_endpit)){
                pebbles -= 1;
                player_side->pits[i] += 1;
            }else if(pebbles == 0){
                break;
            }
        }
    }
    if (pebbles > 0){
        struct player *next = playerlist;
        if (player_side != NULL){
            next = player_side->next;
        }
        make_move(next, 0, pebbles, 0);
    }
}


/**
 * Walk through the linkedlist, and print the board of each player
 *
 * @param fd -1 if the board should be broadcasted. Otherwise, the game state will only be printed to the client
 * with the correspnding fd
 */
void print_game_state(int fd){
    int print_prompt_at_end = 0;
    struct player *p = playerlist;
    while (p != NULL){
        if (p->in_game){
            char *write_buf = malloc(sizeof(char) * (MAXMESSAGE + 1));
            snprintf(write_buf, MAXNAME+4, "%s:  ", p->name); // name + : + 2 spaces + null terminator
            for (int i = 0; i < NPITS; i++){
                char sub_write_buf[MAXMESSAGE-strlen(write_buf) + 1];
                int num_written = snprintf(sub_write_buf, MAXMESSAGE-strlen(write_buf), "[%d]%d ", i, p->pits[i]);
                strncat(write_buf, sub_write_buf, num_written);
            }
            char sub_write_buf [MAXMESSAGE-strlen(write_buf) + 1];
            int num_written = snprintf(sub_write_buf, MAXMESSAGE-strlen(write_buf), " [end pit]%d", p->pits[NPITS]);
            strncat(write_buf, sub_write_buf, num_written);

            if (fd == -1){
                broadcast(write_buf, NULL, 0);
            }else{
                write_to_client(fd, write_buf);
            }
            print_prompt_at_end = 1; // game state will be printed on screen. re-print Move prompt at the end.
            printf("%s\n", write_buf);
            free(write_buf);
        }
        p = p->next;
    }
    if (print_prompt_at_end){
        prompt_for_move(1);
    }
}


/**
 * Check if the data being passed in is a name, or a move, and handle accordingly
 *
 * @param client_fd: file descriptor through which data arrived
 */
void handle_received_data(int client_fd){
    struct player *client = node_with_fd(client_fd);

    if (client != NULL && !client->in_game){
        // client isnt in the game. the data must be their name.
        read_and_parse(client_fd, MAXNAME, client);
    }else {
        int num_read = read_and_parse(client_fd, MAXMESSAGE, NULL);
        if (num_read > -1){ // received a new move
            process_move(client, num_read);
        }
    }
}

/**
 * Attempt to write msg to the specified file descriptor
 *
 * @param client_fd: the file descriptor to write a message in
 * @param msg: the message to write
 */
void write_to_client(int client_fd, char *msg) {
    if (msg != NULL && client_fd > -1){
        char *write_buf = malloc(MAXMESSAGE+1);
        snprintf(write_buf, MAXMESSAGE+1, "%s\r\n", msg);
        int bytes_written = (int) write(client_fd, write_buf, strlen(write_buf));
        if (bytes_written == -1){
            perror("write in write_to_client");
            free(write_buf);
            free_players();
            exit(1);
        }else if (bytes_written != strlen(write_buf)){
            fprintf(stderr, "Unexpected number of bytes written in write_to_client.\n");
            struct player *p = node_with_fd(client_fd);
            if (p){
                p->in_game = 0;
            }
            remove_from_list(client_fd, NULL, 2);
        }
        free(write_buf);
    }
}



/**
 * Read from file descriptor client_fd and parse
 *
 * @param client_fd
 * @param max_bytes
 * @param new_client: if not NULL, the data read will be  added to client->name
 * @return
 */
int read_and_parse(int client_fd, int max_bytes, struct player *new_client){
    char *read_buf = malloc(sizeof(char) * (max_bytes + 1)); // space for data + null terminator
//    char read_buf[max_bytes + 1]; // space for data + null terminator
    int num_read = (int) read(client_fd, read_buf, (size_t) max_bytes);
    if (num_read == -1){
        perror("read in read_and_parse() in server");
        free(read_buf);
        free_players();
        exit(1);
    }else if (num_read == 0){
        // EOF detected
        remove_from_list(client_fd, NULL, 1);
        free(read_buf);
        return -2; // -2 means a client disconnected
    }else{
        read_buf[num_read] = '\0'; // null terminator after the last letter
        int newline_idx = find_newline_idx(read_buf, num_read);
        if (newline_idx != -1){
            read_buf[newline_idx] = '\0';
        }
        if (new_client != NULL){    // client was provided so data read must be a name for the client
            set_client_name(new_client, read_buf, newline_idx);
            free(read_buf);
            return -1; // -1 means a new name was read
        }
        int read_int = (int) strtol(read_buf, NULL, 10);
        if (read_int < 0 || !strlen(read_buf)){ // if number they give is negative or if an empty line is read
            read_int = NPITS + 1; // this will trigger the "Invalid Move" warning
        }
        free(read_buf);
        return read_int;
    }
}



/**
 * Return the index of either \n or \r from \r\n in read_buf
 *
 * @param read_buf: string to look through
 * @param num_read: number of chars to check
 * @return index of newline character. -1 if none found
 */
int find_newline_idx(const char *read_buf, int num_read){
    int slash_r_idx = -1;
    for (int i = 0; i < num_read; i++){
        if (read_buf[i] == '\n'){
            if (slash_r_idx != -1){
                return slash_r_idx;
            }
            return i;
        }else if (read_buf[i] == '\r'){
            slash_r_idx = i;
        }
    }
    return -1;
}


/**
 * Find the next valid player in the linkedlist, and set their my_turn = 1
 *
 * @param current_mover: the player whos turn it currently is
 * @return: 0 on failure, 1 on success
 */
int set_next_mover(struct player *current_mover){
    struct player *start = playerlist;
    if (current_mover != NULL && current_mover->next != NULL){
        start = current_mover->next;
    }
    while (start != NULL){
        if (start->in_game){
            if (current_mover != NULL){
                current_mover->my_turn = 0;
            }
            start->my_turn = 1;
            return 1;
        }
        start = start->next;
    }
    return 0;
}


/**
 * Return the node for the player with the specified file descriptor
 *
 * @param client_fd
 * @return
 */
struct player *node_with_fd(int client_fd){
    struct player *p = playerlist;
    while (p != NULL && p->fd != client_fd){
        p = p->next;
    }
    return p;
}

/** Set the pebbles for player_ptr's pits **/
void init_pebbles(struct player *player_ptr){
    int num_pebbles;
    if (player_ptr->player_num == 1){
        num_pebbles = NPEBBLES;
    }else{
        num_pebbles = compute_average_pebbles();
    }
    for (int i = 0; i < NPITS; i++){
        player_ptr->pits[i] = num_pebbles;
    }
    player_ptr->pits[NPITS] = 0;
}

/**
 * Send out a message to every player in the game
 *
 * @param s: the message to send
 * @param exclusion: (optional) the player to exclude
 * @param prompt: 1 if the move prompt should be printed after the broadcast, 0 otherwise
 */
void broadcast(char *s, struct player *exclusion, int prompt) {
    struct player *p = playerlist; 
    while (p != NULL && p->fd > -1){
        if ((exclusion == NULL || p != exclusion) && p->in_game){
            write_to_client(p->fd, s);
        }
        p = p->next;
    }
    if (prompt){
        prompt_for_move(1);
    }
}