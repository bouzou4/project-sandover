"""The calibration page's nits must survive GOAL's 32-bit float/int conversions both ways.

GOAL's ``(the int f)``, ``(the float i)`` and integer ``*`` are 32-bit, so a peak in millionths
overflows above 2147 nits (#358 review; confirmed in game: 5600 sent as millionths read back -2147,
and as ``(* 1000 (the int (* 1000.0 v)))`` read back 1305). These tests evaluate the exact forms the
game uses with that arithmetic emulated and check that a 5600-nit peak round-trips through
pc-set/pc-get.
"""
import re
from pathlib import Path

import numpy as np
import pytest

REPO = Path(__file__).resolve().parents[3]
PCKERNEL = REPO / "goal_src" / "jak1" / "pc" / "pckernel.gc"
PROGRESS = REPO / "goal_src" / "jak1" / "pc" / "progress-pc.gc"
KMACHINE = REPO / "game" / "kernel" / "common" / "kmachine.cpp"


def goal_the_int(f: float) -> int:
    """cvttss2si to 32 bits: out of range gives the integer indefinite value."""
    f32 = float(np.float32(f))
    return int(f32) if -2**31 <= f32 < 2**31 else -2**31


def goal_the_float(i: int) -> float:
    """cvtsi2ss from the low 32 bits."""
    return float(np.float32(np.int64(i).astype(np.int32)))


def eval_goal(expr: str, v: float):
    """Evaluate a GOAL arithmetic form of `*`, `the int`, `the float` and one `(-> ...)` (= v)."""
    toks = re.findall(r"\(|\)|[^\s()]+", expr)

    def parse(i):
        t = toks[i]
        if t != "(":
            return (t if t in ("int", "float") else float(t) if "." in t else int(t)), i + 1
        op, i = toks[i + 1], i + 2
        if op == "->":
            depth = 1
            while depth:
                depth += {"(": 1, ")": -1}.get(toks[i], 0)
                i += 1
            return v, i
        args = []
        while toks[i] != ")":
            a, i = parse(i)
            args.append(a)
        i += 1
        if op == "the":
            return (goal_the_int(args[1]) if args[0] == "int" else goal_the_float(args[1])), i
        if op == "*":
            out = args[0]
            for a in args[1:]:
                out = out * a
                out = float(np.float32(out)) if isinstance(out, float) else int(np.int64(out).astype(np.int32))
            return out, i
        raise ValueError(op)

    return parse(0)[0]


def peak_send_form() -> tuple[int, str]:
    m = re.search(r"\(sandover-changed\? hdr-peak\) \(pc-set-hdr-output (\d+) (.+)\)\)\n",
                  PCKERNEL.read_text(encoding="utf-8"))
    assert m, "peak push not found"
    return int(m.group(1)), m.group(2)


def kernel_set_scale(option: int) -> float:
    """What pc_set_hdr_output divides this peak option's value by."""
    src = KMACHINE.read_text(encoding="utf-8").split("void pc_set_hdr_output(", 1)[1].split("\n}\n", 1)[0]
    name = {3: "PEAK", 9: "PEAK_MILLI"}[option]
    body = src.split(f"case HdrOutputOption::{name}:", 1)[1].split("break;", 1)[0]
    return 1000.0 if "value / 1000.f" in body else 1000000.0


def kernel_scale(case: int) -> float:
    block = KMACHINE.read_text(encoding="utf-8").split("s64 pc_get_hdr_output(u64 which)", 1)[1]
    m = re.search(rf"case {case}:\n\s+return std::llround\([^;]+\* ([0-9.]+)\);", block.split("\n}\n", 1)[0])
    assert m, case
    return float(m.group(1))


def read_scale(which: int) -> float:
    m = re.search(rf"\(\* ([0-9.]+) \(the float \(pc-get-hdr-output {which}\)\)\)", PROGRESS.read_text(encoding="utf-8"))
    assert m, which
    return float(m.group(1))


def test_emulation_reproduces_what_the_game_did():
    assert eval_goal("(the int (* 1000000.0 (-> want hdr-peak)))", 5600.0) / 1000000.0 == pytest.approx(-2147.483648)
    assert eval_goal("(* 1000 (the int (* 1000.0 (-> want hdr-peak))))", 5600.0) / 1000000.0 == pytest.approx(1305.032704)


@pytest.mark.parametrize("nits", [400.0, 1037.0, 2148.0, 5600.0, 10000.0, -1.0])
def test_peak_sent_exactly(nits):
    option, form = peak_send_form()
    assert eval_goal(form, nits) / kernel_set_scale(option) == nits


@pytest.mark.parametrize("nits", [400.0, 1037.0, 2148.0, 5600.0, 10000.0])
def test_peak_round_trips_through_pc_get(nits):
    option, form = peak_send_form()
    engine = float(np.float32(eval_goal(form, nits) / kernel_set_scale(option)))  # hdr_peak_nits
    for case in (1, 3, 5):
        got = round(engine * kernel_scale(case))
        assert goal_the_float(got) * read_scale(1) == pytest.approx(nits, abs=1e-3), case


def test_black_detected_reads_thousandths():
    assert read_scale(2) == read_scale(1) == 1.0 / kernel_scale(2)


def paper_send_form() -> tuple[int, str]:
    m = re.search(r"\(sandover-changed\? paper-white\) \(pc-set-hdr-output (\d+) (.+)\)\)\n",
                  PCKERNEL.read_text(encoding="utf-8"))
    assert m, "paper white push not found"
    return int(m.group(1)), m.group(2)


@pytest.mark.parametrize("nits", [80.0, 200.0, 213.4375, 399.5, 400.0])
def test_paper_white_sent_in_nits_without_overflow(nits):
    option, form = paper_send_form()
    src = KMACHINE.read_text(encoding="utf-8").split("void pc_set_hdr_output(", 1)[1].split("\n}\n", 1)[0]
    assert option == 2 and "s.paper_white_nits = v;" in src.split("case HdrOutputOption::PAPER_WHITE:", 1)[1]
    assert eval_goal(form, nits) / 1000000.0 == pytest.approx(nits, abs=1e-3)


@pytest.mark.parametrize("nits", [80.0, 240.0, 480.0])
def test_desktop_white_reads_back_in_nits(nits):
    assert read_scale(6) == 1.0 / kernel_scale(6)
    got = round(float(np.float32(nits)) * kernel_scale(6))
    assert goal_the_float(got) * read_scale(6) == pytest.approx(nits, abs=1e-3)
