// SPDX-FileCopyrightText: 2026 ETH Zurich, University of Bologna and EssilorLuxottica SAS
//
// SPDX-License-Identifier: Apache-2.0
//
// Authors: Germain Haugou (germain.haugou@gmail.com)

// Test-and-set alias in front of a memory.
//
// A read is turned into "return the current content, then store the taken
// marker", which is the primitive software spinlocks are built on. Writes are
// forwarded untouched.
//
// The follow-up store is issued only once the read has actually completed, so
// the two orderings the io_v2 protocol allows are handled in the same place:
// when the memory answers inline (IO_REQ_DONE) that happens in req(), and when
// it defers (IO_REQ_GRANTED) it happens in the response callback.

#include <vector>
#include <vp/vp.hpp>
#include <vp/itf/io_v2.hpp>
#include <vp/debug_mem.hpp>
#include <interco/testandset_v2/testandset_config.hpp>

class Testandset : public vp::Component, public vp::DebugMemIf
{
public:
    Testandset(vp::ComponentConf &config);

    // Backdoor debug access (vp/debug_mem.hpp): a backdoor read must not take
    // the lock, so both views forward untouched to the memory downstream.
    // Without this, semi-hosting / GDB / the proxy would have no path to a
    // memory sitting behind this component.
    vp::DebugMemIf *debug_mem_if() override { return this; }
    int debug_mem_access(uint64_t addr, uint8_t *data, uint64_t size,
        bool is_write) override;
    void debug_mem_regions(std::vector<vp::DebugMemRegion> &regions,
        uint64_t local_base, uint64_t window_size, uint64_t entry_base,
        int depth) override;

    TestandsetConfig cfg;

private:
    vp::DebugMemIf *resolve_debug_mem();
    static vp::IoReqStatus input_req(vp::Block *__this, vp::IoReq *req);
    static vp::IoReqStatus passthrough_req(vp::Block *__this, vp::IoReq *req);
    static vp::IoRespAck output_resp(vp::Block *__this, vp::IoReq *req);
    static void output_retry(vp::Block *__this, vp::IoRetryChannel channel);

    void set(uint64_t addr, uint64_t size);

    vp::Trace trace;

    // Aliased view (test-and-set) and normal view. Both funnel into the single
    // master below, so the memory downstream keeps exactly one master.
    vp::IoSlave input_itf{&Testandset::input_req};
    vp::IoSlave passthrough_itf{&Testandset::passthrough_req};
    vp::IoMaster output_itf{&Testandset::output_retry, &Testandset::output_resp};

    // Which view a deferred request came in on, so its response goes back the
    // way it arrived.
    vp::IoSlave *pending_itf;

    // Pre-allocated request for the follow-up store, and the value it carries.
    vp::IoReq set_req;
    uint32_t set_value;
};

Testandset::Testandset(vp::ComponentConf &config)
    : vp::Component(config, this->cfg)
{
    this->traces.new_trace("trace", &this->trace, vp::DEBUG);

    this->new_slave_port("input", &this->input_itf);
    this->new_slave_port("passthrough", &this->passthrough_itf);
    this->new_master_port("output", &this->output_itf);

    this->set_value = (uint32_t)this->cfg.set_value;
    this->pending_itf = &this->input_itf;
}

void Testandset::set(uint64_t addr, uint64_t size)
{
    this->trace.msg(vp::Trace::LEVEL_TRACE,
        "Setting lock (addr: 0x%llx, size: 0x%llx, value: 0x%x)\n",
        (unsigned long long)addr, (unsigned long long)size, this->set_value);

    this->set_req.prepare();
    this->set_req.set_addr(addr);
    this->set_req.set_size(size);
    this->set_req.set_is_write(true);
    this->set_req.set_data((uint8_t *)&this->set_value);

    this->output_itf.req(&this->set_req);
}

vp::IoReqStatus Testandset::input_req(vp::Block *__this, vp::IoReq *req)
{
    Testandset *_this = (Testandset *)__this;

    uint64_t addr = req->get_addr();
    uint64_t size = req->get_size();
    bool is_write = req->get_is_write();

    _this->trace.msg(vp::Trace::LEVEL_DEBUG,
        "Received IO req (addr: 0x%llx, size: 0x%llx, is_write: %d)\n",
        (unsigned long long)addr, (unsigned long long)size, is_write ? 1 : 0);

    _this->pending_itf = &_this->input_itf;

    vp::IoReqStatus status = _this->output_itf.req(req);

    // Only a completed read owes a follow-up store. A deferred one is picked up
    // again in output_resp; a denied one is retried by the master and comes
    // back through here.
    if (status == vp::IO_REQ_DONE && !is_write &&
        req->get_resp_status() == vp::IO_RESP_OK)
    {
        _this->set(addr, size);
    }

    return status;
}


vp::IoReqStatus Testandset::passthrough_req(vp::Block *__this, vp::IoReq *req)
{
    Testandset *_this = (Testandset *)__this;

    _this->trace.msg(vp::Trace::LEVEL_TRACE,
        "Forwarding IO req (addr: 0x%llx, size: 0x%llx, is_write: %d)\n",
        (unsigned long long)req->get_addr(),
        (unsigned long long)req->get_size(),
        req->get_is_write() ? 1 : 0);

    _this->pending_itf = &_this->passthrough_itf;

    return _this->output_itf.req(req);
}

vp::IoRespAck Testandset::output_resp(vp::Block *__this, vp::IoReq *req)
{
    Testandset *_this = (Testandset *)__this;

    // The follow-up store completing is ours, not the initiator's: swallow it.
    if (req == &_this->set_req)
    {
        return vp::IO_RESP_ACCEPTED;
    }

    if (_this->pending_itf == &_this->input_itf &&
        !req->get_is_write() && req->get_resp_status() == vp::IO_RESP_OK)
    {
        _this->set(req->get_addr(), req->get_size());
    }

    return _this->pending_itf->resp(req);
}

void Testandset::output_retry(vp::Block *__this, vp::IoRetryChannel channel)
{
    Testandset *_this = (Testandset *)__this;
    _this->pending_itf->retry(channel);
}

vp::DebugMemIf *Testandset::resolve_debug_mem()
{
    std::vector<vp::SlavePort *> finals = this->output_itf.get_final_ports();
    if (finals.empty() || finals[0]->get_owner() == nullptr)
    {
        return nullptr;
    }
    return finals[0]->get_owner()->debug_mem_if();
}

int Testandset::debug_mem_access(uint64_t addr, uint8_t *data, uint64_t size,
    bool is_write)
{
    vp::DebugMemIf *target = this->resolve_debug_mem();
    return target ? target->debug_mem_access(addr, data, size, is_write) : -1;
}

void Testandset::debug_mem_regions(std::vector<vp::DebugMemRegion> &regions,
    uint64_t local_base, uint64_t window_size, uint64_t entry_base, int depth)
{
    if (depth >= vp::DebugMemIf::MAX_DEPTH)
    {
        return;
    }
    vp::DebugMemIf *target = this->resolve_debug_mem();
    if (target != nullptr)
    {
        target->debug_mem_regions(regions, local_base, window_size, entry_base,
            depth + 1);
    }
}

extern "C" vp::Component *gv_new(vp::ComponentConf &config)
{
    return new Testandset(config);
}
