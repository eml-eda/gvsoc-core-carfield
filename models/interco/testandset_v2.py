# SPDX-FileCopyrightText: 2026 ETH Zurich, University of Bologna and EssilorLuxottica SAS
#
# SPDX-License-Identifier: Apache-2.0
#
# Authors: Germain Haugou (germain.haugou@gmail.com)

"""Test-and-set alias in front of a memory.

PULP memories are exposed twice: once normally, and once through an alias where
a *read* atomically returns the previous content and writes an all-ones value.
That is what software spinlocks are built on (GAP9 maps the L2 private banks a
second time at ``L2_PRIV*_TS_ADDR``, and the cluster does the same for L1).

This component sits between the interconnect and the memory: it forwards the
request, then, for reads, issues a follow-up write of all-ones to the same
address. Writes pass straight through.

It fronts the memory on its own, so it carries both views. ``i_INPUT`` is the
aliased one that performs the test-and-set; ``i_PASSTHROUGH`` is the normal one
and only forwards. Keeping both here rather than putting a router in front is
what lets a single master drive the memory port, as io_v2 requires.
"""

from __future__ import annotations

from config_tree import Config, cfg_field
from gvsoc.systree import Component, SlaveItf
from gvsoc.signature import IoV2Sync


class TestandsetConfig(Config):
    set_value: int = cfg_field(default=0xFFFFFFFF, fmt="hex", dump=True, desc=(
        "Value written back after a read, i.e. the 'taken' marker of the lock"
    ))


class Testandset(Component):

    def __init__(self, parent: Component, name: str,
                 config: TestandsetConfig | None = None):

        if config is None:
            config = TestandsetConfig('testandset')

        super(Testandset, self).__init__(parent, name, config=config)

        self.add_sources(['interco/testandset_v2.cpp'])

    def i_INPUT(self) -> SlaveItf:
        """Aliased view: a read returns the old value and takes the lock."""
        return SlaveItf(self, 'input', signature=IoV2Sync())

    def i_PASSTHROUGH(self) -> SlaveItf:
        """Normal view: forwarded untouched."""
        return SlaveItf(self, 'passthrough', signature=IoV2Sync())

    def o_OUTPUT(self, itf: SlaveItf):
        self.itf_bind('output', itf, signature=IoV2Sync())
