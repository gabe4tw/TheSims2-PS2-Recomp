"""A tiny web server for tests/launcher_install.cpp: serves one folder on 127.0.0.1:<port> and appends every request's path to
<folder>/requests.log (so a test can see that no request was made).
   python -I tests/launcher_net_server.py <folder> <port>"""
import http.server
import os
import sys

folder, port = sys.argv[1], int(sys.argv[2])


class Handler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=folder, **kwargs)

    def do_GET(self):
        with open(os.path.join(folder, 'requests.log'), 'a') as log:
            log.write(self.path + '\n')
        super().do_GET()

    def log_message(self, *args):
        pass


http.server.ThreadingHTTPServer(('127.0.0.1', port), Handler).serve_forever()
