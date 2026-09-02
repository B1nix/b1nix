#!/usr/bin/env python3
"""
pxe_server.py — Zero-dependency TFTP Server & Live UART Monitor for Raspberry Pi 4
"""

import os
import sys
import time
import glob
import struct
import socket
import select
import termios
import tty
import threading

DEFAULT_TFTP_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), "../../build/aarch64/sdcard"))
TFTP_PORT = 69

def get_local_ip():
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        ip = s.getsockname()[0]
        s.close()
        return ip
    except Exception:
        return "127.0.0.1"

class TFTPServer:
    OP_RRQ = 1
    OP_WRQ = 2
    OP_DATA = 3
    OP_ACK = 4
    OP_ERROR = 5
    OP_OACK = 6

    def __init__(self, root_dir, host="0.0.0.0", port=69):
        self.root_dir = root_dir
        self.host = host
        self.port = port
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind((self.host, self.port))
        self.running = True

    def handle_client(self, data, client_addr):
        if len(data) < 2:
            return
        opcode = struct.unpack("!H", data[:2])[0]
        if opcode != self.OP_RRQ:
            return

        parts = data[2:].split(b"\x00")
        if len(parts) < 2:
            return
        raw_filename = parts[0].decode("utf-8", errors="ignore").lstrip("/")

        options = {}
        idx = 2
        while idx < len(parts) - 1:
            opt_name = parts[idx].decode("utf-8", errors="ignore").lower()
            opt_val = parts[idx+1].decode("utf-8", errors="ignore")
            options[opt_name] = opt_val
            idx += 2

        filename = os.path.basename(raw_filename)
        rel_path = raw_filename
        if "/" in rel_path:
            p = rel_path.split("/")
            if len(p) >= 2 and p[-2] == "overlays":
                rel_path = os.path.join("overlays", p[-1])
            else:
                rel_path = p[-1]
        
        file_path = os.path.join(self.root_dir, rel_path)
        if not os.path.isfile(file_path):
            file_path = os.path.join(self.root_dir, filename)

        if not os.path.isfile(file_path):
            err_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            err_pkt = struct.pack("!HH", self.OP_ERROR, 1) + b"File not found\x00"
            err_sock.sendto(err_pkt, client_addr)
            err_sock.close()
            return

        t = threading.Thread(target=self._transfer_file, args=(file_path, client_addr, options), daemon=True)
        t.start()

    def _transfer_file(self, file_path, client_addr, options):
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.settimeout(2.0)
        
        blksize = 512
        if "blksize" in options:
            try:
                blksize = min(int(options["blksize"]), 1468)
            except ValueError:
                pass

        file_size = os.path.getsize(file_path)
        fname = os.path.basename(file_path)
        print(f"[TFTP] Serving {fname} ({file_size} bytes, blksize={blksize}) to {client_addr[0]}:{client_addr[1]}")

        block_num = 0
        if options:
            oack_data = struct.pack("!H", self.OP_OACK)
            if "blksize" in options:
                oack_data += b"blksize\x00" + str(blksize).encode() + b"\x00"
            if "tsize" in options:
                oack_data += b"tsize\x00" + str(file_size).encode() + b"\x00"
            
            sock.sendto(oack_data, client_addr)
            try:
                ack, _ = sock.recvfrom(512)
                if len(ack) >= 4 and struct.unpack("!H", ack[:2])[0] == self.OP_ACK:
                    block_num = struct.unpack("!H", ack[2:4])[0]
            except socket.timeout:
                pass

        with open(file_path, "rb") as f:
            cur_block = 1
            while True:
                chunk = f.read(blksize)
                data_pkt = struct.pack("!HH", self.OP_DATA, cur_block) + chunk
                
                for retry in range(3):
                    sock.sendto(data_pkt, client_addr)
                    try:
                        ack, _ = sock.recvfrom(512)
                        if len(ack) >= 4:
                            ack_op, ack_block = struct.unpack("!HH", ack[:4])
                            if ack_op == self.OP_ACK and ack_block == cur_block:
                                break
                    except socket.timeout:
                        continue
                cur_block = (cur_block + 1) & 0xFFFF
                if len(chunk) < blksize:
                    break
        sock.close()

    def start(self):
        print(f"[TFTP] Server listening on {self.host}:{self.port} (Serving: {self.root_dir})")
        while self.running:
            try:
                data, addr = self.sock.recvfrom(2048)
                self.handle_client(data, addr)
            except Exception as e:
                if self.running:
                    time.sleep(0.1)

def run_uart_listener():
    print("[UART] Watching for UART bridge on /dev/cu.usbmodem*...")
    sys.stdout.flush()
    while True:
        ports = sorted(glob.glob("/dev/cu.usbmodem*") + glob.glob("/dev/cu.usbserial*"))
        if ports:
            port = ports[0]
            try:
                fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
                attrs = termios.tcgetattr(fd)
                tty.setraw(fd)
                attrs[4] = termios.B115200
                attrs[5] = termios.B115200
                termios.tcsetattr(fd, termios.TCSANOW, attrs)
                print(f"\n[UART] Connected to {port} @ 115200. Live terminal:\n" + "="*60)
                sys.stdout.flush()
                while True:
                    r, _, _ = select.select([fd], [], [], 0.1)
                    if r:
                        data = os.read(fd, 1024)
                        if data:
                            sys.stdout.write(data.decode("utf-8", errors="replace"))
                            sys.stdout.flush()
            except Exception as e:
                time.sleep(0.5)
        time.sleep(0.5)

def main():
    tftp_dir = DEFAULT_TFTP_DIR
    if len(sys.argv) > 1:
        tftp_dir = os.path.abspath(sys.argv[1])
    
    local_ip = get_local_ip()
    print("=" * 64)
    print(" b1nix Raspberry Pi 4 PXE / TFTP Server & UART Monitor")
    print(f" Local IP:   {local_ip}")
    print(f" TFTP Root:  {tftp_dir}")
    print("=" * 64)

    uart_t = threading.Thread(target=run_uart_listener, daemon=True)
    uart_t.start()

    try:
        tftp = TFTPServer(tftp_dir, host="0.0.0.0", port=TFTP_PORT)
        tftp.start()
    except PermissionError:
        print(f"[-] Binding to UDP port 69 requires sudo.")
        print(f"[!] Please run: sudo python3 tools/rpi4/pxe_server.py")
        sys.exit(1)

if __name__ == "__main__":
    main()
