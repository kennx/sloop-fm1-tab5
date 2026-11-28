#!/usr/bin/env python3
"""工程快照安全路径的离线回归；不需要设备。"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import inject  # noqa: E402


class FakeSession:
    def __init__(self):
        self.snapshots = {"case": (0x1234, True)}
        self.events = []

    def ok(self, text):
        self.events.append(("ok", text))

    def fail(self, text):
        self.events.append(("fail", text))


def check_close_mismatch() -> None:
    s = FakeSession()
    calls = []
    names = ("proj_slot_state", "store_cmd", "live_hash", "proj_erase")
    saved = {name: getattr(inject, name) for name in names}
    try:
        inject.proj_slot_state = lambda _s, _slot: 0x1234
        inject.store_cmd = lambda *args, **kwargs: calls.append(args[1])
        inject.live_hash = lambda _s: 0x9999
        inject.proj_erase = lambda *args, **kwargs: calls.append("erase")
        try:
            inject.proj_snapshot_close(s, "case", 3, "TEST")
        except inject.InjectError as e:
            message = str(e)
        else:
            raise AssertionError("hash mismatch did not raise InjectError")
        assert "不会擦除" in message and "STORE PROJ LOAD 3" in message
        assert calls == ["STORE PROJ LOAD 3"]
        assert s.snapshots["case"] == (0x1234, True)
    finally:
        for name, value in saved.items():
            setattr(inject, name, value)
    print("[PASS] LOAD hash mismatch raises, preserves D, names recovery command")


def check_exception_bracket() -> None:
    saved_body = inject._run_sequence_body
    saved_cleanup = inject._sequence_cleanup_on_error
    try:
        cleanups = []
        inject._sequence_cleanup_on_error = lambda _s: cleanups.append("cleanup")
        inject._run_sequence_body = lambda _s: (_ for _ in ()).throw(inject.ScenarioRestart("retry"))
        try:
            inject.run_sequence(object())
        except inject.ScenarioRestart:
            pass
        else:
            raise AssertionError("ScenarioRestart swallowed")
        assert cleanups == []
        print("[PASS] ScenarioRestart skips exception cleanup")

        inject._run_sequence_body = lambda _s: (_ for _ in ()).throw(inject.InjectError("original timeout"))
        try:
            inject.run_sequence(object())
        except inject.InjectError as e:
            assert str(e) == "original timeout"
        else:
            raise AssertionError("original exception swallowed")
        assert cleanups == ["cleanup"]
        print("[PASS] exception cleanup runs and original InjectError re-raises")
    finally:
        inject._run_sequence_body = saved_body
        inject._sequence_cleanup_on_error = saved_cleanup


def main() -> None:
    check_close_mismatch()
    check_exception_bracket()
    print("snapshot selftest OK")


if __name__ == "__main__":
    main()
