// Copyright information found in source file:
// Copyright 2022 ETH Zurich and University of Bologna.

// Licensing information found in source file:
// Licensed under Solderpad Hardware License, Version 0.51, see LICENSE for details.
// SPDX-License-Identifier: SHL-0.51

#include <cstdlib>
#include <stdlib.h>

#include <vp/vp.hpp>
#include <vp/itf/io.hpp>
#include <vp/controller.hpp>

// FOR PARSING
#include <sstream>
#include <iomanip>
#include <string>
#include <cstdint>

//
// Register map, based on CTU CAN FD controller TODO: minimalist version
// 

// ============================================================================
// -- Control Registers --
// ============================================================================
#define CAN_CONTROL_REG_BASE        0x000
#define CAN_CONTROL_MODE_REG        (CAN_CONTROL_REG_BASE + 0x00)
#define CAN_CONTROL_CMD_REG         (CAN_CONTROL_REG_BASE + 0x04)
#define CAN_CONTROL_STATUS_REG      (CAN_CONTROL_REG_BASE + 0x08)
#define CAN_CONTROL_FLAGS_REG       (CAN_CONTROL_REG_BASE + 0x0C)

// ============================================================================
// -- Transmit Buffers (8 x 0x100 blocks) --
// ============================================================================
#define CAN_TXT_BUF_BASE    0x100
#define CAN_TXT_BUF_SIZE    0x100
#define CAN_TXT_BUF(n)      (CAN_TXT_BUF_BASE + ((n) * CAN_TXT_BUF_SIZE))

// Buffer Offsets (Simplified)
#define CAN_FRAME_FMT_OFF   0x00  // Frame Format: DLC, flags, FDF, BRS...
#define CAN_IDENTIFIER_OFF  0x04  // ID
#define CAN_DATA_OFF        0x10  // Data start (0-64 bytes)

// Access Macros
#define CAN_BUF_FMT(n)      (CAN_TXT_BUF(n) + CAN_FRAME_FMT_OFF)
#define CAN_BUF_ID(n)       (CAN_TXT_BUF(n) + CAN_IDENTIFIER_OFF)
#define CAN_BUF_DATA(n)     (CAN_TXT_BUF(n) + CAN_DATA_OFF)

// ============================================================================

// -- CAN Controller Model --
enum CanMode {
    CAN_MODE_RESET,
    CAN_MODE_NORMAL,
    CAN_MODE_LISTEN_ONLY,
    CAN_MODE_LOOPBACK
};

enum CanCmd {
    CAN_CMD_NONE,
    CAN_CMD_TX_REQUEST = 1 << 0
};

struct CanFrame {
    uint32_t format;
    uint32_t id;
    uint32_t data[16]; // Max 64 bytes (CAN FD)
};

class CanController : public vp::Component
{
public:
    CanController(vp::ComponentConf &conf);
    void reset(bool active);

private:
    static vp::IoReqStatus req(vp::Block *__this, vp::IoReq *req);
    std::string parse_frame();
    bool send_frame();

    vp::Trace trace;
    vp::IoSlave input_itf;
    vp::WireMaster<bool> irq_itf; // TODO: unused for now

    // Registers
    uint32_t mode;
    uint32_t cmd;
    uint32_t status;
    uint32_t int_status;
    uint32_t int_enable;
    uint32_t flags;
    // Tx buffers TODO: only using 1 buffer for now
    CanFrame tx_buffer;
};


CanController::CanController(vp::ComponentConf &config) : vp::Component(config)
{
    this->traces.new_trace("trace", &this->trace, vp::DEBUG);

    this->input_itf.set_req_meth(&CanController::req);
    new_slave_port("input", &this->input_itf);

    this->new_master_port("irq", &this->irq_itf);
}


void CanController::reset(bool active)
{
    if (active)
    {
        this->mode             = CAN_MODE_RESET;
        this->cmd              = CAN_CMD_NONE;
        this->status           = 0;
        this->flags            = 0;
        this->tx_buffer.format = 0;
        this->tx_buffer.id     = 0;
        for (int i = 0; i < 16; i++)
            this->tx_buffer.data[i] = 0;
    }
}


vp::IoReqStatus CanController::req(vp::Block *__this, vp::IoReq *req)
{
    CanController *_this   = (CanController *)__this;
    uint64_t      offset   = req->get_addr();
    bool          is_write = req->get_is_write();
    uint64_t      size     = req->get_size();
    uint8_t       *data    = req->get_data();

    _this->trace.msg(vp::Trace::LEVEL_TRACE, "Received request (offset: 0x%x, size: 0x%d, is_write: %d)\n",
        req->get_addr(), req->get_size(), req->get_is_write());

    if (is_write)
    {
        // Write operation
        if ((offset == CAN_CONTROL_MODE_REG) && (size == 4))
        {
            // Set mode
            _this->mode = *((uint32_t *)data);
            _this->trace.msg(vp::Trace::LEVEL_TRACE, "Set mode to %d\n", _this->mode);
            return vp::IO_REQ_OK;
        }
        else if ((offset == CAN_CONTROL_CMD_REG) && (size == 4))
        {
            // Set command
            _this->cmd = *((uint32_t *)data);
            _this->trace.msg(vp::Trace::LEVEL_TRACE, "Set cmd to %d\n", _this->cmd);
            if (_this->cmd & CAN_CMD_TX_REQUEST)
            {
                _this->trace.msg(vp::Trace::LEVEL_INFO, "Transmit request received\n");
                if (_this->send_frame())
                {
                    _this->trace.msg(vp::Trace::LEVEL_INFO, "Frame transmitted successfully\n");
                    _this->status |= 1; // Set TX OK status bit
                }
                else
                {
                    _this->trace.msg(vp::Trace::LEVEL_ERROR, "Frame transmission failed\n");
                    _this->status |= 2; // Set TX ERROR status bit
                }
                
                _this->cmd &= ~CAN_CMD_TX_REQUEST; // Clear the TX request bit
            }
            return vp::IO_REQ_OK;
        }
        else if ((offset == CAN_CONTROL_FLAGS_REG) && (size == 4))
        {
            // Set flags
            _this->flags = *((uint32_t *)data);
            _this->trace.msg(vp::Trace::LEVEL_TRACE, "Set flags to %d\n", _this->flags);
            return vp::IO_REQ_OK;
        }
        else if ((offset == CAN_BUF_FMT(0)) && (size == 4))
        {
            // Set frame format
            _this->tx_buffer.format = *((uint32_t *)data);
            _this->trace.msg(vp::Trace::LEVEL_TRACE, "Set frame format to 0x%x\n", _this->tx_buffer.format);
            return vp::IO_REQ_OK;
        }
        else if ((offset == CAN_BUF_ID(0)) && (size == 4))
        {
            // Set identifier
            _this->tx_buffer.id = *((uint32_t *)data);
            _this->trace.msg(vp::Trace::LEVEL_TRACE, "Set identifier to 0x%x\n", _this->tx_buffer.id);
            return vp::IO_REQ_OK;
        }
        else if ((offset >= CAN_BUF_DATA(0)) && (offset < CAN_BUF_DATA(0) + 64) && (size <= 64))
        {
            // Set data
            size_t data_offset = offset - CAN_BUF_DATA(0);
            if (data_offset + size <= 64)
            {
                memcpy((uint8_t *)_this->tx_buffer.data + data_offset, data, size);
                _this->trace.msg(vp::Trace::LEVEL_TRACE, "Set data at offset %d, size %d\n", data_offset, size);
                _this->trace.msg(vp::Trace::LEVEL_TRACE, "Data: %02x %02x %02x %02x ...\n",
                    ((uint8_t *)_this->tx_buffer.data)[0],
                    ((uint8_t *)_this->tx_buffer.data)[1],
                    ((uint8_t *)_this->tx_buffer.data)[2],
                    ((uint8_t *)_this->tx_buffer.data)[3]);
                return vp::IO_REQ_OK;
            }
        }
        _this->trace.warning("Invalid write operation\n");
        return vp::IO_REQ_INVALID;
    }
    else
    {
        // Read operation
        if ((offset == CAN_CONTROL_MODE_REG) && (size == 4))
        {
            // Get mode
            *((uint32_t *)data) = _this->mode;
            _this->trace.msg(vp::Trace::LEVEL_TRACE, "Get mode: %d\n", _this->mode);
            return vp::IO_REQ_OK;
        }
        else if ((offset == CAN_CONTROL_STATUS_REG) && (size == 4))
        {
            // Get status
            *((uint32_t *)data) = _this->status;
            _this->trace.msg(vp::Trace::LEVEL_TRACE, "Get status: %d\n", _this->status);
            return vp::IO_REQ_OK;
        }
        else if ((offset == CAN_CONTROL_FLAGS_REG) && (size == 4))
        {
            // Get flags
            *((uint32_t *)data) = _this->flags;
            _this->trace.msg(vp::Trace::LEVEL_TRACE, "Get flags: %d\n", _this->flags);
            return vp::IO_REQ_OK;
        }

        _this->trace.warning("Invalid read operation\n");
        return vp::IO_REQ_INVALID;
    }
}


std::string CanController::parse_frame()
{
    // Extract CAN ID
    uint32_t id = tx_buffer.id & 0x1FFFFFFF;

    // Extract DLC (number of bytes)
    uint32_t dlc = tx_buffer.format & 0xFF;
    if (dlc > 64) dlc = 64;

    // Convert payload to hex string
    std::ostringstream data_hex;
    uint8_t *bytes = reinterpret_cast<uint8_t*>(tx_buffer.data);
    for (uint32_t i = 0; i < dlc; i++) {
        data_hex << std::hex << std::setfill('0') << std::setw(2)
                 << static_cast<int>(bytes[i]);
    }

    // TODO: Default FD flags (0xA = FDF + BRS)
    char flags = 'A';

    // Construct full cansend string (CAN FD)
    std::ostringstream cmd;
    cmd << std::hex << id << "##" << flags << data_hex.str();

    return cmd.str();
}


bool CanController::send_frame()
{
    // TODO: naive implementation using system call to cansend
    //  This should use socketcan to send the frame
    std::string frame_str = this->parse_frame();

    // Build full command
    std::string cmd = "cansend vcan0 " + frame_str;

    // Optional trace
    this->trace.msg("Sending: %s\n", cmd.c_str());

    // Execute cansend
    int ret = system(cmd.c_str());
    if (ret != 0) {
        this->trace.msg("cansend failed with code %d\n", ret);
        return false;
    }

    return true;
}


extern "C" vp::Component *gv_new(vp::ComponentConf &config)
{
    return new CanController(config);
}
