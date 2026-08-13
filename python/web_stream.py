import time
from http.server import BaseHTTPRequestHandler, HTTPServer
from socketserver import ThreadingMixIn

STREAM_FILE = "/dev/shm/stream.jpg"

class CamHandler(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path.endswith('stream.mjpg'):
            self.send_response(200)
            self.send_header('Content-type', 'multipart/x-mixed-replace; boundary=--jpgboundary')
            self.end_headers()
            while True:
                try:
                    with open(STREAM_FILE, 'rb') as f:
                        img = f.read()

                    self.wfile.write(b"--jpgboundary\r\n")
                    self.send_header('Content-type', 'image/jpeg')
                    self.send_header('Content-length', str(len(img)))
                    self.end_headers()
                    self.wfile.write(img)
                    self.wfile.write(b"\r\n")

                    time.sleep(0.04) 
                except FileNotFoundError:
                    time.sleep(0.1)
                except BrokenPipeError:
                    break
                except Exception as e:
                    print(f"Ошибка трансляции: {e}")
                    break
                    
        elif self.path == '/':
            self.send_response(200)
            self.send_header('Content-type', 'text/html; charset=utf-8')
            self.end_headers()
            html = """
            <html>
                <head>
                    <title>Blind Nav - Камера</title>
                    <style>
                        body { background-color: #222; color: white; text-align: center; font-family: sans-serif; }
                        img { max-width: 100%; border: 3px solid #4CAF50; border-radius: 8px; margin-top: 20px; }
                    </style>
                </head>
                <body>
                    <h2>Трансляция с камеры (YOLOv11)</h2>
                    <img src="stream.mjpg" />
                </body>
            </html>
            """
            self.wfile.write(html.encode('utf-8'))
        else:
            self.send_response(404)
            self.end_headers()

class ThreadedHTTPServer(ThreadingMixIn, HTTPServer):
    allow_reuse_address = True
    pass

if __name__ == '__main__':
    port = 8080
    try:
        server = ThreadedHTTPServer(('0.0.0.0', port), CamHandler)
        print(f"=== Веб-сервер запущен ===")
        print(f"Откройте браузер на вашем ПК и перейдите по адресу:")
        print(f"http://newloggervalya.local:8080")
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nОстановка сервера...")
        server.socket.close()
