#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <sys/time.h>
#include <fcntl.h>
#include "packet.h"

#define MAX_PACKET_SIZE (13 + MAX_PAYLOAD_SIZE) // Example: adjust according to the structure of your packet
#define WINDOW_SIZE 10  
#define TIMEOUT_SEC 1    // Timeout for packet retransmission

typedef struct {
    Packet *packets[WINDOW_SIZE];
    struct timeval time_sent[WINDOW_SIZE];
    int acked[WINDOW_SIZE];
    uint32_t base_seq_num;
    uint32_t next_seq_num;
} SenderWindow;

int main(int argc, char *argv[]) {
    // Print arguments for debugging
    printf("argc: %d\n", argc);
    for (int i = 0; i < argc; i++) {
        printf("argv[%d]: %s\n", i, argv[i]);
    }

    // Argument validation
    if (argc != 5 || strcmp(argv[1], "-r") != 0 || strcmp(argv[3], "-f") != 0) {
        fprintf(stderr, "Usage: sendfile -r <recv host>:<recv port> -f <filename>\n");
        exit(EXIT_FAILURE);
    }

    // Extract receiver host and port
    char *recv_host_port = argv[2];
    char *file_path = argv[4];  // Correctly assign the file path argument

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
        close(file_fd);  // Clean up file descriptor
        exit(EXIT_FAILURE);
    }

    // Set up the receiver's address
    memset(&recv_addr, 0, sizeof(recv_addr));
    recv_addr.sin_family = AF_INET;
    recv_addr.sin_port = htons(recv_port);
    if (inet_pton(AF_INET, recv_host, &recv_addr.sin_addr) <= 0) {
        fprintf(stderr, "Invalid receiver IP address\n");
        close(sockfd);  // Clean up socket
        close(file_fd);
        exit(EXIT_FAILURE);
    }

    // Initialize sender window
    SenderWindow window;
    memset(&window, 0, sizeof(window));
    window.base_seq_num = 0;
    window.next_seq_num = 0;

    // Set socket timeout for recvfrom (for retransmissions)
    struct timeval timeout;
    timeout.tv_sec = TIMEOUT_SEC;
    timeout.tv_usec = 0;  // 1 second timeout
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    // Send start packet with filename
    Packet start_packet;
    memset(&start_packet, 0, sizeof(start_packet));
    start_packet.header.seq_num = window.next_seq_num++;
    start_packet.header.type = PACKET_TYPE_START;
    start_packet.header.length = strlen(file_path);
    memcpy(start_packet.payload, file_path, start_packet.header.length);
    start_packet.header.checksum = compute_checksum(&start_packet);

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
            if (ack_packet.header.type == PACKET_TYPE_ACK &&
                ack_packet.header.ack_num == start_packet.header.seq_num + 1) {
                printf("[recv ack] Ack Num: %u\n", ack_packet.header.ack_num);
                break;
            }
        } else {
            // Timeout, retransmit start packet
            sendto(sockfd, buffer, HEADER_SIZE + start_packet.header.length, 0,
                   (struct sockaddr *)&recv_addr, addr_len);
            printf("[resend start packet] Seq: %u\n", start_packet.header.seq_num);
        }
    }

    // Main data transmission loop
    int eof = 0;
    while (!eof || window.base_seq_num < window.next_seq_num) {
        // Send packets within window
        while (!eof && window.next_seq_num < window.base_seq_num + WINDOW_SIZE) {
            Packet *packet = malloc(sizeof(Packet));
            memset(packet, 0, sizeof(Packet));
            packet->header.seq_num = window.next_seq_num++;
            packet->header.type = PACKET_TYPE_DATA;

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
            int index = packet->header.seq_num % WINDOW_SIZE;
            window.packets[index] = packet;
            window.acked[index] = 0;
            gettimeofday(&window.time_sent[index], NULL);

            // Send packet
            serialize_packet(packet, buffer);
            sendto(sockfd, buffer, HEADER_SIZE + packet->header.length, 0,
                   (struct sockaddr *)&recv_addr, addr_len);
            printf("[send data] Seq: %u Length: %u\n", packet->header.seq_num, packet->header.length);
        }

        // Receive ACKs
        while ((num_bytes = recvfrom(sockfd, buffer, MAX_PACKET_SIZE, 0,
                                     (struct sockaddr *)&recv_addr, &addr_len)) > 0) {
            Packet ack_packet;
            deserialize_packet(buffer, &ack_packet);
            if (ack_packet.header.type == PACKET_TYPE_ACK) {
                uint32_t ack_num = ack_packet.header.ack_num;
                printf("[recv ack] Ack Num: %u\n", ack_num);

                if (ack_num > window.base_seq_num) {
                    // Mark packets as acknowledged
                    for (uint32_t i = window.base_seq_num; i < ack_num; i++) {
                        int index = i % WINDOW_SIZE;
                        if (window.packets[index]) {
                            free(window.packets[index]);
                            window.packets[index] = NULL;
                            window.acked[index] = 1;
                        }
                    }
                    window.base_seq_num = ack_num;
                }
            }
        }

        // Check for timeouts and retransmit if necessary
        struct timeval now;
        gettimeofday(&now, NULL);
        for (uint32_t i = window.base_seq_num; i < window.next_seq_num; i++) {
            int index = i % WINDOW_SIZE;
            if (window.packets[index] && !window.acked[index]) {
                long elapsed = (now.tv_sec - window.time_sent[index].tv_sec);
                if (elapsed >= TIMEOUT_SEC) {
                    // Retransmit packet
                    Packet *packet = window.packets[index];
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
    Packet end_packet;
    memset(&end_packet, 0, sizeof(end_packet));
    end_packet.header.seq_num = window.next_seq_num++;
    end_packet.header.type = PACKET_TYPE_END;
    end_packet.header.checksum = compute_checksum(&end_packet);

    serialize_packet(&end_packet, buffer);
    sendto(sockfd, buffer, HEADER_SIZE, 0,
           (struct sockaddr *)&recv_addr, addr_len);
    printf("[send end packet] Seq: %u\n", end_packet.header.seq_num);

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
            // Timeout, retransmit end packet
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
