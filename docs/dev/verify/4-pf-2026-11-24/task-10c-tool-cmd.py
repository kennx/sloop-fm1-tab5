"""发一行或多行 console 命令并原样打印回显（到提示符为止）。用法: cmd.py PORT LINE [LINE...]"""
import sys, time, importlib.util
spec = importlib.util.spec_from_file_location("inj", "scripts/inject/inject.py")
inj = importlib.util.module_from_spec(spec); spec.loader.exec_module(inj)
import serial
port = sys.argv[1]
with serial.Serial(port, inj.BAUD, timeout=0.05) as ser:
    inj.read_response.__wrapped__ if False else None
    for line in sys.argv[2:]:
        ser.reset_input_buffer()
        ser.write((line + "\n").encode()); ser.flush()
        try:
            out = inj.read_response(ser, 20.0)
        except TimeoutError as e:
            out = f"<<TIMEOUT {e}>>\n"
        print(f">>> {line}\n{out}", end="")
