#ifndef PACKET_H
#define PACKET_H

#include <stdint.h>
#include <stddef.h>
#include <arpa/inet.h>
#include <string.h>

#define MAX_PAYLOAD_SIZE 1024  // Adjust as needed for MTU considerations
#define HEADER_SIZE 13         // Size of PacketHeader when serialized

typedef enum {
    PACKET_TYPE_DATA,
    PACKET_TYPE_ACK,
    PACKET_TYPE_START,  // For initial handshake and metadata
    PACKET_TYPE_END     // To signify the end of transmission
} PacketType;

typedef struct {
    uint32_t seq_num;    // Sequence number
    uint32_t ack_num;    // Acknowledgment number
    uint16_t checksum;   // Checksum for error detection
    uint16_t length;     // Length of the payload
    uint8_t type;        // PacketType
} __attribute__((packed)) PacketHeader;

typedef struct {
    PacketHeader header;
    uint8_t payload[MAX_PAYLOAD_SIZE];
} Packet;

// Function declarations
uint16_t compute_checksum(uint8_t *data, size_t length);
void serialize_packet(Packet *packet, uint8_t *buffer);
void deserialize_packet(uint8_t *buffer, Packet *packet);

#endif // PACKET_H
