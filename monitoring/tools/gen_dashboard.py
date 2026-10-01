#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Generates the Grafana dashboard for mxl-st2110-gateway (SPECIFICATION.md §12.2).

Deterministic output, no network access. CI runs `gen_dashboard.py --check`, which fails if the
committed monitoring/grafana/mxl-st2110-gateway.json differs from a fresh generation.

    python3 monitoring/tools/gen_dashboard.py            # regenerate
    python3 monitoring/tools/gen_dashboard.py --check    # verify the committed file
"""

import argparse
import difflib
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OUTPUT = ROOT / "monitoring" / "grafana" / "mxl-st2110-gateway.json"

# §12.1: the public metric names. Every query below may only use these.
METRICS = {
    "mxlgw_build_info", "mxlgw_ready", "mxlgw_restart_required",
    "mxlgw_ptp_locked", "mxlgw_ptp_selected", "mxlgw_ptp_selection_changes_total", "mxlgw_ptp_info",
    "mxlgw_ptp_offset_ns", "mxlgw_ptp_path_delay_ns", "mxlgw_ptp_utc_offset_seconds",
    "mxlgw_ptp_gm_changes_total", "mxlgw_ptp_sync_total", "mxlgw_ptp_errors_total",
    "mxlgw_clock_mtl_minus_host_tai_ns",
    "mxlgw_nic_link_up", "mxlgw_nic_link_speed_mbps", "mxlgw_nic_info",
    "mxlgw_nic_rx_packets_total", "mxlgw_nic_tx_packets_total", "mxlgw_nic_rx_bytes_total",
    "mxlgw_nic_tx_bytes_total", "mxlgw_nic_rx_errors_total", "mxlgw_nic_rx_missed_total",
    "mxlgw_essence_state", "mxlgw_rx_frames_total", "mxlgw_rx_leg_packets_total", "mxlgw_rx_packets_total",
    "mxlgw_rx_leg_seq_lost_total", "mxlgw_ingest_origin_age_ns",
    "mxlgw_mxl_grains_written_total", "mxlgw_mxl_samples_written_total", "mxlgw_mxl_write_errors_total",
    "mxlgw_mxl_grains_read_total", "mxlgw_mxl_read_timeouts_total", "mxlgw_mxl_late_reads_total",
    "mxlgw_mxl_grains_invalid_total", "mxlgw_mxl_flow_not_found_total", "mxlgw_mxl_read_lag_grains",
    "mxlgw_mxl_reader_info", "mxlgw_mxl_discovered_domains",
    "mxlgw_tx_frames_total", "mxlgw_tx_late_frames_total", "mxlgw_egress_lead_ns",
    "mxlgw_nmos_registered", "mxlgw_nmos_activations_total",
    "mxlgw_mxl_domain_bytes", "mxlgw_mxl_domain_flows",
}

DS = {"type": "prometheus", "uid": "${datasource}"}
INST = 'instance=~"$instance"'
ESS = 'instance=~"$instance", group=~"$group", essence=~"$essence"'

STATE_COLOURS = [
    ("running", "green"),
    ("waiting_for_flow", "yellow"),
    ("no_signal", "yellow"),
    ("degraded", "orange"),
    ("error", "red"),
    ("idle", "text"),
]


class Layout:
    """Places panels left to right in 24 grid columns and assigns stable ids."""

    def __init__(self):
        self.panels = []
        self.next_id = 1
        self.x = 0
        self.y = 0
        self.row_h = 0

    def row(self, title):
        self.newline()
        self.panels.append({"collapsed": False, "gridPos": {"h": 1, "w": 24, "x": 0, "y": self.y}, "id": self._id(), "panels": [], "title": title, "type": "row"})
        self.y += 1

    def add(self, panel, w, h):
        if self.x + w > 24:
            self.newline()
        panel["gridPos"] = {"h": h, "w": w, "x": self.x, "y": self.y}
        panel["id"] = self._id()
        self.panels.append(panel)
        self.x += w
        self.row_h = max(self.row_h, h)

    def newline(self):
        if self.x:
            self.y += self.row_h
        self.x = 0
        self.row_h = 0

    def _id(self):
        i = self.next_id
        self.next_id += 1
        return i


def target(expr, legend="", ref="A", instant=False, fmt=None):
    for name in re.findall(r"\bmxlgw_[a-z0-9_]+", expr):
        if name not in METRICS:
            raise SystemExit(f"unknown metric {name} in: {expr}")
    t = {"datasource": DS, "editorMode": "code", "expr": expr, "legendFormat": legend, "range": not instant, "refId": ref}
    if instant:
        t["instant"] = True
    if fmt:
        t["format"] = fmt
    return t


def targets(*items):
    return [target(*item, ref=chr(ord("A") + i)) if isinstance(item, tuple) else item for i, item in enumerate(items)]


def thresholds(*steps):
    out = [{"color": steps[0], "value": None}]
    for value, colour in zip(steps[1::2], steps[2::2]):
        out.append({"color": colour, "value": value})
    return {"mode": "absolute", "steps": out}


def stat(title, exprs, unit="none", mappings=None, steps=("green",), description="", text_mode="auto", no_value=None):
    defaults = {"color": {"mode": "thresholds"}, "mappings": mappings or [], "thresholds": thresholds(*steps), "unit": unit}
    if no_value:
        defaults["noValue"] = no_value
    return {
        "datasource": DS,
        "description": description,
        "fieldConfig": {"defaults": defaults, "overrides": []},
        "options": {"colorMode": "background", "graphMode": "none", "justifyMode": "auto", "orientation": "auto",
                    "reduceOptions": {"calcs": ["lastNotNull"], "fields": "", "values": False}, "textMode": text_mode},
        "targets": targets(*exprs),
        "title": title,
        "type": "stat",
    }


def timeseries(title, exprs, unit="none", description="", stack=False, min_zero=False, draw="line", steps=None):
    custom = {"drawStyle": draw, "fillOpacity": 10, "lineWidth": 1, "showPoints": "never", "spanNulls": False,
              "stacking": {"group": "A", "mode": "normal" if stack else "none"}}
    defaults = {"color": {"mode": "palette-classic"}, "custom": custom, "unit": unit}
    if min_zero:
        defaults["min"] = 0
    if steps:
        defaults["thresholds"] = thresholds(*steps)
        custom["thresholdsStyle"] = {"mode": "dashed"}
    return {
        "datasource": DS,
        "description": description,
        "fieldConfig": {"defaults": defaults, "overrides": []},
        "options": {"legend": {"calcs": ["lastNotNull", "max"], "displayMode": "table", "placement": "bottom", "showLegend": True},
                    "tooltip": {"mode": "multi", "sort": "desc"}},
        "targets": targets(*exprs),
        "title": title,
        "type": "timeseries",
    }


def state_timeline(title, exprs, mappings, description=""):
    return {
        "datasource": DS,
        "description": description,
        "fieldConfig": {"defaults": {"color": {"mode": "thresholds"}, "custom": {"fillOpacity": 80, "lineWidth": 0}, "mappings": mappings,
                                     "thresholds": thresholds("red", 1, "green")}, "overrides": []},
        "options": {"alignValue": "left", "legend": {"displayMode": "list", "placement": "bottom", "showLegend": False}, "mergeValues": True,
                    "rowHeight": 0.9, "showValue": "auto", "tooltip": {"mode": "single", "sort": "none"}},
        "targets": targets(*exprs),
        "title": title,
        "type": "state-timeline",
    }


def table(title, expr, columns, renames=None, overrides=None, description=""):
    """Instant-query table that shows only the given label columns (in this order)."""
    order = {c: i for i, c in enumerate(columns)}
    exclude = {"Time": True, "Value": True, "__name__": True, "job": True}
    return {
        "datasource": DS,
        "description": description,
        "fieldConfig": {"defaults": {"custom": {"align": "auto", "cellOptions": {"type": "auto"}, "filterable": True}}, "overrides": overrides or []},
        "options": {"cellHeight": "sm", "footer": {"show": False}, "showHeader": True},
        "targets": [target(expr, instant=True, fmt="table")],
        "title": title,
        "transformations": [
            {"id": "labelsToFields", "options": {"mode": "columns"}},
            {"id": "merge", "options": {}},
            {"id": "organize", "options": {"excludeByName": exclude, "includeByName": {c: True for c in columns}, "indexByName": order, "renameByName": renames or {}}},
        ],
        "type": "table",
    }


def value_map(*pairs):
    return [{"options": {str(k): {"color": colour, "index": i, "text": text} for i, (k, text, colour) in enumerate(pairs)}, "type": "value"}]


def text_colour_override(field, pairs):
    return {
        "matcher": {"id": "byName", "options": field},
        "properties": [
            {"id": "custom.cellOptions", "value": {"type": "color-background"}},
            {"id": "mappings", "value": [{"options": {k: {"color": c, "index": i} for i, (k, c) in enumerate(pairs)}, "type": "value"}]},
        ],
    }


def build():
    L = Layout()
    yes_no = value_map((0, "no", "red"), (1, "yes", "green"))

    # ------------------------------------------------------------------ Overview
    L.row("Overview")
    L.add(stat("Ready", [(f"mxlgw_ready{{{INST}}}", "{{instance}}")], mappings=value_map((0, "NOT READY", "red"), (1, "READY", "green")),
               description="/readyz: config valid, MTL up, PTP locked, clock offset in range, configured domains ok, NMOS registered (§10)."), 4, 4)
    L.add(stat("Restart required", [(f"mxlgw_restart_required{{{INST}}}", "{{instance}}")],
               mappings=value_map((0, "no", "green"), (1, "RESTART", "orange"))), 4, 4)
    L.add(stat("PTP locked", [(f"mxlgw_ptp_locked{{{INST}}}", "{{instance}} {{port}}")], mappings=value_map((0, "UNLOCKED", "red"), (1, "LOCKED", "green")),
               no_value="external / no MTL PTP", description="Per port; absent when ptp.mode = external or on the kernel backend (§5.2)."), 4, 4)
    L.add(stat("NMOS registered", [(f"mxlgw_nmos_registered{{{INST}}}", "{{instance}}")], mappings=yes_no), 4, 4)
    L.add(stat("MTL − host TAI", [(f'mxlgw_clock_mtl_minus_host_tai_ns{{{INST}, stat=""}}', "{{instance}}")], unit="ns",
               steps=("red", -1_000_000, "orange", -10_000, "green", 10_000, "orange", 1_000_000, "red"),
               description="§5.3: MTL PTP time minus the host CLOCK_TAI. Beyond ptp.max_offset_ns the gateway is not ready."), 4, 4)
    L.add(stat("Essences running", [(f'sum by (instance) (mxlgw_essence_state{{{INST}, state="running"}})', "{{instance}}")]), 4, 4)
    L.add(table("Grandmaster", f"mxlgw_ptp_info{{{INST}}}", ["instance", "port", "gm_identity", "parent_port_identity", "domain", "bind_mode"],
                renames={"gm_identity": "GM identity", "parent_port_identity": "parent port", "bind_mode": "bind mode"}), 12, 6)
    L.add(table("Build", f"mxlgw_build_info{{{INST}}}", ["instance", "version", "mtl", "dpdk", "mxl", "nmos_cpp"]), 12, 6)
    L.add(timeseries("MTL − host TAI offset", [
        (f'mxlgw_clock_mtl_minus_host_tai_ns{{{INST}, stat=""}}', "{{instance}}"),
        (f'mxlgw_clock_mtl_minus_host_tai_ns{{{INST}, stat="min"}}', "{{instance}} min 60 s"),
        (f'mxlgw_clock_mtl_minus_host_tai_ns{{{INST}, stat="max"}}', "{{instance}} max 60 s"),
    ], unit="ns", steps=("green", 10_000, "orange", 1_000_000, "red")), 24, 7)

    # ------------------------------------------------------------------ PTP
    L.row("PTP")
    L.add(timeseries("Offset from master", [
        (f'mxlgw_ptp_offset_ns{{{INST}, stat=""}}', "{{instance}} {{port}}"),
        (f'mxlgw_ptp_offset_ns{{{INST}, stat="min"}}', "{{instance}} {{port}} min 60 s"),
        (f'mxlgw_ptp_offset_ns{{{INST}, stat="max"}}', "{{instance}} {{port}} max 60 s"),
    ], unit="ns"), 12, 8)
    L.add(timeseries("Mean path delay", [
        (f'mxlgw_ptp_path_delay_ns{{{INST}, stat=""}}', "{{instance}} {{port}}"),
        (f'mxlgw_ptp_path_delay_ns{{{INST}, stat="min"}}', "{{instance}} {{port}} min 60 s"),
        (f'mxlgw_ptp_path_delay_ns{{{INST}, stat="max"}}', "{{instance}} {{port}} max 60 s"),
    ], unit="ns"), 12, 8)
    L.add(state_timeline("Lock per port", [(f"mxlgw_ptp_locked{{{INST}}}", "{{instance}} {{port}}")],
                         value_map((0, "unlocked", "red"), (1, "locked", "green"))), 8, 6)
    L.add(state_timeline("BMCA selection per port", [(f"mxlgw_ptp_selected{{{INST}}}", "{{instance}} {{port}}")],
                         value_map((0, "standby", "text"), (1, "selected", "blue")),
                         description="§5.5 dual-port BMCA: the port whose PTP instance steers the PHC."), 8, 6)
    L.add(timeseries("GM changes and selection changes", [
        (f"increase(mxlgw_ptp_gm_changes_total{{{INST}}}[$__rate_interval])", "{{instance}} {{port}} GM changes"),
        (f"increase(mxlgw_ptp_selection_changes_total{{{INST}}}[$__rate_interval])", "{{instance}} selection changes"),
    ], draw="bars", min_zero=True), 8, 6)
    L.add(timeseries("Sync rate", [(f"rate(mxlgw_ptp_sync_total{{{INST}}}[$__rate_interval])", "{{instance}} {{port}}")], unit="hertz", min_zero=True), 8, 6)
    L.add(timeseries("PTP errors", [(f"rate(mxlgw_ptp_errors_total{{{INST}}}[$__rate_interval])", "{{instance}} {{port}} {{kind}}")], unit="ops", min_zero=True), 8, 6)
    L.add(timeseries("UTC offset", [(f"mxlgw_ptp_utc_offset_seconds{{{INST}}}", "{{instance}} {{port}}")], unit="s"), 8, 6)

    # ------------------------------------------------------------------ NIC
    L.row("NIC")
    L.add(state_timeline("Link", [(f"mxlgw_nic_link_up{{{INST}}}", "{{instance}} {{port}}")], value_map((0, "down", "red"), (1, "up", "green"))), 8, 6)
    L.add(table("Ports", f"mxlgw_nic_info{{{INST}}}", ["instance", "port", "mac", "pci", "driver", "ddp_package"], renames={"ddp_package": "DDP package"}), 16, 6)
    L.add(timeseries("Throughput", [
        (f"rate(mxlgw_nic_rx_bytes_total{{{INST}}}[$__rate_interval]) * 8", "{{instance}} {{port}} rx"),
        (f"rate(mxlgw_nic_tx_bytes_total{{{INST}}}[$__rate_interval]) * 8", "{{instance}} {{port}} tx"),
    ], unit="bps", min_zero=True), 8, 7)
    L.add(timeseries("Packet rate", [
        (f"rate(mxlgw_nic_rx_packets_total{{{INST}}}[$__rate_interval])", "{{instance}} {{port}} rx"),
        (f"rate(mxlgw_nic_tx_packets_total{{{INST}}}[$__rate_interval])", "{{instance}} {{port}} tx"),
    ], unit="pps", min_zero=True), 8, 7)
    L.add(timeseries("Errors and missed", [
        (f"rate(mxlgw_nic_rx_errors_total{{{INST}}}[$__rate_interval])", "{{instance}} {{port}} errors"),
        (f"rate(mxlgw_nic_rx_missed_total{{{INST}}}[$__rate_interval])", "{{instance}} {{port}} missed"),
    ], unit="pps", min_zero=True, steps=("green", 0.001, "red")), 8, 7)
    L.add(stat("Link speed", [(f"mxlgw_nic_link_speed_mbps{{{INST}}}", "{{instance}} {{port}}")], unit="Mbits"), 24, 3)

    # ------------------------------------------------------------------ Ingest
    L.row("Ingest (ST 2110 → MXL)")
    ingest = f'{ESS}, direction="ingest"'
    L.add(timeseries("Frames by result", [(f"rate(mxlgw_rx_frames_total{{{ingest}}}[$__rate_interval])", "{{group}} / {{essence}} {{result}}")], unit="ops", min_zero=True), 12, 8)
    L.add(timeseries("ST 2022-7 sequence loss per leg", [(f"rate(mxlgw_rx_leg_seq_lost_total{{{ingest}}}[$__rate_interval])", "{{group}} / {{essence}} leg {{leg}}")],
                     unit="pps", min_zero=True, steps=("green", 0.001, "red"),
                     description="Packets lost on leg P vs R before the 2022-7 merge. Loss on one leg is harmless while the other is clean."), 12, 8)
    L.add(timeseries("Packets per leg", [
        (f"rate(mxlgw_rx_leg_packets_total{{{ingest}}}[$__rate_interval])", "{{group}} / {{essence}} leg {{leg}}"),
        (f"rate(mxlgw_rx_packets_total{{{ingest}}}[$__rate_interval])", "{{group}} / {{essence}} merged"),
    ], unit="pps", min_zero=True), 8, 7)
    L.add(timeseries("Origin age", [(f"mxlgw_ingest_origin_age_ns{{{ingest}}}", "{{group}} / {{essence}}")], unit="ns",
                     description="TAI now minus the origin time of the last frame written (§3.7)."), 8, 7)
    L.add(timeseries("MXL writes", [
        (f"rate(mxlgw_mxl_grains_written_total{{{ingest}}}[$__rate_interval])", "{{group}} / {{essence}} grains"),
        (f"rate(mxlgw_mxl_samples_written_total{{{ingest}}}[$__rate_interval])", "{{group}} / {{essence}} samples"),
        (f"rate(mxlgw_mxl_write_errors_total{{{ingest}}}[$__rate_interval])", "{{group}} / {{essence}} errors"),
    ], unit="ops", min_zero=True), 8, 7)

    # ------------------------------------------------------------------ Egress
    L.row("Egress (MXL → ST 2110)")
    egress = f'{ESS}, direction="egress"'
    L.add(timeseries("Late frames", [(f"rate(mxlgw_tx_late_frames_total{{{egress}}}[$__rate_interval])", "{{group}} / {{essence}}")],
                     unit="ops", min_zero=True, steps=("green", 0.001, "red")), 8, 7)
    L.add(timeseries("TX lead time", [(f"mxlgw_egress_lead_ns{{{egress}}}", "{{group}} / {{essence}}")], unit="ns", steps=("red", 0, "green"),
                     description="Time from grain available to its TX deadline; negative = late."), 8, 7)
    L.add(timeseries("Frames sent", [(f"rate(mxlgw_tx_frames_total{{{egress}}}[$__rate_interval])", "{{group}} / {{essence}}")], unit="ops", min_zero=True), 8, 7)
    L.add(timeseries("MXL read lag", [(f"mxlgw_mxl_read_lag_grains{{{egress}}}", "{{group}} / {{essence}}")], unit="none",
                     description="Writer head index minus read index, in grains (§5.7). Mirror flows on another host show the replication delay here."), 8, 7)
    L.add(timeseries("Flow-not-found retries", [(f"rate(mxlgw_mxl_flow_not_found_total{{{egress}}}[$__rate_interval])", "{{group}} / {{essence}}")],
                     unit="ops", min_zero=True, description="§5.8: receivers wait with backoff (500 ms → 5 s) until the flow or domain appears."), 8, 7)
    L.add(timeseries("No data / invalid / late reads", [
        (f"rate(mxlgw_mxl_read_timeouts_total{{{egress}}}[$__rate_interval])", "{{group}} / {{essence}} no data"),
        (f"rate(mxlgw_mxl_grains_invalid_total{{{egress}}}[$__rate_interval])", "{{group}} / {{essence}} invalid"),
        (f"rate(mxlgw_mxl_late_reads_total{{{egress}}}[$__rate_interval])", "{{group}} / {{essence}} late"),
    ], unit="ops", min_zero=True), 8, 7)
    L.add(table("Essence state", f"mxlgw_essence_state{{{ESS}}} == 1", ["instance", "direction", "group", "essence", "type", "state"],
                overrides=[text_colour_override("state", STATE_COLOURS)],
                description="waiting_for_flow and no_signal are expected while a flow is absent (§5.8); they do not affect readiness."), 12, 8)
    L.add(table("Resolved MXL domains", f"mxlgw_mxl_reader_info{{{ESS}}}", ["instance", "group", "essence", "domain_kind", "domain_id", "domain_path", "flow_id"],
                renames={"domain_kind": "kind", "domain_id": "domain id", "domain_path": "path", "flow_id": "flow id"},
                overrides=[text_colour_override("kind", [("mirror", "purple"), ("discovered", "blue"), ("configured", "green")])],
                description="Domain each enabled MXL Receiver reads from; mirror = replicated by mxl-fabrics-agent (§8.6)."), 12, 8)
    L.add(timeseries("Grains read", [(f"rate(mxlgw_mxl_grains_read_total{{{egress}}}[$__rate_interval])", "{{group}} / {{essence}}")], unit="ops", min_zero=True), 12, 7)
    L.add(timeseries("Essences by state", [(f"sum by (state) (mxlgw_essence_state{{{ESS}}})", "{{state}}")], stack=True, min_zero=True), 12, 7)

    # ------------------------------------------------------------------ NMOS
    L.row("NMOS")
    L.add(state_timeline("Registered", [(f"mxlgw_nmos_registered{{{INST}}}", "{{instance}}")], value_map((0, "not registered", "orange"), (1, "registered", "green"))), 8, 6)
    L.add(timeseries("IS-05 activations", [(f"increase(mxlgw_nmos_activations_total{{{INST}}}[$__rate_interval])", "{{instance}} {{kind}} {{transport}} {{result}}")],
                     draw="bars", min_zero=True), 16, 6)

    # ------------------------------------------------------------------ MXL domains
    L.row("MXL domains")
    L.add(timeseries("Domain usage", [(f"mxlgw_mxl_domain_bytes{{{INST}}}", "{{instance}} {{domain}} {{kind}}")], unit="bytes", stack=True, min_zero=True), 12, 7)
    L.add(timeseries("Flows per domain", [(f"mxlgw_mxl_domain_flows{{{INST}}}", "{{instance}} {{domain}}")], min_zero=True), 6, 7)
    L.add(stat("Discovered domains", [(f"mxlgw_mxl_discovered_domains{{{INST}}}", "{{kind}}")],
               steps=("green",), description="Domains found under mxl.scan_path (§8.5); conflict = duplicate domain ids."), 6, 7)

    L.newline()
    return {
        "annotations": {"list": [{"builtIn": 1, "datasource": {"type": "grafana", "uid": "-- Grafana --"}, "enable": True, "hide": True,
                                  "iconColor": "rgba(0, 211, 255, 1)", "name": "Annotations & Alerts", "type": "dashboard"}]},
        "description": "mxl-st2110-gateway: PTP, NIC, ST 2110 ingest/egress, MXL readers and domains, NMOS (generated by monitoring/tools/gen_dashboard.py)",
        "editable": True,
        "fiscalYearStartMonth": 0,
        "graphTooltip": 1,
        "links": [],
        "panels": L.panels,
        "refresh": "10s",
        "schemaVersion": 39,
        "tags": ["mxl", "st2110", "nmos"],
        "templating": {"list": [
            {"current": {}, "hide": 0, "includeAll": False, "label": "Data source", "multi": False, "name": "datasource", "options": [],
             "query": "prometheus", "refresh": 1, "regex": "", "type": "datasource"},
            variable("instance", "Instance", "label_values(mxlgw_build_info, instance)"),
            variable("group", "Group", 'label_values(mxlgw_essence_state{instance=~"$instance"}, group)'),
            variable("essence", "Essence", 'label_values(mxlgw_essence_state{instance=~"$instance", group=~"$group"}, essence)'),
        ]},
        "time": {"from": "now-1h", "to": "now"},
        "timepicker": {},
        "timezone": "",
        "title": "mxl-st2110-gateway",
        "uid": "mxl-st2110-gateway",
        "version": 1,
    }


def variable(name, label, query):
    return {"allValue": ".*", "current": {}, "datasource": DS, "definition": query, "hide": 0, "includeAll": True, "label": label, "multi": True,
            "name": name, "options": [], "query": {"qryType": 1, "query": query, "refId": "PrometheusVariableQueryEditor-VariableQuery"},
            "refresh": 2, "regex": "", "sort": 1, "type": "query"}


def render():
    return json.dumps(build(), indent=2, ensure_ascii=False) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--check", action="store_true", help="fail if the committed dashboard differs from a fresh generation")
    parser.add_argument("--output", type=Path, default=OUTPUT)
    args = parser.parse_args()
    text = render()
    if args.check:
        current = args.output.read_text(encoding="utf-8") if args.output.exists() else ""
        if current != text:
            sys.stdout.writelines(difflib.unified_diff(current.splitlines(True), text.splitlines(True), str(args.output), "generated", n=2))
            print(f"\n{args.output} is out of date: run python3 monitoring/tools/gen_dashboard.py", file=sys.stderr)
            return 1
        print(f"{args.output} is up to date")
        return 0
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(text, encoding="utf-8")
    print(f"wrote {args.output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
