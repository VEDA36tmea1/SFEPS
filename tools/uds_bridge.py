#!/usr/bin/env python3
import socket, os, sys, threading

path = sys.argv[1] if len(sys.argv) > 1 else '/tmp/rc522_events.sock'
try:
    os.unlink(path)
except OSError:
    pass

lsock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
lsock.bind(path)
os.chmod(path, 0o666)
lsock.listen(5)

def handle_pair(a, b):
    def pump(src, dst):
        try:
            while True:
                data = src.recv(4096)
                if not data:
                    break
                dst.sendall(data)
        except Exception:
            pass
        finally:
            try: src.close()
            except: pass
            try: dst.close()
            except: pass

    t1 = threading.Thread(target=pump, args=(a,b), daemon=True)
    t2 = threading.Thread(target=pump, args=(b,a), daemon=True)
    t1.start(); t2.start()

print("UDS bridge listening on", path)
while True:
    c1, _ = lsock.accept()
    c2, _ = lsock.accept()
    handle_pair(c1, c2)
