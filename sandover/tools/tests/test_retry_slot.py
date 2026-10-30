"""The automatic retry stays on a slot the run was given and waits for it to clear."""


import pytest

from sandover.rig import capture as C


class FakeSlot:
    index = 2

    def __init__(self, busy_calls: int):
        self.calls = busy_calls

    def busy_ports(self):
        self.calls -= 1
        return [1] if self.calls >= 0 else []


@pytest.fixture(autouse=True)
def no_orphans(monkeypatch):
    monkeypatch.setattr(C.locks, "reclaim_orphans", lambda s: [])


def test_retry_slot_is_a_crashed_one():
    assert C.retry_slot({2}) == 2
    assert C.retry_slot({3, 1}) == 1


def test_waits_while_busy_then_proceeds():
    s = FakeSlot(3)
    C.await_clear(s, 5, poll=0.01)
    assert s.calls < 0


def test_times_out_when_still_busy():
    with pytest.raises(TimeoutError):
        C.await_clear(FakeSlot(10**6), 0.05, poll=0.01)
