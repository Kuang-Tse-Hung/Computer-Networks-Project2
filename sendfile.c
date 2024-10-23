// sendfile.c

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/time.h>
#include <fcntl.h>
#include <math.h>
#include "packet.h"

#define MAX_PACKET_SIZE (HEADER_SIZE + MAX_PAYLOAD_SIZE)
#define WINDOW_SIZE 10
#define TIMEOUT_SEC 1  // Timeout in seconds

typedef struct {
    Packet *packets[WINDOW_SIZE];
    struct timeval time_sent[WINDOW_SIZE];
    uint32_t base_seq_num;
    uint32_t next_seq_num;
} SenderWindow;

int main(int argc, char *argv[]) {
    // Argument validation
    if (argc != 5 || strcmp(argv[1], "-r") != 0 || strcmp(argv[3], "-f") != 0) {
        fprintf(stderr, "Usage: sendfile -r <recv host>:<recv port> -f <filename>\n");
        exit(EXIT_FAILURE);
    }

    // Extract receiver host and port
    char *recv_host_port = argv[2];
    char *file_path = argv[4];

    // Split the host and port
    char *colon = strchr(recv_host_port, ':');
    if (!colon) {
        fprintf(stderr, "Invalid receiver address format. Use <recv host>:<recv port>\n");
        exit(EXIT_FAILURE);
    }

    *colon = '\0';
    char *recv_host = recv_host_port;
    uint16_t recv_port = atoi(colon + 1);

    // Open the file
    int file_fd = open(file_path, O_RDONLY);
    if (file_fd < 0) {
        fprintf(stderr, "Failed to open file: %s\n", file_path);
        perror("Error");
        exit(EXIT_FAILURE);
    }

    // Create a UDP socket
    int sockfd;
    struct sockaddr_in recv_addr;
    socklen_t addr_len = sizeof(recv_addr);
    uint8_t buffer[MAX_PACKET_SIZE];
    ssize_t num_bytes;

    if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("Socket creation failed");
        close(file_fd);
        exit(EXIT_FAILURE);
    }

    // Set up the receiver's address
    memset(&recv_addr, 0, sizeof(recv_addr));
    recv_addr.sin_family = AF_INET;
    recv_addr.sin_port = htons(recv_port);
    if (inet_pton(AF_INET, recv_host, &recv_addr.sin_addr) <= 0) {
        fprintf(stderr, "Invalid receiver IP address\n");
        close(sockfd);
        close(file_fd);
        exit(EXIT_FAILURE);
    }

    // Initialize sender window
    SenderWindow window;
    memset(&window, 0, sizeof(window));
    window.base_seq_num = 0;  // Will update after START packet
    window.next_seq_num = 0;

    // Set socket timeout for recvfrom (non-blocking)
    struct timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = 0;  // Non-blocking
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    // Dynamic calculation of the Retransmission Time Out (RTO)
    // Based on real-time RTT
    // Update every time receiving an ACK of a non-retransmitted packet
    // The time is measured in ms
    // Define the initial values and the update factors
    // Update Formula: 
    // SRTT_i = alpha * SRTT_i-1 + (1 - alpha) * RTT_i
    // RTTVAR_i = (1 - beta) * RTTVAR_i-1 + beta * |SRTT_i - RTT_i|
    // RTO_i = SRTT_i + 4 * RTTVAR_i
    long SRTT = 0;          // smoothed rtt
    long RTTVAR = 0.75;     // rtt variation
    long RTO = TIMEOUT_SEC * 1000000;  // initial rto
    long alpha = 0.125;     // SRTT update factor
    long beta = 0.25;       // RTTVAR update factor

    // Send start packet with filename
    Packet start_packet = {0};
    start_packet.header.seq_num = window.next_seq_num++;
    start_packet.header.type = PACKET_TYPE_START;
    start_packet.header.length = strlen(file_path);
    memcpy(start_packet.payload, file_path, start_packet.header.length);
    start_packet.header.retrans = 0;
    start_packet.header.checksum = compute_checksum(&start_packet);

    // Store the start packet in window
    int index = start_packet.header.seq_num % WINDOW_SIZE;  // Use modulo for circular buffer
    window.packets[index] = &start_packet;
    gettimeofday(&window.time_sent[index], NULL);

    printf("[debug] packet sending. seq_num: %d, type: %d, length: %d, retrans: %d\n", 
        start_packet.header.seq_num, start_packet.header.type, start_packet.header.length, start_packet.header.retrans);
    printf("[debug] the sending text: %s\n", start_packet.payload);

    serialize_packet(&start_packet, buffer);
    sendto(sockfd, buffer, HEADER_SIZE + start_packet.header.length, 0,
           (struct sockaddr *)&recv_addr, addr_len);
    printf("[send start packet] Seq: %u Filename: %s\n", start_packet.header.seq_num, file_path);

    // Wait for ACK of start packet
    while (1) {
        num_bytes = recvfrom(sockfd, buffer, MAX_PACKET_SIZE, 0,
                             (struct sockaddr *)&recv_addr, &addr_len);
        if (num_bytes > 0) {
            Packet ack_packet;
            deserialize_packet(buffer, &ack_packet);

            // Verify checksum
            uint16_t received_checksum = ack_packet.header.checksum;
            uint16_t computed_checksum = compute_checksum(&ack_packet);
            if (received_checksum != computed_checksum) {
                printf("[recv corrupt ack packet]\n");
                continue;
            }

            if (ack_packet.header.type == PACKET_TYPE_ACK &&
                ack_packet.header.ack_num == start_packet.header.seq_num + 1) {
                printf("[recv ack] Ack Num: %u\n", ack_packet.header.ack_num);

                // Update rto if it is not the ack for the retransmitted start packet
                if (!ack_packet.header.retrans) {
                    struct timeval now;
                    gettimeofday(&now, NULL);
                    struct timeval sent = window.time_sent[index];
                    double RTT = (now.tv_sec - sent.tv_sec) * 1000000 + (now.tv_usec - sent.tv_usec);
                    SRTT = RTT;
                    RTTVAR = RTT / 2;
                    RTO = SRTT + 4 * RTTVAR;
                }

                // Update base_seq_num
                window.base_seq_num = ack_packet.header.ack_num;
                printf("[update base_seq_num] base_seq_num: %u\n", window.base_seq_num);
                break;
            }
        } else {
            // Timeout, retransmit start packet
            usleep(RTO);

            start_packet.header.retrans = 1;
            start_packet.header.checksum = compute_checksum(&start_packet);
            serialize_packet(&start_packet, buffer);

            sendto(sockfd, buffer, HEADER_SIZE + start_packet.header.length, 0,
                   (struct sockaddr *)&recv_addr, addr_len);
            printf("[resend start packet] Seq: %u\n", start_packet.header.seq_num);
        }
    }

    // Main data transmission loop
    int eof = 0;
    while (!eof || window.base_seq_num < window.next_seq_num) {
        // Send packets within the window
        while (!eof && window.next_seq_num < window.base_seq_num + WINDOW_SIZE) {
            Packet *packet = malloc(sizeof(Packet));
            memset(packet, 0, sizeof(Packet));
            packet->header.seq_num = window.next_seq_num++;
            packet->header.type = PACKET_TYPE_DATA;
            packet->header.retrans = 0;

            // Read data from file
            packet->header.length = read(file_fd, packet->payload, MAX_PAYLOAD_SIZE);
            if (packet->header.length < 0) {
                perror("File read error");
                free(packet);
                close(file_fd);
                close(sockfd);
                exit(EXIT_FAILURE);
            } else if (packet->header.length == 0) {
                eof = 1;
                free(packet);
                window.next_seq_num--;  // Adjust seq_num as no data was read
                break;
            }

            packet->header.checksum = compute_checksum(packet);

            // Store packet in window
            int index = packet->header.seq_num % WINDOW_SIZE;  // Use modulo for circular buffer
            window.packets[index] = packet;
            gettimeofday(&window.time_sent[index], NULL);

            // Send packet
            serialize_packet(packet, buffer);
            sendto(sockfd, buffer, HEADER_SIZE + packet->header.length, 0,
                   (struct sockaddr *)&recv_addr, addr_len);
            printf("[send data] Seq: %u Length: %u\n", packet->header.seq_num, packet->header.length);
            printf("[debug] base_seq_num: %u, next_seq_num: %u, index: %d\n",
                   window.base_seq_num, window.next_seq_num, index);
        }

        // Receive ACKs
        timeout.tv_sec = 0;
        timeout.tv_usec = 100000;  // 100ms timeout for recvfrom
        setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        int sack = 0;
        while ((num_bytes = recvfrom(sockfd, buffer, MAX_PACKET_SIZE, 0,
                                     (struct sockaddr *)&recv_addr, &addr_len)) > 0) {
            
            printf("[debug] num of bytes received: %ld\n", num_bytes);
            Packet ack_packet;
            deserialize_packet(buffer, &ack_packet);

            // Verify checksum
            uint16_t received_checksum = ack_packet.header.checksum;
            uint16_t computed_checksum = compute_checksum(&ack_packet);
            if (received_checksum != computed_checksum) {
                printf("[recv corrupt ack packet]\n");
                continue;
            }

            if (ack_packet.header.type == PACKET_TYPE_ACK) {
                uint32_t ack_num = ack_packet.header.ack_num;
                printf("[recv ack] Ack Num: %u\n", ack_num);
                if (ack_packet.header.sack_num != ack_num)
                    printf("[recv sack] Sack Num: %u\n", ack_packet.header.sack_num);

                // update sack: always update with the largest one, as largest one indicates the latest situation:
                // when receiver gives an sack with a larger value, it means the receiver has filled the gap [ack: last_sack]
                sack = ack_packet.header.sack_num > sack ? ack_packet.header.sack_num : sack;

                if (ack_num > window.base_seq_num) {
                    // Mark packets as acknowledged
                    for (uint32_t i = window.base_seq_num; i < ack_num; i++) {
                        int index = i % WINDOW_SIZE;  // Use modulo for circular buffer
                        if (window.packets[index]) {
                            free(window.packets[index]);
                            window.packets[index] = NULL;
                            // window.acked[index] = 1;
                        }
                    }
                    window.base_seq_num = ack_num;  // Slide the window
                    printf("[slide window] new base_seq_num: %u\n", window.base_seq_num);
                }

                // Dynamically update the RTO
                // but the retransmitted packets are not included in the update process
                if (!ack_packet.header.retrans) {
                    struct timeval now;
                    gettimeofday(&now, NULL);
                    struct timeval sent = window.time_sent[index];
                    long RTT = (now.tv_sec - sent.tv_sec) * 1000000 + (now.tv_usec - sent.tv_usec);

                    if (SRTT == 0) {
                        // SRTT hasn't updated yet
                        SRTT = RTT;
                        RTTVAR = RTT / 2;
                    } 
                    else {
                        // smooth update
                        RTTVAR = (1 - beta) * RTTVAR + beta * fabs(SRTT - RTT);
                        SRTT = (1 - alpha) * SRTT + alpha * RTT;
                    }

                    RTO = SRTT + 4 * RTTVAR;
                }
                printf("[RTO update] new rto: %lu\n", RTO);
            }
        }

        // Check for rto and retransmit if necessary
        if (sack >= window.next_seq_num)
            continue;
        // retransmit until the sack
        struct timeval now;
        gettimeofday(&now, NULL);
        for (uint32_t i = window.base_seq_num; i < sack; i++) {
            int index = i % WINDOW_SIZE;
            if (window.packets[index]) {
                long elapsed = (now.tv_sec - window.time_sent[index].tv_sec) * 1000000 +
                               (now.tv_usec - window.time_sent[index].tv_usec);
                if (elapsed >= RTO) {
                    // Retransmit packet
                    Packet *packet = window.packets[index];
                    packet->header.retrans = 1;
                    packet->header.checksum = compute_checksum(packet);
                    serialize_packet(packet, buffer);
                    sendto(sockfd, buffer, HEADER_SIZE + packet->header.length, 0,
                           (struct sockaddr *)&recv_addr, addr_len);
                    gettimeofday(&window.time_sent[index], NULL);
                    printf("[resend data] Seq: %u Length: %u\n", packet->header.seq_num, packet->header.length);
                }
            }
        }
    }

    // Send end packet
    Packet end_packet = {0};
    end_packet.header.seq_num = window.next_seq_num++;
    end_packet.header.type = PACKET_TYPE_END;
    end_packet.header.retrans = 0;
    end_packet.header.checksum = compute_checksum(&end_packet);

    serialize_packet(&end_packet, buffer);
    sendto(sockfd, buffer, HEADER_SIZE, 0,
           (struct sockaddr *)&recv_addr, addr_len);
    printf("[send end packet] Seq: %u\n", end_packet.header.seq_num);

    // Set socket timeout before waiting for ACK
    timeout.tv_sec = RTO * 1000000;  // e.g., 1 second
    timeout.tv_usec = RTO % 1000000;
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    // Wait for ACK of end packet
    while (1) {
        num_bytes = recvfrom(sockfd, buffer, MAX_PACKET_SIZE, 0,
                             (struct sockaddr *)&recv_addr, &addr_len);
        if (num_bytes > 0) {
            Packet ack_packet;
            deserialize_packet(buffer, &ack_packet);
            if (ack_packet.header.type == PACKET_TYPE_ACK &&
                ack_packet.header.ack_num == end_packet.header.seq_num + 1) {
                printf("[recv ack] Ack Num: %u\n", ack_packet.header.ack_num);
                break;
            }
        } else {
            // Timeout occurred, retransmit end packet
            end_packet.header.retrans = 0;
            end_packet.header.checksum = compute_checksum(&end_packet);
            serialize_packet(&end_packet, buffer);
            sendto(sockfd, buffer, HEADER_SIZE, 0,
                   (struct sockaddr *)&recv_addr, addr_len);
            printf("[resend end packet] Seq: %u\n", end_packet.header.seq_num);
        }
    }

    // Clean up
    close(file_fd);
    close(sockfd);
    printf("[completed]\n");
    return 0;
}
