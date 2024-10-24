#include "packet.h"

uint16_t compute_checksum(uint8_t *data, size_t length) {
    uint32_t sum = 0;

    // Sum all 16-bit words
    for (size_t i = 0; i + 1 < length; i += 2) {
        uint16_t word = (data[i] << 8) | data[i + 1];
        sum += word;
        if (sum > 0xFFFF) {
            sum -= 0xFFFF; // Wrap around
        }
    }

    // Handle any remaining byte
    if (length % 2) {
        uint16_t word = data[length - 1] << 8; // Pad with zero
        sum += word;
        if (sum > 0xFFFF) {
            sum -= 0xFFFF;
        }
    }

    // One's complement
    return ~sum & 0xFFFF;
}

void serialize_packet(Packet *packet, uint8_t *buffer) {
    PacketHeader *header = &packet->header;

    // Convert header fields to network byte order
    uint32_t seq_num = htonl(header->seq_num);
    uint32_t ack_num = htonl(header->ack_num);
    uint32_t sack_num = htonl(header->sack_num);
    uint16_t length = htons(header->length);
    uint8_t retrans = header->retrans;
    uint8_t type = header->type;
    uint16_t checksum = 0; // Initialize checksum to zero

    // Serialize header fields
    memcpy(buffer, &seq_num, sizeof(seq_num));
    memcpy(buffer + 4, &ack_num, sizeof(ack_num));
    memcpy(buffer + 8, &sack_num, sizeof(sack_num));
    memcpy(buffer + 12, &checksum, sizeof(checksum)); // Placeholder for checksum
    memcpy(buffer + 14, &length, sizeof(length));
    memcpy(buffer + 16, &retrans, sizeof(retrans));
    memcpy(buffer + 17, &type, sizeof(type));
    memcpy(buffer + HEADER_SIZE, packet->payload, header->length);

    // Compute checksum over the entire packet (header + payload)
    checksum = compute_checksum(buffer, HEADER_SIZE + header->length);
    checksum = htons(checksum); // Convert checksum to network byte order

    // Insert checksum into buffer
    memcpy(buffer + 12, &checksum, sizeof(checksum));
}

void deserialize_packet(uint8_t *buffer, Packet *packet) {
    PacketHeader *header = &packet->header;

    // Extract header fields
    memcpy(&header->seq_num, buffer, sizeof(header->seq_num));
    memcpy(&header->ack_num, buffer + 4, sizeof(header->ack_num));
    memcpy(&header->sack_num, buffer + 8, sizeof(header->sack_num));
    memcpy(&header->checksum, buffer + 12, sizeof(header->checksum));
    memcpy(&header->length, buffer + 14, sizeof(header->length));
    memcpy(&header->retrans, buffer + 16, sizeof(header->retrans));
    memcpy(&header->type, buffer + 17, sizeof(header->type));

    // Convert fields from network byte order to host byte order
    header->seq_num = ntohl(header->seq_num);
    header->ack_num = ntohl(header->ack_num);
    header->sack_num = ntohl(header->sack_num);
    header->checksum = ntohs(header->checksum);
    header->length = ntohs(header->length);

    // Copy payload
    memcpy(packet->payload, buffer + HEADER_SIZE, header->length);
}
