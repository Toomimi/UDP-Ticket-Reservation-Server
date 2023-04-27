#include <iostream>
#include <fstream>
#include <getopt.h>
#include <sstream>
#include <unordered_map>
#include <cstring>
#include <utility>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstdarg>
#include <cassert>
#include <vector>

#ifdef NDEBUG
    const bool debug = false;
#else
    const bool debug = true;
#endif

#define ENSURE(x)                                                         \
    do {                                                                  \
        bool result = (x);                                                \
        if (!result) {                                                    \
            fprintf(stderr, "Error: %s was false in %s at %s:%d\n",       \
                #x, __func__, __FILE__, __LINE__);                        \
            exit(EXIT_FAILURE);                                           \
        }                                                                 \
    } while (0)

#define PRINT_ERRNO()                                                  \
    do {                                                               \
        if (errno != 0) {                                              \
            fprintf(stderr, "Error: errno %d in %s at %s:%d\n%s\n",    \
              errno, __func__, __FILE__, __LINE__, strerror(errno));   \
            exit(EXIT_FAILURE);                                        \
        }                                                              \
    } while (0)

#define CHECK_ERRNO(x)                                                             \
    do {                                                                           \
        errno = 0;                                                                 \
        (void) (x);                                                                \
        PRINT_ERRNO();                                                             \
    } while (0)

#define MAX_BUFFER_SIZE 65507
#define ONE_TICKET_SIZE 7
#define TICKET_MSG_INFO_SIZE 7
#define FIRST_RESERVATION_ID 1000000
#define COOKIE_SIZE 48
#define COOKIE_MIN_CHAR 33
#define COOKIE_RANGE 94

using namespace std;

enum message_id {GET_EVENTS = 1, EVENTS, GET_RESERVATION, RESERVATION,
                                    GET_TICKETS, TICKETS, BAD_REQUEST = 255};

using message_id_t = uint8_t;
using desc_len_t = uint8_t;
using desc_t = string;
using port_t = uint16_t;
using ticket_count_t = uint16_t;
using event_id_t = uint32_t;
struct event;
using event_map_t = unordered_map <event_id_t, event>;

using reservation_id_t = uint32_t;
using time_type = uint64_t;
struct reservation;
using reservation_vec_t = vector<reservation>;


struct event {
    ticket_count_t ticket_count;
    desc_len_t desc_len;
    desc_t desc;

    event( ticket_count_t ticketCount, desc_len_t descLen, desc_t desc)
    : ticket_count(ticketCount), desc_len(descLen), desc(std::move(desc)) {}
};

struct reservation {
    reservation_id_t reservation_id;
    event_id_t event_id;
    ticket_count_t tickets_count;
    string cookie;
    time_type expires;

    bool realized = false;
    bool expired = false;
    string generated_tickets;

    reservation(reservation_id_t reservation_id, event_id_t event_id,
                ticket_count_t tickets, string &cookie, time_type expires)
                : reservation_id(reservation_id), event_id(event_id),
                  tickets_count(tickets), cookie(cookie), expires(expires) {}
};

bool tickets_fit_in_UDP_datagram(ticket_count_t tc) {
    return TICKET_MSG_INFO_SIZE + (tc * ONE_TICKET_SIZE) <= MAX_BUFFER_SIZE;
}

void fatal(const char *fmt, ...) {
    va_list fmt_args;
    fprintf(stderr, "Error: ");
    va_start(fmt_args, fmt);
    vfprintf(stderr, fmt, fmt_args);
    va_end(fmt_args);
    fprintf(stderr, "\n");
    exit(EXIT_FAILURE);
}

void invalid_usage() { fatal("Invalid usage."); }


void read_options(int argc, char *argv[], port_t &port_number,
                                            int &timeout, string &file_name) {
    int opt;
    char *end_ptr;
    while ((opt = getopt(argc, argv, ";f:p:t:")) != -1) {
        switch (opt) {
            case 'f':
                file_name = optarg;
                break;
            case 'p':
                port_number = int(strtol(optarg, &end_ptr, 10));

                if (*end_ptr != '\0')
                    invalid_usage();
                break;
            case 't':
                timeout = int(strtol(optarg, &end_ptr, 10));
                if (timeout < 1 || timeout > 86400 || *end_ptr != '\0')
                    invalid_usage();
                break;
            default:
                invalid_usage();
        }
    }
}

 void read_file(string &file_name, event_map_t &event_map) {
    fstream file;
    file.open(file_name, ios::in);
    if (!file) {
        cerr << "Error: file couldn't be opened" << endl;
        exit(1);
    }

    event_id_t event_id = 0;
    ticket_count_t ticket_count = 0;
    desc_t desc;
    string line;
    while (getline(file, desc)) {
        getline(file, line);
        istringstream iss{line};
        iss >> ticket_count;
        event new_event(ticket_count, desc.size(), desc);
        event_map.insert({event_id, new_event});
        event_id++;
    }
    file.close();
}


int bind_socket(uint16_t port) {
    int socket_fd = socket(AF_INET, SOCK_DGRAM, 0); // creating IPv4 UDP socket
    ENSURE(socket_fd > 0);
    // after socket() call; we should close(sock) on any execution path;

    struct sockaddr_in server_address{};
    server_address.sin_family = AF_INET; // IPv4
    server_address.sin_addr.s_addr = htonl(INADDR_ANY); // listening on all interfaces
    server_address.sin_port = htons(port);

    // bind the socket to a concrete address
    CHECK_ERRNO(bind(socket_fd, (struct sockaddr *) &server_address,
                     (socklen_t) sizeof(server_address)));

    return socket_fd;
}

size_t read_message(int socket_fd, struct sockaddr_in *client_address, char *buffer, size_t max_length) {
    auto address_length = (socklen_t) sizeof(*client_address);
    int flags = 0; // we do not request anything special
    errno = 0;
    ssize_t len = recvfrom(socket_fd, buffer, max_length, flags,
                           (struct sockaddr *) client_address, &address_length);
    if (len < 0) {
        PRINT_ERRNO();
    }
    return (size_t) len;
}

void send_message(int socket_fd, const struct sockaddr_in *client_address, const char *message, size_t length) {
    auto address_length = (socklen_t) sizeof(*client_address);
    int flags = 0;
    ssize_t sent_length = sendto(socket_fd, message, length, flags,
                                 (struct sockaddr *) client_address, address_length);
    ENSURE(sent_length == (ssize_t) length);
}

class Server {
    port_t port;
    int socket_fd;

    port_t current_client_port = 0;
    char *current_client_ip;
    struct sockaddr_in client_address{};

    event_map_t event_map;

    time_type timeout;
    size_t first_not_expired_reserv = 0;
    reservation_id_t last_reservation_id = FIRST_RESERVATION_ID;
    reservation_vec_t reservation_vec{};

    string last_ticket = "0000000";
    short current_column = 6;

public:
    Server(port_t port_number, event_map_t event_map, time_type timeout)
        : port(port_number), event_map(std::move(event_map)), timeout(timeout) {
         socket_fd = bind_socket(port);
         char no_ip[16] = "No IP specified";
         current_client_ip = no_ip;
    }

    ~Server() {
        cout << "Closing connection" << endl;
        CHECK_ERRNO(close(socket_fd));
    }

    void listen() {
        char shared_buffer[MAX_BUFFER_SIZE];
        memset(shared_buffer, 0, sizeof(shared_buffer));
        size_t read_length;
        printf("Starting listening on port %u\n", port);
        do {
            printf("JA TUTAJ SŁUCHAM DO CHUJA PANA\n");
            read_length = read_message(socket_fd, &client_address, shared_buffer, sizeof(shared_buffer));
            current_client_ip = inet_ntoa(client_address.sin_addr);
            current_client_port = ntohs(client_address.sin_port);
            parse_received_message_and_answer(shared_buffer, read_length);
        } while (read_length > 0);
    }

    void parse_received_message_and_answer(char *buffer, size_t message_len) {
        string invalid_message = "Received invalid message, ignoring...\n";
        if ( message_len < 1) {
            cerr << invalid_message;
            return;
        }
        update_expirations();
        switch(buffer[0]) {
            case message_id::GET_EVENTS:
                if (message_len != 1)
                    cerr << invalid_message;
                else
                    answer_get_events(buffer);
                break;
            case message_id::GET_RESERVATION:
                if (message_len != 7)
                    cerr << invalid_message;
                else
                    answer_get_reservation(buffer);
                break;
            case message_id::GET_TICKETS:
                if (message_len != 53)
                    cerr << invalid_message;
                else
                    answer_get_tickets(buffer);
                break;
            default:
                cerr << invalid_message;
        }
    }

    template<typename T>
    void add_number_to_buffer(char *buffer, size_t &id, T el) {
        T big_endian_el = sizeof(T) == 4 ? htonl(el) :
                sizeof(T) == 2 ? htons(el) : htobe64(el);
        memcpy(buffer + id, &big_endian_el, sizeof(T));
        id += sizeof(T);
    }

    template<typename T>
    T get_number_from_buffer(char *buffer, size_t &id) {
        T el;
        memcpy(&el, buffer + id, sizeof(T));
        id += sizeof(T);
        el = sizeof(T) == 4 ? ntohl(el) : sizeof(T) == 2 ? ntohs(el) : be64toh(el);
        return el;
    }

    void update_expirations() {
        size_t i = first_not_expired_reserv;
        if (i >= reservation_vec.size())
            return;
        reservation curr = reservation_vec[i];
        do {
            curr = reservation_vec[i];
            i++;

            if (curr.realized)
                continue;

            time_type  actual_time = time(nullptr);
            if (actual_time >= curr.expires) {
                curr.expired = true;
                first_not_expired_reserv = i;
            }
        } while (curr.expired);
    }

    void write_received_message(const string &received_message) {
        if (debug)
            cout << "Received " << received_message << " message from "
                << current_client_ip << ":" << current_client_port << endl;
    }

    void write_sending_message(const string &sending_message) {
        if (debug)
            cout << "Sending " << sending_message << " message to "
                << current_client_ip << ":" << current_client_port << endl;
    }

    template<typename T>
    void send_bad_request(char *buffer, T id) {
        size_t buffer_index = 0;
        buffer[buffer_index] = message_id::BAD_REQUEST;
        add_number_to_buffer(buffer, buffer_index, id);
        write_sending_message("BAD_REQUEST");
        send_message(socket_fd, &client_address, buffer, buffer_index);
    }

    void answer_get_events(char *buffer) {
        write_received_message("GET_EVENTS");
        size_t buffer_index = 0;
        buffer[buffer_index] = message_id::EVENTS;
        buffer_index++;

        size_t basic_event_size = sizeof(event_id_t) +
                                    sizeof(ticket_count_t) + sizeof(desc_len_t);
        for (auto const& [event_id, event] : event_map) {
            size_t event_message_size = basic_event_size + event.desc_len;
            if (buffer_index + event_message_size >= MAX_BUFFER_SIZE) break;

            add_number_to_buffer(buffer,buffer_index, event_id);
            add_number_to_buffer(buffer,buffer_index, event.ticket_count);

            buffer[buffer_index] = event.desc_len;
            buffer_index++;

            memcpy(buffer + buffer_index, event.desc.c_str(), event.desc_len);
            buffer_index += event.desc_len;
        }
        write_sending_message("EVENTS");
        send_message(socket_fd, &client_address, buffer, buffer_index);
    }

    bool valid_get_reservations_request(char *buffer, event_id_t event_id,
                                        ticket_count_t cl_tc) {
        auto it = event_map.find(event_id);
        if (it == event_map.end()) {
            send_bad_request(buffer, event_id);
            return false;
        }

        event & chosen_event = it->second;
        if (cl_tc <= 0 || chosen_event.ticket_count < cl_tc
            || !tickets_fit_in_UDP_datagram(cl_tc)) {
            send_bad_request(buffer, event_id);
            return false;
        }
        chosen_event.ticket_count -= cl_tc;
        return true;
    }

    static string cookie_generator() {
        string cookie;
        cookie.reserve(COOKIE_SIZE);
        for (int i = 0; i < COOKIE_SIZE; i++) {
            cookie += char(COOKIE_MIN_CHAR + (rand() % COOKIE_RANGE));;
        }
        return cookie;
    }

    size_t fill_buffer_with_reservation(char *buffer, event_id_t  event_id,
                                                            ticket_count_t tc) {
        size_t buffer_index = 0;
        buffer[buffer_index] = message_id::RESERVATION;
        buffer_index++;
        add_number_to_buffer(buffer, buffer_index, last_reservation_id);
        add_number_to_buffer(buffer, buffer_index, event_id);
        add_number_to_buffer(buffer, buffer_index, tc);
        string cookie = cookie_generator();
        memcpy(buffer + buffer_index, cookie.c_str(), COOKIE_SIZE);
        buffer_index += COOKIE_SIZE;
        time_type expiration_time = time(nullptr) + timeout;
        add_number_to_buffer(buffer, buffer_index, expiration_time);

        reservation new_reservation{last_reservation_id,
                                    event_id, tc, cookie, expiration_time};

        reservation_vec.push_back(new_reservation);

        last_reservation_id++;
        return buffer_index;
    }

    void answer_get_reservation(char *buffer) {
        write_received_message("GET_RESERVATION");
        size_t buffer_index = 1;
        auto cl_event_id =
                get_number_from_buffer<event_id_t>(buffer, buffer_index);
        auto cl_ticket_count =
                get_number_from_buffer<ticket_count_t>(buffer, buffer_index);

        if(!valid_get_reservations_request(buffer,cl_event_id, cl_ticket_count))
            return;

        size_t buffer_len = fill_buffer_with_reservation(
                buffer,cl_event_id, cl_ticket_count);
        write_sending_message("RESERVATION");
        send_message(socket_fd, &client_address, buffer, buffer_len);
    }

    string generate_tickets (ticket_count_t tc) {
        string tickets;
        if (tc == 0)
            return "Tickets couldn't be generated";

        for (int i = 0; i < tc; i++) {
            if (last_ticket[current_column] == 'Z')
                current_column--;

            if (last_ticket[current_column] == '9')
                last_ticket[current_column] = 'A';
            else
                last_ticket[current_column] += 1;
            tickets += last_ticket;
        }
        return tickets;
    }

    void prepare_and_send_tickets_message(char *buffer, reservation_id_t reservation_id) {
        auto reservation = reservation_vec[reservation_id - FIRST_RESERVATION_ID];
        assert(reservation_id == reservation.reservation_id);
        size_t buffer_index = 0;
        buffer[buffer_index] = message_id::TICKETS;
        buffer_index++;

        add_number_to_buffer(buffer, buffer_index, reservation_id);
        add_number_to_buffer(buffer, buffer_index, reservation.tickets_count);
        string& tickets = reservation.generated_tickets;
        memcpy(buffer + buffer_index, tickets.c_str(),tickets.size());
        buffer_index += tickets.size();
        write_sending_message("TICKETS");
        send_message(socket_fd, &client_address, buffer, buffer_index);
    }

    void answer_get_tickets(char *buffer) {
        write_received_message("GET_TICKETS");
        size_t buffer_index = 1;
        auto reservation_id =
                get_number_from_buffer<reservation_id_t>(buffer, buffer_index);
        auto res_vec_id = reservation_id - FIRST_RESERVATION_ID;
        auto& reservation = reservation_vec[res_vec_id];
        assert(reservation.reservation_id == reservation_id);

        string cookie(buffer + buffer_index, buffer + buffer_index + COOKIE_SIZE);

        if (reservation.expired) {
            send_bad_request(buffer, reservation_id);
            return;
        }

        if (cookie == reservation.cookie) {
            if (!reservation.realized) {
                reservation.realized = true;
                string tickets = generate_tickets(reservation.tickets_count);
                assert(tickets.size() == 7 * reservation.tickets_count);
                reservation.generated_tickets = tickets;
            }
            prepare_and_send_tickets_message(buffer, reservation_id);
        }
        else {
            send_bad_request(buffer, reservation_id);
        }
    }


};

int main(int argc, char *argv[]) {
    port_t port_number = 2022;
    int timeout = 5;
    string file_name;
    read_options(argc, argv, port_number, timeout, file_name);
    event_map_t event_map;
    read_file(file_name, event_map);

    Server server(port_number, event_map, timeout);
    server.listen();

    return 0;
}
