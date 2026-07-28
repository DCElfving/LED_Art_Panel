import http.server
import socketserver
import json
import os
import re
import shutil
from urllib.parse import urlparse, parse_qs

# Configuration
PORT = 8000
MOCK_DIR = "mock_uploads"
DATA_DIR = "data"

# Ensure mock directories exist
if not os.path.exists(MOCK_DIR):
    os.makedirs(MOCK_DIR)

# Mock Settings
SETTINGS = {
    "brightness": 50,
    "interval": 5,
    "transitionMode": 1,
    "ssid": "Mock_WiFi_Network",
    "pass": "secret"
}

# Mock WLED State
WLED_STATE = {
    "wledActive": False,
    "wledMode": 1,
    "wledSpeed": 128,
    "wledColor": 16755200, # 0xFFAA00
    "wledBrightness": 255
}

class ESP32MockHandler(http.server.SimpleHTTPRequestHandler):
    def do_GET(self):
        parsed = urlparse(self.path)
        path = parsed.path
        
        # Serve the main interface
        if path == '/' or path == '/index.html':
            self.send_response(200)
            self.send_header('Content-type', 'text/html')
            self.send_header('Cache-Control', 'no-store, no-cache, must-revalidate')
            self.end_headers()
            try:
                with open(os.path.join(DATA_DIR, 'index.html'), 'rb') as f:
                    self.wfile.write(f.read())
            except FileNotFoundError:
                self.wfile.write(b"Error: data/index.html not found")
            return
            
        # Mock /list endpoint
        if path == '/list':
            self.send_response(200)
            self.send_header('Content-type', 'application/json')
            self.end_headers()
            
            files = []
            used_space = 0
            if os.path.exists(MOCK_DIR):
                for f in os.listdir(MOCK_DIR):
                    if not f.startswith('.'):
                        fp = os.path.join(MOCK_DIR, f)
                        sz = os.path.getsize(fp)
                        files.append({'name': f, 'size': sz})
                        used_space += sz
            
            resp = {
                "used": used_space,
                "total": 4194304,
                "apIP": "localhost",
                "localIP": "192.168.1.50",
                "mdns": "led-panela1b2.local",
                "ssid": SETTINGS["ssid"],
                "pass_set": True,
                "files": files,
                "brightness": SETTINGS["brightness"],
                "interval": SETTINGS["interval"],
                "deviceMode": 1 if WLED_STATE["wledActive"] else 0,
                "transitionMode": SETTINGS["transitionMode"],
                "wledActive": WLED_STATE["wledActive"],
                "wledMode": WLED_STATE["wledMode"],
                "wledSpeed": WLED_STATE["wledSpeed"],
                "wledColor": WLED_STATE["wledColor"],
                "wledBrightness": WLED_STATE["wledBrightness"]
            }
            self.wfile.write(json.dumps(resp).encode())
            return

        # Mock /files endpoint
        if path == '/files':
            self.send_response(200)
            self.send_header('Content-type', 'application/json')
            self.end_headers()
            
            files = []
            if os.path.exists(MOCK_DIR):
                for f in os.listdir(MOCK_DIR):
                    if not f.startswith('.'):
                        fp = os.path.join(MOCK_DIR, f)
                        sz = os.path.getsize(fp)
                        files.append({'name': f, 'size': sz})
            
            self.wfile.write(json.dumps(files).encode())
            return

        # Serve uploaded files (Thumbnails/Binaries)
        filename = path.lstrip('/')
        file_path = os.path.join(MOCK_DIR, filename)
        
        if os.path.exists(file_path) and os.path.isfile(file_path):
             self.send_response(200)
             self.send_header('Content-type', 'application/octet-stream')
             self.end_headers()
             with open(file_path, 'rb') as f:
                 self.wfile.write(f.read())
             return
        
        self.send_error(404, "File not found")

    def do_DELETE(self):
        parsed = urlparse(self.path)
        params = parse_qs(parsed.query)
        name = params.get('name', [None])[0]
        
        if name:
            p = os.path.join(MOCK_DIR, name)
            if os.path.exists(p):
                os.remove(p)
                print(f"Deleted file: {name}")
                self.send_response(200)
                self.end_headers()
                self.wfile.write(b"Deleted")
                return
            else:
                self.send_error(404, "File not found to delete")
                return
        
        self.send_error(400, "Bad Request: Missing 'name' parameter")

    def do_POST(self):
        parsed = urlparse(self.path)
        
        if parsed.path == '/settings':
            length = int(self.headers.get('Content-Length', 0))
            if length > 0:
                body = self.rfile.read(length).decode('utf-8')
                params = parse_qs(body)
                if 'brightness' in params: SETTINGS['brightness'] = int(params['brightness'][0])
                if 'interval' in params: SETTINGS['interval'] = int(params['interval'][0])
                if 'transitionMode' in params: SETTINGS['transitionMode'] = int(params['transitionMode'][0])
                if 'ssid' in params: SETTINGS['ssid'] = params['ssid'][0]
                if 'pass' in params: SETTINGS['pass'] = params['pass'][0]
            
            self.send_response(200)
            self.end_headers()
            self.wfile.write(b"Settings Saved")
            print(f"Updated Settings: {SETTINGS}")
            return

        if parsed.path == '/wled':
            length = int(self.headers.get('Content-Length', 0))
            if length > 0:
                body = self.rfile.read(length).decode('utf-8')
                params = parse_qs(body)
                if 'active' in params: WLED_STATE['wledActive'] = (params['active'][0] == 'true')
                if 'mode' in params: WLED_STATE['wledMode'] = int(params['mode'][0])
                if 'speed' in params: WLED_STATE['wledSpeed'] = int(params['speed'][0])
                if 'brightness' in params: WLED_STATE['wledBrightness'] = int(params['brightness'][0])
                if 'color' in params: WLED_STATE['wledColor'] = int(params['color'][0])
                
            self.send_response(200)
            self.end_headers()
            self.wfile.write(b"WLED State Updated")
            print(f"Updated WLED: {WLED_STATE}")
            return

        if parsed.path == '/jump':
            length = int(self.headers.get('Content-Length', 0))
            if length > 0:
                body = self.rfile.read(length).decode('utf-8')
                params = parse_qs(body)
                name = params.get('name', [''])[0]
                print(f"Jump to image: {name}")
            self.send_response(200)
            self.end_headers()
            self.wfile.write(b"ok")
            return

        if parsed.path == '/upload':
            content_type = self.headers.get('Content-Type', '')
            if 'multipart/form-data' in content_type:
                try:
                    length = int(self.headers.get('Content-Length', 0))
                    body = self.rfile.read(length)
                    boundary = content_type.split("boundary=")[1].encode()
                    parts = body.split(b'--' + boundary)
                    
                    files_saved = 0
                    
                    for part in parts:
                        if b'filename="' not in part: continue
                        header_end = part.find(b'\r\n\r\n')
                        if header_end == -1: continue
                        headers = part[:header_end].decode('utf-8', errors='ignore')
                        content = part[header_end+4:]
                        if content.endswith(b'\r\n'): content = content[:-2]
                        
                        match = re.search(r'filename="([^"]+)"', headers)
                        if match:
                            filename = match.group(1)
                            save_path = os.path.join(MOCK_DIR, filename)
                            with open(save_path, 'wb') as f: f.write(content)
                            print(f"Uploaded file: {filename}")
                            files_saved += 1
                            
                    if files_saved > 0:
                        self.send_response(200)
                        self.end_headers()
                        self.wfile.write(b"Upload successful")
                        return
                    else:
                        self.send_error(400, "No valid file found in request")
                        return

                except Exception as e:
                    print(f"Upload error: {e}")
                    self.send_error(500, f"Server Error: {str(e)}")
                    return

        self.send_error(400, "Bad Request")

if __name__ == "__main__":
    print("-" * 50)
    print(f"Starting ESP32 Mock Server on port {PORT}...")
    print(f"Serving UI from: {DATA_DIR}/index.html")
    print(f"Storage path: {MOCK_DIR}/")
    print("-" * 50)
    print(f"Open http://localhost:{PORT} in your browser to test.")
    print("-" * 50)
    
    socketserver.TCPServer.allow_reuse_address = True
    with socketserver.TCPServer(("", PORT), ESP32MockHandler) as httpd:
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            print("\nServer stopped.")
            httpd.server_close()
