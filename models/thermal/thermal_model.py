# SPDX-FileCopyrightText: 2026 ETH Zurich, University of Bologna and EssilorLuxottica SAS
#
# SPDX-License-Identifier: Apache-2.0
#
# Authors: Germain Haugou (germain.haugou@gmail.com)

"""Closed-loop thermal modeling.

This module provides the ``ThermalModel`` generator, a platform-level
component which periodically samples the power consumed at a list of
sync points (component paths), hands it to a thermal simulator, and
applies the returned temperatures back onto the power framework so
that subsequent power values account for them.

The sync points can be described in a YAML file (typically exported
from the thermal simulator side, where they correspond to its areas)
and loaded with :func:`load_thermal_yaml` / :func:`apply_thermal_yaml`::

    period: <sampling period, picoseconds>          # optional
    temp_ambient: <celsius>                         # optional
    temp_init: <celsius>                            # optional
    verbose: <true/false>                           # optional
    temp_min: <celsius>                             # optional, GUI color scale
    temp_max: <celsius>                             # optional, both or neither
    sync_points:
      <name>:
        path: <component path from the platform top>
        rth: <thermal resistance to ambient, K/W>   # optional
        tau: <thermal time constant, seconds>       # optional
        geometry:                                   # optional, GUI heat map
          x: <float>                                # floorplan rectangle in
          y: <float>                                # abstract units, y grows
          w: <float>                                # downward; w and h must
          h: <float>                                # be > 0

The ``geometry`` rectangles and the ``temp_min``/``temp_max`` color-scale
bounds are only used by the GUI heat-map view; the thermal model itself
ignores them. Without ``temp_min``/``temp_max`` the heat map auto-ranges
its color scale; sync points without ``geometry`` are laid out
automatically.

The component also declares a ``file`` target parameter, so a target
can embed a dormant ThermalModel and let the user activate it from the
command line without editing the target, and a ``verbose`` one forcing
the per-update trace lines on for a single run::

    gvrun --target=<target> --power --parameter <path>/file=<yaml> run
    gvrun --target=<target> --power --parameter <path>/verbose=true run
"""

from __future__ import annotations

import os
import sys
from typing import Annotated, ClassVar

import yaml

import gvsoc.gui
import gvsoc.systree
from config_tree import Config, cfg_field
from gvrun.parameter import TargetParameter
from gvrun.runtime import Runtime


class ThermalSyncPointConfig(Config):
    """One thermal sync point: a component subtree whose power consumption is
    periodically sampled and whose power sources receive the temperature
    computed by the thermal simulator.

    The Config ``name`` field carries the sync point name, used for the
    temperature VCD trace (``temp_<name>``) and the verbose log lines.
    """

    _defer_parent_init: ClassVar[bool] = True

    # Named component_path since 'path' is a reserved Config attribute
    # (the path of the config node itself)
    component_path: str = cfg_field(default='', desc=(
        "Component path from the platform top, '/'-separated (e.g. "
        "'chip/cluster'). The power consumption of the whole subtree below "
        "this component is sampled and its temperature is set."
    ))
    rth: float = cfg_field(default=10.0, desc=(
        "Thermal resistance to ambient, in K/W (built-in RC simulator only)"
    ))
    tau: float = cfg_field(default=0.010, desc=(
        "Thermal time constant, in seconds (built-in RC simulator only)"
    ))
    geo_x: float = cfg_field(default=0.0, desc=(
        "Floorplan rectangle X position, in abstract units (GUI heat map only)"
    ))
    geo_y: float = cfg_field(default=0.0, desc=(
        "Floorplan rectangle Y position, y grows downward (GUI heat map only)"
    ))
    geo_width: float = cfg_field(default=0.0, desc=(
        "Floorplan rectangle width; <= 0 means no geometry (GUI heat map only)"
    ))
    geo_height: float = cfg_field(default=0.0, desc=(
        "Floorplan rectangle height (GUI heat map only)"
    ))


class ThermalModelConfig(Config):
    """Configuration for the thermal model component.

    All the fields are runtime-annotated: their values are carried by the
    per-run runtime config file instead of the compiled platform tree, so
    changing the sync points file (or any other setting) does not require
    rebuilding the target.
    """

    period: Annotated[int, Runtime] = cfg_field(default=10_000_000_000, dump=True, desc=(
        "Sampling/update period, in picoseconds (default 10ms). Every period, "
        "the power consumed at each sync point is given to the thermal "
        "simulator and the temperatures it returns are applied."
    ))
    temp_ambient: Annotated[float, Runtime] = cfg_field(default=25.0, dump=True, desc=(
        "Ambient temperature, in celsius (built-in RC simulator only)"
    ))
    temp_init: Annotated[float, Runtime] = cfg_field(default=25.0, dump=True, desc=(
        "Initial temperature of all sync points, in celsius"
    ))
    verbose: Annotated[bool, Runtime] = cfg_field(default=False, dump=True, desc=(
        "Print one 'thermal <name> power_w=<power> temp_c=<temp>' line per "
        "sync point on every update (used by testbenches)"
    ))
    temp_min: Annotated[float, Runtime] = cfg_field(default=0.0, dump=True, desc=(
        "Heat-map color-scale lower bound, in celsius (GUI only). With "
        "temp_max <= temp_min (the default), the GUI auto-ranges the scale."
    ))
    temp_max: Annotated[float, Runtime] = cfg_field(default=0.0, dump=True, desc=(
        "Heat-map color-scale upper bound, in celsius (GUI only, see temp_min)"
    ))
    sync_points: Annotated[list[ThermalSyncPointConfig], Runtime] = cfg_field(
        default_factory=list, init=False, desc=(
        "Sync points with the thermal simulator"
    ))


def _resolve_path(path: str) -> str:
    # Resolve relative to the current directory first (typically the test
    # or run directory the file was given relative to on the command
    # line), then from PYTHONPATH like other model property files.
    if os.path.isabs(path) or os.path.exists(path):
        return path

    for dirpath in sys.path:
        full_path = os.path.join(dirpath, path)
        if os.path.exists(full_path):
            return full_path

    raise FileNotFoundError(f'Could not find thermal model file from PYTHONPATH: {path}')


# Optional top-level settings a thermal YAML file can carry, applied to
# the ThermalModelConfig by apply_thermal_yaml
_GLOBAL_KEYS = ('period', 'temp_ambient', 'temp_init', 'verbose', 'temp_min',
    'temp_max')


def _parse_geometry(path: str, name: str, geometry) -> dict:
    """Validate a sync point 'geometry' mapping and flatten it into
    ThermalSyncPointConfig geo_* kwargs."""
    if not isinstance(geometry, dict):
        raise ValueError(
            f'{path}: sync point "{name}" geometry must be a mapping')

    keys = set(geometry.keys())
    if keys != {'x', 'y', 'w', 'h'}:
        unknown = sorted(keys - {'x', 'y', 'w', 'h'})
        missing = sorted({'x', 'y', 'w', 'h'} - keys)
        details = []
        if missing:
            details.append(f'missing keys: {missing}')
        if unknown:
            details.append(f'unknown keys: {unknown}')
        raise ValueError(
            f'{path}: sync point "{name}" geometry has {", ".join(details)}')

    values = {key: float(value) for key, value in geometry.items()}
    if values['w'] <= 0.0 or values['h'] <= 0.0:
        raise ValueError(
            f'{path}: sync point "{name}" geometry w and h must be > 0')

    return {'geo_x': values['x'], 'geo_y': values['y'],
            'geo_width': values['w'], 'geo_height': values['h']}


def _parse_thermal_yaml(path: str) -> tuple[dict, list[ThermalSyncPointConfig]]:
    full_path = _resolve_path(path)
    with open(full_path, 'r', encoding='utf-8') as file_desc:
        content = yaml.safe_load(file_desc)

    if not isinstance(content, dict) or not isinstance(content.get('sync_points'), dict):
        raise ValueError(f'{path}: expected a top-level "sync_points" mapping')

    unknown = set(content.keys()) - {'sync_points', *_GLOBAL_KEYS}
    if unknown:
        raise ValueError(f'{path}: unknown top-level keys: {sorted(unknown)}')

    if ('temp_min' in content) != ('temp_max' in content):
        raise ValueError(f'{path}: temp_min and temp_max must be given together')
    if 'temp_min' in content and float(content['temp_max']) <= float(content['temp_min']):
        raise ValueError(f'{path}: temp_max must be > temp_min')

    sync_points = []
    for name, entry in content['sync_points'].items():
        if not isinstance(entry, dict):
            raise ValueError(f'{path}: sync point "{name}" must be a mapping')

        unknown = set(entry.keys()) - {'path', 'rth', 'tau', 'geometry'}
        if unknown:
            raise ValueError(
                f'{path}: sync point "{name}" has unknown keys: {sorted(unknown)}')

        if 'path' not in entry:
            raise ValueError(f'{path}: sync point "{name}" is missing "path"')

        kwargs = {'name': str(name), 'component_path': str(entry['path'])}
        if 'rth' in entry:
            kwargs['rth'] = float(entry['rth'])
        if 'tau' in entry:
            kwargs['tau'] = float(entry['tau'])
        if 'geometry' in entry:
            kwargs.update(_parse_geometry(path, name, entry['geometry']))
        sync_points.append(ThermalSyncPointConfig(**kwargs))

    settings = {key: content[key] for key in _GLOBAL_KEYS if key in content}
    return settings, sync_points


def load_thermal_yaml(path: str) -> list[ThermalSyncPointConfig]:
    """Load thermal sync points from a YAML file.

    The file is resolved relative to the current directory, then from
    PYTHONPATH like other model property files. See the module docstring
    for the schema; the optional top-level settings are ignored here —
    use :func:`apply_thermal_yaml` to get them applied too.

    Parameters
    ----------
    path: str
        Path of the YAML file.

    Returns
    -------
    list[ThermalSyncPointConfig]
        One config per sync point, suitable for the ``sync_points``
        config field, in file order.
    """
    return _parse_thermal_yaml(path)[1]


def apply_thermal_yaml(config: ThermalModelConfig, path: str):
    """Load a thermal YAML file and apply it onto a ThermalModelConfig.

    The sync points replace ``config.sync_points``, and the optional
    top-level settings (``period``, ``temp_ambient``, ``temp_init``,
    ``verbose``) override the corresponding config fields when present.

    Parameters
    ----------
    config: ThermalModelConfig
        Config to be updated.
    path: str
        Path of the YAML file.
    """
    settings, sync_points = _parse_thermal_yaml(path)

    if 'period' in settings:
        config.period = int(settings['period'])
    if 'temp_ambient' in settings:
        config.temp_ambient = float(settings['temp_ambient'])
    if 'temp_init' in settings:
        config.temp_init = float(settings['temp_init'])
    if 'verbose' in settings:
        config.verbose = bool(settings['verbose'])
    if 'temp_min' in settings:
        config.temp_min = float(settings['temp_min'])
    if 'temp_max' in settings:
        config.temp_max = float(settings['temp_max'])

    config.sync_points = sync_points


class ThermalModel(gvsoc.systree.Component):
    """Closed-loop thermal model.

    Overview
    ~~~~~~~~

    A platform-level component which closes the loop between the power
    framework and a thermal simulator. It is configured with a list of
    sync points, each identified by the path of a component — given
    inline or loaded from a YAML file with :func:`load_thermal_yaml`.
    Every ``period`` picoseconds it:

    1. Samples the energy consumed since the previous update by each
       sync point (through the never-reset total energy counters of the
       component's power trace, so it does not interfere with ``--power``
       report captures) and derives the average power over the interval.
    2. Hands the per-sync-point powers to the thermal simulator, which
       returns the new per-sync-point temperatures.
    3. Applies each temperature to all the power sources below the sync
       point component, so that temperature-dependent power tables
       (typically leakage) are re-evaluated.

    For now the thermal simulator is a built-in fake: each sync point is
    an independent first-order RC network to ambient
    (``T += dt/tau * (P*Rth + T_amb - T)``). The simulator sits behind a
    small C++ interface (``thermal_simulator.hpp``) so it can later be
    replaced by a connection to an external thermal simulator.

    Each sync point exposes a real-valued VCD trace ``temp_<name>`` with
    its temperature, and a child trace ``temp_<name>/power`` with the
    power it sampled at each update (W, averaged over the period).

    The component is inert (and warns once) when power modeling is not
    enabled (``--power``).

    The component declares a ``file`` target parameter carrying the path
    of a sync points YAML file. When set, the file is applied on top of
    the config (sync points and any period/temperature settings it
    defines). This lets a target embed a dormant thermal model — no sync
    points, fully inert — which the user activates per run::

        gvrun --target=<target> --power --parameter <path>/file=<yaml> run

    A ``verbose`` target parameter forces the per-update trace lines on
    for one run, without editing the file::

        gvrun --target=<target> --power --parameter <path>/verbose=true run

    or programmatically from a test ``config.py`` (the command line still
    wins over this)::

        def declare(target):
            target.set_parameter('thermal/file', 'thermal.yaml')

    Parameters
    ----------
    parent : Component
        Parent component, typically the platform top.
    name : str
        Local name of the component within ``parent``.
    config : ThermalModelConfig
        Optional configuration, including the sync point list. When not
        given, a default config is used and the sync points can only
        come from the ``file`` parameter.
    """

    def __init__(self, parent: gvsoc.systree.Component, name: str,
                 config: ThermalModelConfig | None = None):
        if config is None:
            config = ThermalModelConfig()
        super().__init__(parent, name, config=config)

        self._file = TargetParameter(
            self, name='file', value='',
            description='YAML file describing the thermal sync points; '
                        'overrides the config sync points and settings',
            cast=str,
        )

        self._verbose = TargetParameter(
            self, name='verbose', value=False,
            description='Force the per-update "thermal <name> power_w=<power> '
                        'temp_c=<temp>" lines on (one per sync point per '
                        'period, so a short period is very verbose). Can also '
                        'be enabled with the "verbose" key of the sync points '
                        'file.',
            cast=bool,
        )

        self.add_sources(['thermal/thermal_model.cpp'])

    def configure(self):
        # The file parameter is consumed here rather than in __init__ so it
        # can also be set programmatically (set_parameter('thermal/file',
        # ...) from a test config.py), which happens after the tree is
        # built. All the thermal config fields are runtime-carried, so this
        # is early enough: the runtime config file is dumped at run time.
        file = self._file.get_value()
        if file:
            apply_thermal_yaml(self.get_config(), file)

        # Applied after the file so the parameter can turn the trace lines
        # on for one run without editing it. It only forces them on: the
        # file (or the config) stays the way to enable them by default.
        if self._verbose.get_value():
            self.get_config().verbose = True

    def gen_gui(self, parent_signal):
        # Declare the temperature (and sampled power) of every sync point as
        # regular GUI signals, in the 'power' group so they show up in the
        # timeline exactly when power modeling is enabled (--power), like any
        # other power signal. Runs at GUI-config generation time, after
        # configure(), so the sync points file is already applied.
        config = self.get_config()
        if not config.sync_points:
            return parent_signal

        thermal = gvsoc.gui.Signal(self, parent_signal, name=self.get_name(),
            groups=['power'])
        for point in config.sync_points:
            temp = gvsoc.gui.Signal(self, thermal, name=f'temp_{point.name}',
                path=f'temp_{point.name}', display=gvsoc.gui.DisplayAnalog(),
                groups=['power'])
            gvsoc.gui.Signal(self, temp, name='power',
                path=f'temp_{point.name}/power',
                display=gvsoc.gui.DisplayAnalog(), groups=['power'])
        return parent_signal
