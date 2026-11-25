import sys, time, serial
sys.path.insert(0, "/home/kenn/projects/sloop-fm1-tab5/scripts"); sys.path.insert(0, "/home/kenn/projects/sloop-fm1-tab5/scripts/inject")
import screenshot as sc, inject
from PIL import Image
ACC = (224, 176, 40)
def cols(img):
    y0 = inject.STEP_GRID_Y0; y1 = y0 + inject.STEP_ROW_PITCH - 4
    out = []
    for c in range(16):
        x0 = inject.STEP_GRID_X0 + c * inject.STEP_CELL_PITCH
        n = sum(1 for x in range(x0, x0 + inject.STEP_CELL_PITCH, 2) for y in range(y0, y1, 4)
                if max(abs(img.getpixel((x, y))[i] - ACC[i]) for i in range(3)) <= 12)
        if n > 30: out.append(c)
    return out
ser = serial.Serial("/dev/ttyACM0", 115200, timeout=0.3)
def cmd(c, t=1.0):
    ser.reset_input_buffer(); ser.write((c + "\n").encode()); ser.flush()
    b = b""; t0 = time.time()
    while time.time() - t0 < t:
        ch = ser.read(4096)
        if ch:
            b += ch
            if b.rstrip().endswith(b"sloop>"): break
    return b.decode("utf-8", "replace")
cmd("UI transport"); cmd("TAP %d %d" % inject.TRANSPORT_PLAY)
t_play = time.time()
for i in range(6):
    ser.reset_input_buffer(); t_s = time.time(); ser.write(b"SCR\n"); ser.flush()
    w, h, fmt, data = sc.read_frame(ser, 60.0)
    img = sc.rgb565_to_rgb(data, w, h) if fmt == 'rgb565' else sc.rgb888_to_rgb(data, w, h)
    print(f"capture {i}: SCR sent at +{t_s-t_play:.2f}s, done +{time.time()-t_play:.2f}s playhead cols={cols(img)}")
    time.sleep(0.25)
cmd("TAP %d %d" % inject.TRANSPORT_STOP); cmd("UI play")
ser.close()
