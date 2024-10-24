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
#include "packet.h"

#define MAX_PACKET_SIZE (HEADER_SIZE + MAX_PAYLOAD_SIZE)
#define FIXED_RTO 500000      // Fixed Retransmission Timeout in microseconds (500 ms)
#define MAX_CWND 1000.0       // Maximum congestion window size to limit memory usage
#define WINDOW_SIZE 1000      // Should be at least as big as MAX_CWND

typedef struct {
    Packet *packets[WINDOW_SIZE];
    struct timeval time_sent[WINDOW_SIZE];
    int acked[WINDOW_SIZE];
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

    // Congestion Control Variables
    double cwnd = 1.0;          // Start with a window size of 1 packet
    double ssthresh = 64.0;     // Initial slow start threshold
    const double max_cwnd = MAX_CWND; // Maximum window size to limit memory usage
    int dup_ack_count = 0;
    uint32_t last_ack_num = 0;

    // Set socket timeout for recvfrom
    struct timeval timeout;

    // Send start packet with filename
    Packet start_packet = {0};
    start_packet.header.seq_num = window.next_seq_num++;
    start_packet.header.type = PACKET_TYPE_START;
    start_packet.header.length = strlen(file_path);
    memcpy(start_packet.payload, file_path, start_packet.header.length);

    // Serialize packet (checksum computed inside serialize_packet)
    serialize_packet(&start_packet, buffer);
    sendto(sockfd, buffer, HEADER_SIZE + start_packet.header.length, 0,
           (struct sockaddr *)&recv_addr, addr_len);
    printf("[send start packet] Seq: %u Filename: %s\n", start_packet.header.seq_num, file_path);

    // Wait for ACK of start packet
    while (1) {
        // Set socket timeout to fixed RTO
        timeout.tv_sec = (int)(FIXED_RTO / 1000000);
        timeout.tv_usec = (int)(FIXED_RTO - (timeout.tv_sec * 1000000));
        setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

        num_bytes = recvfrom(sockfd, buffer, MAX_PACKET_SIZE, 0,
                             (struct sockaddr *)&recv_addr, &addr_len);
        if (num_bytes > 0) {
            // Verify checksum of received ACK packet
            uint16_t received_checksum;
            memcpy(&received_checksum, buffer + 8, sizeof(received_checksum));
            received_checksum = ntohs(received_checksum);

            // Zero out the checksum field in the buffer for calculation
            buffer[8] = 0;
            buffer[9] = 0;

            // Compute checksum over the received packet
            uint16_t computed_checksum = compute_checksum(buffer, num_bytes);

            if (computed_checksum != received_checksum) {
                printf("[recv corrupt ack]\n");
                continue; // Discard the packet
            }

            // Deserialize the packet
            Packet ack_packet;
            deserialize_packet(buffer, &ack_packet);

            if (ack_packet.header.type == PACKET_TYPE_ACK &&
                ack_packet.header.ack_num == start_packet.header.seq_num + 1) {
                printf("[recv ack] Ack Num: %u\n", ack_packet.header.ack_num);
                // Update base_seq_num
                window.base_seq_num = ack_packet.header.ack_num;
                printf("[update base_seq_num] base_seq_num: %u\n", window.base_seq_num);
                break;
            }
        } else {
            // Timeout, retransmit start packet
            printf("[timeout waiting for ack of start packet]\n");
            // Re-serialize and send the start packet
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
        while (!eof && window.next_seq_num < window.base_seq_num + (uint32_t)cwnd &&
               window.next_seq_num < window.base_seq_num + (uint32_t)max_cwnd) {
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

            // Store packet in window
            int index = packet->header.seq_num % WINDOW_SIZE;  // Use modulo for circular buffer
            window.packets[index] = packet;
            window.acked[index] = 0;
            gettimeofday(&window.time_sent[index], NULL);

            // Serialize packet (checksum computed inside serialize_packet)
            serialize_packet(packet, buffer);
            // Send packet
            sendto(sockfd, buffer, HEADER_SIZE + packet->header.length, 0,
                   (struct sockaddr *)&recv_addr, addr_len);
            printf("[send data] Seq: %u Length: %u\n", packet->header.seq_num, packet->header.length);
            printf("[debug] base_seq_num: %u, next_seq_num: %u, cwnd: %.2f, ssthresh: %.2f\n",
                   window.base_seq_num, window.next_seq_num, cwnd, ssthresh);
        }

        // Adjust socket timeout to fixed RTO
        timeout.tv_sec = (int)(FIXED_RTO / 1000000);
        timeout.tv_usec = (int)(FIXED_RTO - (timeout.tv_sec * 1000000));
        setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

        // Receive ACKs
        num_bytes = recvfrom(sockfd, buffer, MAX_PACKET_SIZE, 0,
                             (struct sockaddr *)&recv_addr, &addr_len);
        while (num_bytes > 0) {
            // Verify checksum of received ACK packet
            uint16_t received_checksum;
            memcpy(&received_checksum, buffer + 8, sizeof(received_checksum));
            received_checksum = ntohs(received_checksum);

            // Zero out the checksum field in the buffer for calculation
            buffer[8] = 0;
            buffer[9] = 0;

            // Compute checksum over the received packet
            uint16_t computed_checksum = compute_checksum(buffer, num_bytes);

            if (computed_checksum != received_checksum) {
                printf("[recv corrupt ack]\n");
                // Try receiving the next packet
                num_bytes = recvfrom(sockfd, buffer, MAX_PACKET_SIZE, 0,
                                     (struct sockaddr *)&recv_addr, &addr_len);
                continue; // Discard the packet
            }

            // Deserialize the packet
            Packet ack_packet;
            deserialize_packet(buffer, &ack_packet);

            if (ack_packet.header.type == PACKET_TYPE_ACK) {
                uint32_t ack_num = ack_packet.header.ack_num;
                printf("[recv ack] Ack Num: %u\n", ack_num);

                if (ack_num > window.base_seq_num) {
                    // Successful ACK, reset duplicate ACK count
                    dup_ack_count = 0;
                    last_ack_num = ack_num;

                    // Mark packets as acknowledged
                    for (uint32_t i = window.base_seq_num; i < ack_num; i++) {
                        int idx = i % WINDOW_SIZE;  // Use modulo for circular buffer
                        if (window.packets[idx]) {
                            free(window.packets[idx]);
                            window.packets[idx] = NULL;
                            window.acked[idx] = 1;
                        }
                    }
                    window.base_seq_num = ack_num;  // Slide the window
                    printf("[slide window] new base_seq_num: %u\n", window.base_seq_num);

                    // Update cwnd
                    if (cwnd < ssthresh) {
                        // Slow start
                        cwnd += 1.0;
                    } else {
                        // Congestion avoidance
                        cwnd += 1.0 / cwnd;
                    }

                    // Ensure cwnd does not exceed max_cwnd
                    if (cwnd > max_cwnd) cwnd = max_cwnd;

                } else if (ack_num == window.base_seq_num) {
                    // Duplicate ACK
                    dup_ack_count++;
                    if (dup_ack_count == 3) {
                        // Fast retransmit
                        printf("[fast retransmit] Ack Num: %u\n", ack_num);
                        ssthresh = cwnd / 2;
                        if (ssthresh < 1) ssthresh = 1;
                        cwnd = ssthresh + 3;

                        // Retransmit the missing packet
                        int index = window.base_seq_num % WINDOW_SIZE;
                        Packet *packet = window.packets[index];
                        if (packet) {
                            serialize_packet(packet, buffer);
                            sendto(sockfd, buffer, HEADER_SIZE + packet->header.length, 0,
                                   (struct sockaddr *)&recv_addr, addr_len);
                            gettimeofday(&window.time_sent[index], NULL);
                            printf("[retransmit data] Seq: %u Length: %u\n", packet->header.seq_num, packet->header.length);
                        }
                    }
                } else {
                    // ACK for a packet we've already acknowledged
                    printf("[recv old ack] Ack Num: %u\n", ack_num);
                }
            }

            // Try receiving the next packet
            num_bytes = recvfrom(sockfd, buffer, MAX_PACKET_SIZE, 0,
                                 (struct sockaddr *)&recv_addr, &addr_len);
        }

        // Check for timeouts and retransmit if necessary
        struct timeval now;
        gettimeofday(&now, NULL);
        for (uint32_t i = window.base_seq_num; i < window.next_seq_num; i++) {
            int index = i % WINDOW_SIZE;
            if (window.packets[index] && !window.acked[index]) {
                long elapsed = (now.tv_sec - window.time_sent[index].tv_sec) * 1000000 +
                               (now.tv_usec - window.time_sent[index].tv_usec);
                if (elapsed >= FIXED_RTO * 1.5) {
                    // Timeout occurred
                    printf("[timeout] Seq: %u\n", window.packets[index]->header.seq_num);
                    ssthresh = cwnd / 2;
                    if (ssthresh < 1) ssthresh = 1;
                    cwnd = 1.0; // Reset cwnd to 1
                    dup_ack_count = 0;

                    // Retransmit packet
                    Packet *packet = window.packets[index];
                    serialize_packet(packet, buffer);
                    sendto(sockfd, buffer, HEADER_SIZE + packet->header.length, 0,
                           (struct sockaddr *)&recv_addr, addr_len);
                    gettimeofday(&window.time_sent[index], NULL);
                    printf("[retransmit data] Seq: %u Length: %u\n", packet->header.seq_num, packet->header.length);
                }
            }
        }
    }

    // Send end packet
    Packet end_packet = {0};
    end_packet.header.seq_num = window.next_seq_num++;
    end_packet.header.type = PACKET_TYPE_END;

    // Serialize packet (checksum computed inside serialize_packet)
    serialize_packet(&end_packet, buffer);
    sendto(sockfd, buffer, HEADER_SIZE, 0,
           (struct sockaddr *)&recv_addr, addr_len);
    printf("[send end packet] Seq: %u\n", end_packet.header.seq_num);

    // Wait for ACK of end packet
    while (1) {
        // Set socket timeout to fixed RTO
        timeout.tv_sec = (int)(FIXED_RTO / 1000000);
        timeout.tv_usec = (int)(FIXED_RTO - (timeout.tv_sec * 1000000));
        setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

        num_bytes = recvfrom(sockfd, buffer, MAX_PACKET_SIZE, 0,
                             (struct sockaddr *)&recv_addr, &addr_len);
        if (num_bytes > 0) {
            // Verify checksum of received ACK packet
            uint16_t received_checksum;
            memcpy(&received_checksum, buffer + 8, sizeof(received_checksum));
            received_checksum = ntohs(received_checksum);

            // Zero out the checksum field in the buffer for calculation
            buffer[8] = 0;
            buffer[9] = 0;

            // Compute checksum over the received packet
            uint16_t computed_checksum = compute_checksum(buffer, num_bytes);

            if (computed_checksum != received_checksum) {
                printf("[recv corrupt ack]\n");
                continue; // Discard the packet
            }

            // Deserialize the packet
            Packet ack_packet;
            deserialize_packet(buffer, &ack_packet);

            if (ack_packet.header.type == PACKET_TYPE_ACK &&
                ack_packet.header.ack_num == end_packet.header.seq_num + 1) {
                printf("[recv ack] Ack Num: %u\n", ack_packet.header.ack_num);
                break;
            }
        } else {
            // Timeout occurred, retransmit end packet
            printf("[timeout waiting for ack of end packet]\n");
            // Re-serialize and send the end packet
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
