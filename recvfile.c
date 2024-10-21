// recvfile.c

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/stat.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include "packet.h"

#define MAX_PACKET_SIZE (13 + MAX_PAYLOAD_SIZE)
#define WINDOW_SIZE 10

typedef struct {
    Packet *packets[WINDOW_SIZE];
    uint32_t base_seq_num;
} ReceiverWindow;

int main(int argc, char *argv[]) {
    if (argc != 3 || strcmp(argv[1], "-p") != 0) {
        fprintf(stderr, "Usage: recvfile -p <recv port>\n");
        exit(EXIT_FAILURE);
    }

    uint16_t recv_port = atoi(argv[2]);
    if (recv_port < 18000 || recv_port > 18200) {
        fprintf(stderr, "Port number must be between 18000 and 18200\n");
        exit(EXIT_FAILURE);
    }

    int sockfd;
    struct sockaddr_in recv_addr, sender_addr;
    socklen_t addr_len = sizeof(sender_addr);
    uint8_t buffer[MAX_PACKET_SIZE];
    ssize_t num_bytes;

    // Create UDP socket
    if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }

    // Bind the socket to the specified port
    memset(&recv_addr, 0, sizeof(recv_addr));
    recv_addr.sin_family = AF_INET;
    recv_addr.sin_addr.s_addr = INADDR_ANY;
    recv_addr.sin_port = htons(recv_port);

    if (bind(sockfd, (struct sockaddr *)&recv_addr, sizeof(recv_addr)) < 0) {
        perror("Bind failed");
        close(sockfd);
        exit(EXIT_FAILURE);
    }

    // Initialize the receiver window
    ReceiverWindow window;
    memset(&window, 0, sizeof(window));
    window.base_seq_num = 0;  // Will update after START packet

    FILE *fp = NULL;
    char filename[256] = {0};
    char subdir[100];
    struct stat st = {0};
    int expecting_start_packet = 1;

    printf("Receiver started, waiting for sender...\n");

    while (1) {
        // Receive a packet from the sender
        num_bytes = recvfrom(sockfd, buffer, MAX_PACKET_SIZE, 0,
                             (struct sockaddr *)&sender_addr, &addr_len);
        if (num_bytes < 0) {
            perror("recvfrom failed");
            continue;
        }

        Packet packet;
        deserialize_packet(buffer, &packet);

        // Verify checksum
        uint16_t received_checksum = packet.header.checksum;
        packet.header.checksum = 0;
        uint16_t computed_checksum = compute_checksum(&packet);
        if (received_checksum != computed_checksum) {
            printf("[recv corrupt packet]\n");
            continue;
        }

        // Handle START packet
        if (packet.header.type == PACKET_TYPE_START && expecting_start_packet) {
            strncpy(filename, (char *)packet.payload, packet.header.length);
            filename[packet.header.length] = '\0';  // Ensure null-termination

            // check subdir
            strncpy(subdir, filename, strrchr(filename, '/') - filename);
            subdir[strrchr(filename, '/') - filename] = '\0';
            if (stat(subdir, &st) == -1) {
                if (mkdir(subdir, 0700) != 0) {
                    perror("Failed to create directory");
                    exit(EXIT_FAILURE);
                }
                printf("Directory created: %s\n", subdir);
            }
            
            strcat(filename, ".recv");
            fp = fopen(filename, "wb");
            if (!fp) {
                perror("Failed to open file");
                exit(EXIT_FAILURE);
            }
            expecting_start_packet = 0;
            printf("[recv start packet] Filename: %s\n", filename);

            // Set base sequence number to the next expected sequence number
            window.base_seq_num = packet.header.seq_num + 1;
            printf("[update base_seq_num] base_seq_num: %u\n", window.base_seq_num);

            // Send ACK for the start packet
            Packet ack_packet = {0};
            ack_packet.header.type = PACKET_TYPE_ACK;
            ack_packet.header.ack_num = window.base_seq_num;
            ack_packet.header.checksum = compute_checksum(&ack_packet);

            serialize_packet(&ack_packet, buffer);
            sendto(sockfd, buffer, HEADER_SIZE, 0, (struct sockaddr *)&sender_addr, addr_len);
            printf("[send ack] Ack Num: %u\n", ack_packet.header.ack_num);
            continue;
        }

        // Ignore packets if we haven't received the start packet yet
        if (expecting_start_packet) {
            continue;
        }

        // Handle DATA packets
        if (packet.header.type == PACKET_TYPE_DATA) {
            uint32_t seq_num = packet.header.seq_num;
            printf("[recv data] Seq: %u Length: %u\n", seq_num, packet.header.length);

            // Calculate the index using modulo arithmetic for circular buffer
            int index = seq_num % WINDOW_SIZE;
            printf("[debug] base_seq_num: %u, seq_num: %u, index: %d\n",
                   window.base_seq_num, seq_num, index);

            // Send ACK for the received packet
            Packet ack_packet = {0};
            ack_packet.header.type = PACKET_TYPE_ACK;
            ack_packet.header.ack_num = seq_num + 1;
            ack_packet.header.checksum = compute_checksum(&ack_packet);

            serialize_packet(&ack_packet, buffer);
            sendto(sockfd, buffer, HEADER_SIZE, 0, (struct sockaddr *)&sender_addr, addr_len);
            printf("[send ack] Ack Num: %u\n", ack_packet.header.ack_num);

            // Check if the packet is within the window
            if (seq_num >= window.base_seq_num && seq_num < window.base_seq_num + WINDOW_SIZE) {
                // Store the packet if it hasn't been received before
                if (window.packets[index] == NULL) {
                    window.packets[index] = malloc(sizeof(Packet));
                    memcpy(window.packets[index], &packet, sizeof(Packet));
                }

                // Deliver all in-order packets
                while (window.packets[window.base_seq_num % WINDOW_SIZE]) {
                    Packet *p = window.packets[window.base_seq_num % WINDOW_SIZE];
                    fwrite(p->payload, 1, p->header.length, fp);
                    free(p);
                    window.packets[window.base_seq_num % WINDOW_SIZE] = NULL;
                    window.base_seq_num++;
                    printf("[slide window] new base_seq_num: %u\n", window.base_seq_num);
                }
            } else {
                printf("[packet outside window] Seq: %u\n", seq_num);
            }
        }

        // Handle END packet
        if (packet.header.type == PACKET_TYPE_END) {
            printf("[recv end packet]\n");

            // Send ACK for the END packet
            Packet ack_packet = {0};
            ack_packet.header.type = PACKET_TYPE_ACK;
            ack_packet.header.ack_num = packet.header.seq_num + 1;
            ack_packet.header.checksum = compute_checksum(&ack_packet);

            serialize_packet(&ack_packet, buffer);
            sendto(sockfd, buffer, HEADER_SIZE, 0, (struct sockaddr *)&sender_addr, addr_len);
            printf("[send ack] Ack Num: %u\n", ack_packet.header.ack_num);
            break;
        }
    }

    // Clean up
    if (fp) fclose(fp);
    close(sockfd);
    printf("[completed]\n");
    return 0;
}
