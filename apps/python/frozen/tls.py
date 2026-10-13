# P4OS - Python: tls, client side, as MicroPython's own module names it.
#
# The connection is the firmware's (socket, mod_net.c): TLS negotiated by
# the system and the server checked against its certificate store, always.
# There is no store of Python's own, so verify_mode is kept but not obeyed
# (CERT_NONE checks too) and there are no certificates to load.

PROTOCOL_TLS_CLIENT = 0
PROTOCOL_TLS_SERVER = 1
CERT_NONE = 0
CERT_OPTIONAL = 1
CERT_REQUIRED = 2


class SSLContext:
    def __init__(self, protocol=PROTOCOL_TLS_CLIENT):
        if protocol != PROTOCOL_TLS_CLIENT:
            raise OSError("P4OS: TLS for clients only")
        self.protocol = protocol
        self.verify_mode = CERT_REQUIRED

    def wrap_socket(self, sock, server_side=False, do_handshake_on_connect=True, server_hostname=None):
        if server_side:
            raise OSError("P4OS: TLS for clients only")
        return sock._tls(server_hostname)

    def load_verify_locations(self, cafile=None, cadata=None):
        raise OSError("P4OS: servers are checked against the system's certificates")

    def load_cert_chain(self, certfile, keyfile=None):
        raise OSError("P4OS: no client certificates")
