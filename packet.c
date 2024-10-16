#include "packet.h"

uint16_t compute_checksum(Packet *packet) {
    uint32_t sum = 0;
    uint16_t *ptr = (uint16_t *)packet;
    size_t count = (HEADER_SIZE + packet->header.length) / 2;

    packet->header.checksum = 0;  // Ensure checksum field is zero before calculation

    for (size_t i = 0; i < count; i++) {
        sum += ntohs(*ptr++);
        if (sum & 0xFFFF0000) {
            sum = (sum & 0xFFFF) + (sum >> 16);
        }
    }

    return ~(sum & 0xFFFF);
}

void serialize_packet(Packet *packet, uint8_t *buffer) {
    PacketHeader *header = &packet->header;

    uint32_t seq_num = htonl(header->seq_num);
    uint32_t ack_num = htonl(header->ack_num);
    uint16_t checksum = htons(header->checksum);
    uint16_t length = htons(header->length);
    uint8_t type = header->type;

    memcpy(buffer, &seq_num, sizeof(seq_num));
    memcpy(buffer + 4, &ack_num, sizeof(ack_num));
    memcpy(buffer + 8, &checksum, sizeof(checksum));
    memcpy(buffer + 10, &length, sizeof(length));
    memcpy(buffer + 12, &type, sizeof(type));
    memcpy(buffer + HEADER_SIZE, packet->payload, header->length);
}

void deserialize_packet(uint8_t *buffer, Packet *packet) {
    PacketHeader *header = &packet->header;

    memcpy(&header->seq_num, buffer, sizeof(header->seq_num));
    memcpy(&header->ack_num, buffer + 4, sizeof(header->ack_num));
    memcpy(&header->checksum, buffer + 8, sizeof(header->checksum));
    memcpy(&header->length, buffer + 10, sizeof(header->length));
    memcpy(&header->type, buffer + 12, sizeof(header->type));

    header->seq_num = ntohl(header->seq_num);
    header->ack_num = ntohl(header->ack_num);
    header->checksum = ntohs(header->checksum);
    header->length = ntohs(header->length);

    memcpy(packet->payload, buffer + HEADER_SIZE, header->length);
}
