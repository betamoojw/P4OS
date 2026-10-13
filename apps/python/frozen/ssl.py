# P4OS - Python: ssl, the older name of tls (MicroPython's own keeps both).

from tls import *


def wrap_socket(sock, server_side=False, key=None, cert=None, cert_reqs=CERT_NONE, cadata=None,
                server_hostname=None, do_handshake=True):
    return SSLContext(PROTOCOL_TLS_SERVER if server_side else PROTOCOL_TLS_CLIENT).wrap_socket(
        sock, server_side=server_side, server_hostname=server_hostname)
