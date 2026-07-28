// SPDX-FileCopyrightText: 2026 ETH Zurich, University of Bologna and EssilorLuxottica SAS
//
// SPDX-License-Identifier: Apache-2.0
//
// Authors: Germain Haugou (germain.haugou@gmail.com)

#include <vp/vp.hpp>
#include <vp/itf/clock.hpp>
#include <utils/clock_divider/clock_divider_config.hpp>

class ClockDivider : public vp::Component {

public:
    ClockDivider(vp::ComponentConf &conf);

private:
    void reset(bool active) override;
    static void clock_sync(vp::Block *__this, bool value);
    static void ctrl_sync(vp::Block *__this, int value);
    static void enable_sync(vp::Block *__this, bool value);

    ClockDividerConfig cfg;
    vp::Trace trace;
    vp::ClockSlave input_itf;
    vp::ClockMaster output_itf;
    vp::WireSlave<int> ctrl_itf;
    vp::WireSlave<bool> enable_itf;

    int count;
    int value;
    bool enabled;
};

ClockDivider::ClockDivider(vp::ComponentConf &config)
    : vp::Component(config, this->cfg) {
    traces.new_trace("trace", &trace, vp::DEBUG);

    this->ctrl_itf.set_sync_meth(&ClockDivider::ctrl_sync);
    this->new_slave_port("clock_ctrl", &this->ctrl_itf);

    this->enable_itf.set_sync_meth(&ClockDivider::enable_sync);
    this->new_slave_port("enable", &this->enable_itf);

    this->input_itf.set_sync_meth(&ClockDivider::clock_sync);
    this->new_slave_port("clock_in", &this->input_itf);

    this->new_master_port("clock_out", &this->output_itf);
}

void ClockDivider::reset(bool active) {
    this->count = 0;
    this->value = 0;
    this->enabled = this->cfg.enabled;
}

void ClockDivider::clock_sync(vp::Block *__this, bool active) {
    ClockDivider *_this = (ClockDivider *)__this;

    // A disabled divider models a gated source: nothing downstream ever sees
    // an edge, so a consumer waiting on this clock simply never advances.
    if (!_this->enabled) {
        return;
    }

    _this->count++;
    if (_this->count == _this->cfg.divider) {
        _this->count = 0;
        _this->output_itf.sync(_this->value);
        _this->value ^= 1;
    }
}

void ClockDivider::ctrl_sync(vp::Block *__this, int value) {
    ClockDivider *_this = (ClockDivider *)__this;
    _this->count = 0;
    _this->cfg.divider = value;
}

void ClockDivider::enable_sync(vp::Block *__this, bool value) {
    ClockDivider *_this = (ClockDivider *)__this;

    _this->trace.msg(vp::Trace::LEVEL_DEBUG, "Changing clock gate (enabled: %d)\n", value);

    _this->count = 0;
    _this->enabled = value;
}

extern "C" vp::Component *gv_new(vp::ComponentConf &config) { return new ClockDivider(config); }
