# SPDX-License-Identifier: GPL-2.0-or-later
"""Aggregate completed libusb bulk-lane statistics without retaining samples."""

import lldb
import math


_callback_breakpoint = None
_transfers = 0
_blocks = 0
_samples = 0
_sum = [0.0, 0.0]
_sum_sq = [0.0, 0.0]
_cross = 0.0


def _unsigned(process, address, size):
    error = lldb.SBError()
    value = process.ReadUnsignedFromMemory(address, size, error)
    return value if error.Success() else None


def _report():
    if _samples == 0:
        return
    means = [_sum[lane] / _samples for lane in range(2)]
    variances = [max(_sum_sq[lane] / _samples - means[lane] ** 2, 0.0)
                 for lane in range(2)]
    denominator = math.sqrt(variances[0] * variances[1])
    correlation = (_cross / _samples - means[0] * means[1]) / denominator \
        if denominator else 0.0
    print("OPENRSP_BULK_STATS transfers={} blocks={} pairs={} "
          "lane0_mean={:.6f} lane0_rms={:.6f} lane1_mean={:.6f} "
          "lane1_rms={:.6f} correlation={:.9f}".format(
              _transfers, _blocks, _samples, means[0], math.sqrt(variances[0]),
              means[1], math.sqrt(variances[1]), correlation))


def completed_bulk(frame, breakpoint_location, _dictionary):
    del breakpoint_location
    global _transfers, _blocks, _samples, _cross
    transfer = frame.FindRegister("x0").GetValueAsUnsigned()
    process = frame.GetThread().GetProcess()
    status = _unsigned(process, transfer + 16, 4)
    actual_length = _unsigned(process, transfer + 24, 4)
    buffer_address = _unsigned(process, transfer + 48, 8)
    if status != 0 or actual_length is None or buffer_address is None or actual_length < 1024:
        return False
    error = lldb.SBError()
    block = process.ReadMemory(buffer_address, 1024, error)
    if not error.Success() or len(block) != 1024:
        return False
    payload = block[16:1024]
    for offset in range(0, len(payload), 2):
        lane0 = payload[offset] - 256 if payload[offset] >= 128 else payload[offset]
        lane1 = payload[offset + 1] - 256 if payload[offset + 1] >= 128 else payload[offset + 1]
        _sum[0] += lane0
        _sum[1] += lane1
        _sum_sq[0] += lane0 * lane0
        _sum_sq[1] += lane1 * lane1
        _cross += lane0 * lane1
        _samples += 1
    _transfers += 1
    _blocks += 1
    if _transfers in (1, 16, 64, 256):
        _report()
    if _transfers >= 256 and _callback_breakpoint is not None:
        _callback_breakpoint.SetEnabled(False)
    return False


def submitted_bulk(frame, breakpoint_location, _dictionary):
    del breakpoint_location
    global _callback_breakpoint
    if _callback_breakpoint is not None:
        return False
    transfer = frame.FindRegister("x0").GetValueAsUnsigned()
    process = frame.GetThread().GetProcess()
    transfer_type = _unsigned(process, transfer + 10, 1)
    callback = _unsigned(process, transfer + 32, 8)
    if transfer_type != 2 or callback in (None, 0):
        return False
    target = process.GetTarget()
    _callback_breakpoint = target.BreakpointCreateByAddress(callback)
    _callback_breakpoint.SetScriptCallbackFunction(
        "lldb_rspduo_bulk_stats.completed_bulk")
    print("OPENRSP_BULK_STATS armed completed-transfer callback")
    return False


def __lldb_init_module(debugger, _dictionary):
    target = debugger.GetSelectedTarget()
    submit = target.BreakpointCreateByName("libusb_submit_transfer")
    submit.SetScriptCallbackFunction("lldb_rspduo_bulk_stats.submitted_bulk")
    print("OPENRSP_BULK_STATS armed libusb submit")
