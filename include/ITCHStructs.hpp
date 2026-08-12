#pragma once

#include <cstdint>

namespace itch {

#pragma pack(push, 1)

// Common 11-byte header present at the start of all NASDAQ ITCH 5.0 messages
struct ITCHHeader {
    char message_type;
    uint16_t stock_locate;
    uint16_t tracking_number;
    uint8_t timestamp[6]; // 48-bit Big-Endian nanoseconds since midnight
};

// Outer framing header for moldUDP/binary streams (2-byte packet length prefix)
struct PacketHeader {
    uint16_t length;
};

// Message Type 'S': System Event Message (12 bytes)
// Used for market lifecycle markers: 'Q' (Open), 'M' (Close), 'E' (End of System Hours)
struct SystemEventMessage {
    ITCHHeader header;
    char event_code;
};

// Message Type 'R': Stock Directory Message (39 bytes)
// Maps Stock Locate ID (uint16_t) to ticker symbol string (8 ASCII bytes)
struct StockDirectoryMessage {
    ITCHHeader header;
    char stock[8];
    char market_category;
    char financial_status_indicator;
    uint32_t round_lot_size;
    char round_lots_only;
    char issue_classification;
    char issue_subtype[2];
    char authenticity;
    char short_sale_threshold_indicator;
    char ipo_flag;
    char luld_ref_price_tier;
    char etp_flag;
    uint32_t etp_leverage_factor;
    char inverse_indicator;
};

// Message Type 'A': Add Order - No MPID (36 bytes)
struct AddOrderMessage {
    ITCHHeader header;
    uint64_t order_reference_number;
    char buy_sell_indicator; // 'B' = Buy, 'S' = Sell
    uint32_t shares;
    char stock[8];
    uint32_t price; // Fixed-point (divide by 10000.0 for USD)
};

// Message Type 'F': Add Order with MPID (40 bytes)
struct AddOrderMPIDMessage {
    ITCHHeader header;
    uint64_t order_reference_number;
    char buy_sell_indicator;
    uint32_t shares;
    char stock[8];
    uint32_t price;
    char attribution[4]; // 4-byte participant identifier (MPID)
};

// Message Type 'E': Order Executed (31 bytes)
struct OrderExecutedMessage {
    ITCHHeader header;
    uint64_t order_reference_number;
    uint32_t executed_shares;
    uint64_t match_number;
};

// Message Type 'C': Order Executed With Price (36 bytes)
struct OrderExecutedWithPriceMessage {
    ITCHHeader header;
    uint64_t order_reference_number;
    uint32_t executed_shares;
    uint64_t match_number;
    char printable;
    uint32_t execution_price;
};

// Message Type 'X': Order Cancel (23 bytes)
struct OrderCancelMessage {
    ITCHHeader header;
    uint64_t order_reference_number;
    uint32_t canceled_shares;
};

// Message Type 'D': Order Delete (19 bytes)
struct OrderDeleteMessage {
    ITCHHeader header;
    uint64_t order_reference_number;
};

// Message Type 'U': Order Replace (35 bytes)
struct OrderReplaceMessage {
    ITCHHeader header;
    uint64_t original_order_reference_number;
    uint64_t new_order_reference_number;
    uint32_t shares;
    uint32_t price;
};

#pragma pack(pop)

} // namespace itch