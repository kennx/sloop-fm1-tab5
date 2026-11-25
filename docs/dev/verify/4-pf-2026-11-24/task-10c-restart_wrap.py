"""Task 10c：在真实设备上演练 inject 场景的「截屏传输失败 → 复位 → 从头重跑」路径（终审 C-I1）。
不改 inject.py：包一层 Session.capture——
  pos：工作工程快照（proj_snapshot_open）已拍下之后的第一次截屏，人为让传输失败
       （SCR 行换成 SCRX ⇒ 读不到帧头 ⇒ TransferError）。期望：场景复位重跑，重跑的开括号回显
       「重跑：诊断槽 3 仍是第一轮的快照 …（不再 SAVE）」，场景正常收口、exit 0。
  neg：同一时刻先把改过的工作工程存进诊断槽 3（模拟快照被改写），再让传输失败。期望：重跑的开括号
       InjectError 终止（非零），不拿别的工程去「还原」。
用法: restart_wrap.py MODE PORT --scenario NAME   （其余参数与 inject.py 相同）"""
import importlib.util, sys
spec = importlib.util.spec_from_file_location("inj", "scripts/inject/inject.py")
inj = importlib.util.module_from_spec(spec); spec.loader.exec_module(inj)
MODE = sys.argv.pop(1)
PORT = sys.argv.pop(1)
sys.argv[1:1] = ["--port", PORT]
state = {"fired": False}
orig_capture = inj.Session.capture


def capture(self, label, timeout=30.0):
    if not state["fired"] and any(k in self.snapshots for k in ("step_edit.erase", "params.tools", "transport.pattern")):
        state["fired"] = True
        if MODE == "neg":
            bpm = inj.store_bpm(self)
            self.cmd(f"PARAM GLOBAL 3 {bpm + 7}")
            self.cmd("STORE PROJ SAVE 3")
            self.ser.note(f"[10c 重启演练/neg] 把改过的工作工程（BPM {bpm} → {bpm + 7}）存进诊断槽 3：{self.last.strip().splitlines()[-2:]}")
        self.ser.note(f"[10c 重启演练/{MODE}] 人为让截屏 {label} 传输失败（SCR 行替换为 SCRX，3s 无帧头）")
        w = self.ser.write
        self.ser.write = lambda d: w(b"SCRX\n" if d == b"SCR\n" else d)
        return orig_capture(self, label, timeout=3.0)
    return orig_capture(self, label, timeout)


inj.Session.capture = capture
inj.main()
