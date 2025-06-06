
# rpc.py - RFC1057/RFC1831
#
# Copyright 2001 Cendio AB
# 
# Copyright (c) 2001 Python Software Foundation.
# All rights reserved.
# 
# Copyright (c) 2000 BeOpen.com.
# All rights reserved.
# 
# Copyright (c) 1995-2001 Corporation for National Research Initiatives.
# All rights reserved.
# 
# Copyright (c) 1991-1995 Stichting Mathematisch Centrum.
# All rights reserved.
# 
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; version 2 of the License. 
# 
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
# 
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.

# XXX The UDP version of the protocol resends requests when it does
# XXX not receive a timely reply -- use only for idempotent calls!

# XXX There is no provision for call timeout on TCP connections

__pychecker__ = 'no-callinit'

import xdrlib
import socket
import os
import time

RPCVERSION = 2

CALL = 0
REPLY = 1

AUTH_NULL = 0
AUTH_UNIX = 1
AUTH_SHORT = 2
AUTH_DES = 3

MSG_ACCEPTED = 0
MSG_DENIED = 1

SUCCESS = 0				# RPC executed successfully
PROG_UNAVAIL  = 1			# remote hasn't exported program
PROG_MISMATCH = 2			# remote can't support version #
PROC_UNAVAIL  = 3			# program can't support procedure
GARBAGE_ARGS  = 4			# procedure can't decode params

RPC_MISMATCH = 0			# RPC version number != 2
AUTH_ERROR = 1				# remote can't authenticate caller

AUTH_BADCRED      = 1			# bad credentials (seal broken)
AUTH_REJECTEDCRED = 2			# client must begin new session
AUTH_BADVERF      = 3			# bad verifier (seal broken)
AUTH_REJECTEDVERF = 4			# verifier expired or replayed
AUTH_TOOWEAK      = 5			# rejected for security reasons

# All RPC errors are subclasses of RPCException
class RPCException(Exception):
    def __str__(self):
        return "RPCException"

class BadRPCMsgType(RPCException):
    def __init__(self, msg_type):
        self.msg_type = msg_type

    def __str__(self):
        return "BadRPCMsgType %d" % self.msg_type

class BadRPCVersion(RPCException):
    def __init__(self, version):
        self.version = version

    def __str__(self):
        return "BadRPCVersion %d" % self.version

class RPCMsgDenied(RPCException):
    # MSG_DENIED
    def __init__(self, stat):
        self.stat = stat

    def __str__(self):
        return "RPCMsgDenied %d" % self.stat

class RPCMisMatch(RPCException):
    # MSG_DENIED: RPC_MISMATCH
    def __init__(self, low, high):
        self.low = low
        self.high = high

    def __str__(self):
        return "RPCMisMatch %d,%d" % (self.low, self.high)

class RPCAuthError(RPCException):
    # MSG_DENIED: AUTH_ERROR
    def __init__(self, stat):
        self.stat = stat

    def __str__(self):
        return "RPCAuthError %d" % self.stat

class BadRPCReplyType(RPCException):
    # Neither MSG_DENIED nor MSG_ACCEPTED
    def __init__(self, msg_type):
        self.msg_type = msg_type

    def __str__(self):
        return "BadRPCReplyType %d" % self.msg_type

class RPCProgUnavail(RPCException):
    # PROG_UNAVAIL
    def __str__(self):
        return "RPCProgUnavail"

class RPCProgMismatch(RPCException):
    # PROG_MISMATCH
    def __init__(self, low, high):
        self.low = low
        self.high = high

    def __str__(self):
        return "RPCProgMismatch %d,%d" % (self.low, self.high)

class RPCProcUnavail(RPCException):
    # PROC_UNAVAIL
    def __str__(self):
        return "RPCProcUnavail"

class RPCGarbageArgs(RPCException):
    # GARBAGE_ARGS
    def __str__(self):
        return "RPCGarbageArgs"

class RPCUnextractedData(RPCException):
    # xdrlib raised exception because unextracted data remained
    def __str__(self):
        return "RPCUnextractedData"

class RPCBadAcceptStats(RPCException):
    # Unknown accept_stat
    def __init__(self, stat):
        self.stat = stat

    def __str__(self):
        return "RPCBadAcceptStats %d" % self.stat

class XidMismatch(RPCException):
    # Got wrong XID in reply, got "xid" instead of "expected"
    def __init__(self, xid, expected):
        self.xid = xid
        self.expected = expected

    def __str__(self):
        return "XidMismatch %d,%d" % (self.xid, self.expected)

class TimeoutError(RPCException):
    pass

class PortMapError(RPCException):
    pass


class Packer(xdrlib.Packer):

    def pack_auth(self, auth):
        flavor, stuff = auth
        self.pack_enum(flavor)
        self.pack_opaque(stuff)

    def pack_auth_unix(self, stamp, machinename, uid, gid, gids):
        self.pack_uint(stamp)
        self.pack_string(machinename)
        self.pack_uint(uid)
        self.pack_uint(gid)
        self.pack_uint(len(gids))
        for i in gids:
            self.pack_uint(i)

    def pack_callheader(self, xid, prog, vers, proc, cred, verf):
        self.pack_uint(xid)
        self.pack_enum(CALL)
        self.pack_uint(RPCVERSION)
        self.pack_uint(prog)
        self.pack_uint(vers)
        self.pack_uint(proc)
        self.pack_auth(cred)
        self.pack_auth(verf)
        # Caller must add procedure-specific part of call

    def pack_replyheader(self, xid, verf):
        self.pack_uint(xid)
        self.pack_enum(REPLY)
        self.pack_uint(MSG_ACCEPTED)
        self.pack_auth(verf)
        self.pack_enum(SUCCESS)
        # Caller must add procedure-specific part of reply


class Unpacker(xdrlib.Unpacker):

    def unpack_auth(self):
        flavor = self.unpack_enum()
        stuff = self.unpack_opaque()
        if flavor == AUTH_UNIX:
                p = Unpacker(stuff)
                stuff = p.unpack_auth_unix()
        return (flavor, stuff)

    def unpack_auth_unix(self):
        stamp=self.unpack_uint()
        machinename=self.unpack_string()
        print("machinename: %s" % machinename)
        uid=self.unpack_uint()
        gid=self.unpack_uint()
        n_gids=self.unpack_uint()
        gids = []
        print("n_gids: %d" % n_gids)
        for i in range(n_gids):
            gids.append(self.unpack_uint())
        return stamp, machinename, uid, gid, gids


    def unpack_callheader(self):
        xid = self.unpack_uint()
        msg_type = self.unpack_enum()
        if msg_type != CALL:
            raise BadRPCMsgType(msg_type)
        rpcvers = self.unpack_uint()
        if rpcvers != RPCVERSION:
            raise BadRPCVersion(rpcvers)
        prog = self.unpack_uint()
        vers = self.unpack_uint()
        proc = self.unpack_uint()
        cred = self.unpack_auth()
        verf = self.unpack_auth()
        return xid, prog, vers, proc, cred, verf
        # Caller must add procedure-specific part of call

    def unpack_replyheader(self):
        xid = self.unpack_uint()
        msg_type = self.unpack_enum()
        if msg_type != REPLY:
            raise BadRPCMsgType(msg_type)
        stat = self.unpack_enum()
        if stat == MSG_DENIED:
            stat = self.unpack_enum()
            if stat == RPC_MISMATCH:
                low = self.unpack_uint()
                high = self.unpack_uint()
                raise RPCMisMatch(low, high)
            if stat == AUTH_ERROR:
                stat = self.unpack_uint()
                raise RPCAuthError(stat)
            raise RPCMsgDenied(stat)
        if stat != MSG_ACCEPTED:
            raise BadRPCReplyType(stat)
        verf = self.unpack_auth()
        stat = self.unpack_enum()
        if stat == PROG_UNAVAIL:
            raise RPCProgUnavail()
        if stat == PROG_MISMATCH:
            low = self.unpack_uint()
            high = self.unpack_uint()
            raise RPCProgMismatch(low, high)
        if stat == PROC_UNAVAIL:
            raise RPCProcUnavail()
        if stat == GARBAGE_ARGS:
            raise RPCGarbageArgs()
        if stat != SUCCESS:
            raise RPCBadAcceptStats(stat)
        return xid, verf
        # Caller must get procedure-specific part of reply


# Subroutines to create opaque authentication objects

def make_auth_null():
    return ''

def make_auth_unix(seed, host, uid, gid, groups):
    p = Packer()
    p.pack_auth_unix(seed, host, uid, gid, groups)
    return p.get_buffer()

def make_auth_unix_default():
    try:
        uid = os.getuid()
        gid = os.getgid()
    except AttributeError:
        uid = gid = 0
    return make_auth_unix(int(time.time()-unix_epoch()), \
              socket.gethostname(), uid, gid, [])

_unix_epoch = -1
def unix_epoch():
    """Very painful calculation of when the Unix Epoch is.

    This is defined as the return value of time.time() on Jan 1st,
    1970, 00:00:00 GMT.

    On a Unix system, this should always return 0.0.  On a Mac, the
    calculations are needed -- and hard because of integer overflow
    and other limitations.

    """
    global _unix_epoch
    if _unix_epoch >= 0: return _unix_epoch
    now = time.time()
    localt = time.localtime(now)	# (y, m, d, hh, mm, ss, ..., ..., ...)
    gmt = time.gmtime(now)
    offset = time.mktime(localt) - time.mktime(gmt)
    y, m, d, hh, mm, ss = 1970, 1, 1, 0, 0, 0
    offset, ss = divmod(ss + offset, 60)
    offset, mm = divmod(mm + offset, 60)
    offset, hh = divmod(hh + offset, 24)
    d = d + offset
    _unix_epoch = time.mktime((y, m, d, hh, mm, ss, 0, 0, 0))
    print("Unix epoch:", time.ctime(_unix_epoch))
    return _unix_epoch


# Common base class for clients

class Client:

    def __init__(self, host, prog, vers, port):
        self.host = host
        self.prog = prog
        self.vers = vers
        self.port = port
        self.sock = None
        self.makesocket() # Assigns to self.sock
        self.bindsocket()
        self.connsocket()
        # Servers may do XID caching, so try to come up with something
        # unique to start with. XIDs are 32 bits. Python integers are always
        # at least 32 bits. 
        self.lastxid = int(int(time.time() * 1E6) & 0xfffffff)
        self.addpackers()
        self.cred = None
        self.verf = None

    def close(self):
        self.sock.close()

    def makesocket(self):
        # This MUST be overridden
        raise RuntimeError("makesocket not defined")

    def connsocket(self):
        # Override this if you don't want/need a connection
        self.sock.connect((self.host, self.port))

    def bindsocket(self):
        # Override this to bind to a different port (e.g. reserved)
        self.sock.bind(('', 0))

    def addpackers(self):
        # Override this to use derived classes from Packer/Unpacker
        self.packer = Packer()
        self.unpacker = Unpacker('')

    def make_call(self, proc, args, pack_func, unpack_func):
        # Don't normally override this (but see Broadcast)
        if pack_func is None and args is not None:
            raise TypeError("non-null args with null pack_func")
        self.start_call(proc)
        if pack_func:
            pack_func(args)
        self.do_call()
        if unpack_func:
            result = unpack_func()
        else:
            result = None
        try:
            self.unpacker.done()
        except xdrlib.Error:
            raise RPCUnextractedData()
            
        return result

    def start_call(self, proc):
        # Don't override this
        self.lastxid = xid = self.lastxid + 1
        cred = self.mkcred()
        verf = self.mkverf()
        p = self.packer
        p.reset()
        p.pack_callheader(xid, self.prog, self.vers, proc, cred, verf)

    def do_call(self):
        # This MUST be overridden
        raise RuntimeError("do_call not defined")

    def mkcred(self):
        # Override this to use more powerful credentials
        if self.cred == None:
            self.cred = (AUTH_NULL, make_auth_null())
        return self.cred

    def mkverf(self):
        # Override this to use a more powerful verifier
        if self.verf == None:
            self.verf = (AUTH_NULL, make_auth_null())
        return self.verf

    def call_0(self):		# Procedure 0 is always like this
        return self.make_call(0, None, None, None)


# Record-Marking standard support

def sendfrag(sock, last, frag):
    x = len(frag)
    if last: x = x | 0x80000000
    header = (chr(int(x>>24 & 0xff)) + chr(int(x>>16 & 0xff)) + \
              chr(int(x>>8 & 0xff)) + chr(int(x & 0xff)))
    sock.send(header + frag)

def sendrecord(sock, record):
    sendfrag(sock, 1, record)

def recvfrag(sock):
    header = sock.recv(4)
    if len(header) < 4:
        raise EOFError
    x = int(ord(header[0]))<<24 | ord(header[1])<<16 | \
        ord(header[2])<<8 | ord(header[3])
    last = ((x & 0x80000000) != 0)
    n = int(x & 0x7fffffff)
    frag = ''
    while n > 0:
        buf = sock.recv(n)
        if not buf: raise EOFError
        n = n - len(buf)
        frag = frag + buf
    return last, frag

def recvrecord(sock):
    record = ''
    last = 0
    while not last:
        last, frag = recvfrag(sock)
        record = record + frag
    return record


# Try to bind to a reserved port (must be root)

last_resv_port_tried = None
def bindresvport(sock, host):
    global last_resv_port_tried
    FIRST, LAST = 600, 1024 # Range of ports to try
    if last_resv_port_tried == None:
        last_resv_port_tried = FIRST + os.getpid() % (LAST-FIRST)
    for i in list(range(last_resv_port_tried, LAST)) + \
              list(range(FIRST, last_resv_port_tried)):
        last_resv_port_tried = i
        try:
            sock.bind((host, i))
            return last_resv_port_tried
        except socket.error as xxx_todo_changeme:
            (errno, msg) = xxx_todo_changeme.args
            if errno != 114:
                raise socket.error(errno, msg)
    raise RuntimeError("can't assign reserved port")


# Client using TCP to a specific port

class RawTCPClient(Client):

    def makesocket(self):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)

    def do_call(self):
        call = self.packer.get_buffer()
        sendrecord(self.sock, call)
        reply = recvrecord(self.sock)
        u = self.unpacker
        u.reset(reply)
        xid, verf = u.unpack_replyheader()
        if xid != self.lastxid:
            # Can't really happen since this is TCP...
            raise XidMismatch(xid, self.lastxid)

# Client using UDP to a specific port

class RawUDPClient(Client):

    def makesocket(self):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    def do_call(self):
        call = self.packer.get_buffer()
        self.sock.send(call)
        try:
            from select import select
        except ImportError:
            print('Warning: Select not found, RPC may hang')
            select = None
        BUFSIZE = 8192 # Max UDP buffer size
        timeout = 1
        count = 5
        while 1:
            r, w, x = [self.sock], [], []
            if select:
                r, w, x = select(r, w, x, timeout)
            if self.sock not in r:
                count = count - 1
                if count < 0: raise TimeoutError() 
                if timeout < 25: timeout = timeout *2
##				print 'RESEND', timeout, count
                self.sock.send(call)
                continue
            reply = self.sock.recv(BUFSIZE)
            u = self.unpacker
            u.reset(reply)
            xid, verf = u.unpack_replyheader()
            if xid != self.lastxid:
##				print 'BAD xid'
                continue
            break


# Client using UDP broadcast to a specific port

class RawBroadcastUDPClient(RawUDPClient):

    def __init__(self, bcastaddr, prog, vers, port):
        RawUDPClient.__init__(self, bcastaddr, prog, vers, port)
        self.reply_handler = None
        self.timeout = 30

    def connsocket(self):
        # Don't connect -- use sendto
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)

    def set_reply_handler(self, reply_handler):
        self.reply_handler = reply_handler

    def set_timeout(self, timeout):
        self.timeout = timeout # Use None for infinite timeout

    def make_call(self, proc, args, pack_func, unpack_func):
        if pack_func is None and args is not None:
            raise TypeError("non-null args with null pack_func")
        self.start_call(proc)
        if pack_func:
            pack_func(args)
        call = self.packer.get_buffer()
        self.sock.sendto(call, (self.host, self.port))
        try:
            from select import select
        except ImportError:
            print('Warning: Select not found, broadcast will hang')
            select = None
        BUFSIZE = 8192 # Max UDP buffer size (for reply)
        replies = []
        if unpack_func is None:
            def dummy(): pass
            unpack_func = dummy
        while 1:
            r, w, x = [self.sock], [], []
            if select:
                if self.timeout is None:
                    r, w, x = select(r, w, x)
                else:
                    r, w, x = select(r, w, x, self.timeout)
            if self.sock not in r:
                break
            reply, fromaddr = self.sock.recvfrom(BUFSIZE)
            u = self.unpacker
            u.reset(reply)
            xid, verf = u.unpack_replyheader()
            if xid != self.lastxid:
##				print 'BAD xid'
                continue
            reply = unpack_func()
            try:
                self.unpacker.done()
            except xdrlib.Error:
                raise RPCUnextractedData()
            replies.append((reply, fromaddr))
            if self.reply_handler:
                self.reply_handler(reply, fromaddr)
        return replies


# Port mapper interface

# Program number, version and (fixed!) port number
PMAP_PROG = 100000
PMAP_VERS = 2
PMAP_PORT = 111

# Procedure numbers
PMAPPROC_NULL = 0			# (void) -> void
PMAPPROC_SET = 1			# (mapping) -> bool
PMAPPROC_UNSET = 2			# (mapping) -> bool
PMAPPROC_GETPORT = 3			# (mapping) -> unsigned int
PMAPPROC_DUMP = 4			# (void) -> pmaplist
PMAPPROC_CALLIT = 5			# (call_args) -> call_result

# A mapping is (prog, vers, prot, port) and prot is one of:

IPPROTO_TCP = 6
IPPROTO_UDP = 17

# A pmaplist is a variable-length list of mappings, as follows:
# either (1, mapping, pmaplist) or (0).

# A call_args is (prog, vers, proc, args) where args is opaque;
# a call_result is (port, res) where res is opaque.


class PortMapperPacker(Packer):

    def pack_mapping(self, mapping):
        prog, vers, prot, port = mapping
        self.pack_uint(prog)
        self.pack_uint(vers)
        self.pack_uint(prot)
        self.pack_uint(port)

    def pack_pmaplist(self, list):
        self.pack_list(list, self.pack_mapping)

    def pack_call_args(self, ca):
        prog, vers, proc, args = ca
        self.pack_uint(prog)
        self.pack_uint(vers)
        self.pack_uint(proc)
        self.pack_opaque(args)


class PortMapperUnpacker(Unpacker):

    def unpack_mapping(self):
        prog = self.unpack_uint()
        vers = self.unpack_uint()
        prot = self.unpack_uint()
        port = self.unpack_uint()
        return prog, vers, prot, port

    def unpack_pmaplist(self):
        return self.unpack_list(self.unpack_mapping)

    def unpack_call_result(self):
        port = self.unpack_uint()
        res = self.unpack_opaque()
        return port, res


class PartialPortMapperClient:
    __pychecker__ = 'no-classattr'
    def addpackers(self):
        self.packer = PortMapperPacker()
        self.unpacker = PortMapperUnpacker('')

    def Set(self, mapping):
        return self.make_call(PMAPPROC_SET, mapping, \
                self.packer.pack_mapping, \
                self.unpacker.unpack_uint)

    def Unset(self, mapping):
        return self.make_call(PMAPPROC_UNSET, mapping, \
                self.packer.pack_mapping, \
                self.unpacker.unpack_uint)

    def Getport(self, mapping):
        return self.make_call(PMAPPROC_GETPORT, mapping, \
                self.packer.pack_mapping, \
                self.unpacker.unpack_uint)

    def Dump(self):
        return self.make_call(PMAPPROC_DUMP, None, \
                None, \
                self.unpacker.unpack_pmaplist)

    def Callit(self, ca):
        return self.make_call(PMAPPROC_CALLIT, ca, \
                self.packer.pack_call_args, \
                self.unpacker.unpack_call_result)


class TCPPortMapperClient(PartialPortMapperClient, RawTCPClient):

    def __init__(self, host):
        RawTCPClient.__init__(self, \
                host, PMAP_PROG, PMAP_VERS, PMAP_PORT)


class UDPPortMapperClient(PartialPortMapperClient, RawUDPClient):

    def __init__(self, host):
        RawUDPClient.__init__(self, \
                host, PMAP_PROG, PMAP_VERS, PMAP_PORT)


class BroadcastUDPPortMapperClient(PartialPortMapperClient, \
                                   RawBroadcastUDPClient):

    def __init__(self, bcastaddr):
        RawBroadcastUDPClient.__init__(self, \
                bcastaddr, PMAP_PROG, PMAP_VERS, PMAP_PORT)


# Generic clients that find their server through the Port mapper

class TCPClient(RawTCPClient):

    def __init__(self, host, prog, vers):
        pmap = TCPPortMapperClient(host)
        port = pmap.Getport((prog, vers, IPPROTO_TCP, 0))
        pmap.close()
        if port == 0:
            raise PortMapError("program not registered")
        RawTCPClient.__init__(self, host, prog, vers, port)


class UDPClient(RawUDPClient):

    def __init__(self, host, prog, vers):
        pmap = UDPPortMapperClient(host)
        port = pmap.Getport((prog, vers, IPPROTO_UDP, 0))
        pmap.close()
        if port == 0:
            raise PortMapError("program not registered")
        RawUDPClient.__init__(self, host, prog, vers, port)


class BroadcastUDPClient(Client):

    def __init__(self, bcastaddr, prog, vers):
        self.pmap = BroadcastUDPPortMapperClient(bcastaddr)
        self.pmap.set_reply_handler(self.my_reply_handler)
        self.prog = prog
        self.vers = vers
        self.user_reply_handler = None
        self.addpackers()

    def close(self):
        self.pmap.close()

    def set_reply_handler(self, reply_handler):
        self.user_reply_handler = reply_handler

    def set_timeout(self, timeout):
        self.pmap.set_timeout(timeout)

    def my_reply_handler(self, reply, fromaddr):
        port, res = reply
        self.unpacker.reset(res)
        result = self.unpack_func()
        try:
            self.unpacker.done()
        except xdrlib.Error:
            raise RPCUnextractedData()
        self.replies.append((result, fromaddr))
        if self.user_reply_handler is not None:
            self.user_reply_handler(result, fromaddr)

    def make_call(self, proc, args, pack_func, unpack_func):
        self.packer.reset()
        if pack_func:
            pack_func(args)
        if unpack_func is None:
            def dummy(): pass
            self.unpack_func = dummy
        else:
            self.unpack_func = unpack_func
        self.replies = []
        packed_args = self.packer.get_buffer()
        dummy_replies = self.pmap.Callit( \
                (self.prog, self.vers, proc, packed_args))
        return self.replies


# Server classes

# These are not symmetric to the Client classes
# XXX No attempt is made to provide authorization hooks yet

class Server:

    def __init__(self, host, prog, vers, port):
        self.host = host # Should normally be '' for default interface
        self.prog = prog
        self.vers = vers
        self.port = port # Should normally be 0 for random port
        self.sock = None
        self.prot = None
        self.makesocket() # Assigns to self.sock and self.prot
        self.bindsocket()
        self.host, self.port = self.sock.getsockname()
        self.addpackers()

    def register(self):
        mapping = self.prog, self.vers, self.prot, self.port
        p = TCPPortMapperClient(self.host)
        if not p.Set(mapping):
            raise PortMapError("register failed")

    def unregister(self):
        mapping = self.prog, self.vers, self.prot, self.port
        p = TCPPortMapperClient(self.host)
        if not p.Unset(mapping):
            raise PortMapError("unregister failed")

    def handle(self, call):
        # Don't use unpack_header but parse the header piecewise
        # XXX I have no idea if I am using the right error responses!
        self.unpacker.reset(call)
        self.packer.reset()
        xid = self.unpacker.unpack_uint()
        self.packer.pack_uint(xid)
        temp = self.unpacker.unpack_enum()
        if temp != CALL:
            return None # Not worthy of a reply
        self.packer.pack_uint(REPLY)
        temp = self.unpacker.unpack_uint()
        if temp != RPCVERSION:
            self.packer.pack_uint(MSG_DENIED)
            self.packer.pack_uint(RPC_MISMATCH)
            self.packer.pack_uint(RPCVERSION)
            self.packer.pack_uint(RPCVERSION)
            return self.packer.get_buffer()
        self.packer.pack_uint(MSG_ACCEPTED)
        self.packer.pack_auth((AUTH_NULL, make_auth_null()))
        prog = self.unpacker.unpack_uint()
        if prog != self.prog:
            self.packer.pack_uint(PROG_UNAVAIL)
            return self.packer.get_buffer()
        vers = self.unpacker.unpack_uint()
        if vers != self.vers:
            self.packer.pack_uint(PROG_MISMATCH)
            self.packer.pack_uint(self.vers)
            self.packer.pack_uint(self.vers)
            return self.packer.get_buffer()
        proc = self.unpacker.unpack_uint()
        methname = 'handle_' + repr(proc)
        try:
            meth = getattr(self, methname)
        except AttributeError:
            self.packer.pack_uint(PROC_UNAVAIL)
            return self.packer.get_buffer()
        self.recv_cred = self.unpacker.unpack_auth()
        self.recv_verf = self.unpacker.unpack_auth()
        try:
            meth() # Unpack args, call turn_around(), pack reply
        except (EOFError, RPCGarbageArgs):
            # Too few or too many arguments
            self.packer.reset()
            self.packer.pack_uint(xid)
            self.packer.pack_uint(REPLY)
            self.packer.pack_uint(MSG_ACCEPTED)
            self.packer.pack_auth((AUTH_NULL, make_auth_null()))
            self.packer.pack_uint(GARBAGE_ARGS)
        return self.packer.get_buffer()

    def turn_around(self):
        try:
            self.unpacker.done()
        except xdrlib.Error:
            raise RPCUnextractedData()
        
        self.packer.pack_uint(SUCCESS)

    def handle_0(self): # Handle NULL message
        self.turn_around()

    def makesocket(self):
        # This MUST be overridden
        raise RuntimeError("makesocket not defined")

    def bindsocket(self):
        # Override this to bind to a different port (e.g. reserved)
        self.sock.bind((self.host, self.port))

    def addpackers(self):
        # Override this to use derived classes from Packer/Unpacker
        self.packer = Packer()
        self.unpacker = Unpacker('')


class TCPServer(Server):

    def makesocket(self):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.prot = IPPROTO_TCP

    def loop(self):
        self.sock.listen(0)
        while 1:
            self.session(self.sock.accept())

    def session(self, connection):
        sock, (host, port) = connection
        while 1:
            try:
                call = recvrecord(sock)
            except EOFError:
                break
            except socket.error as msg:
                print('Socket error:', msg)
                break
            reply = self.handle(call)
            if reply is not None:
                sendrecord(sock, reply)

    def forkingloop(self):
        # Like loop but uses forksession()
        self.sock.listen(0)
        while 1:
            self.forksession(self.sock.accept())

    def forksession(self, connection):
        # Like session but forks off a subprocess
        # Wait for deceased children
        try:
            while 1:
                pid, sts = os.waitpid(0, 1)
        except os.error:
            pass
        pid = None
        try:
            pid = os.fork()
            if pid: # Parent
                connection[0].close()
                return
            # Child
            self.session(connection)
        finally:
            # Make sure we don't fall through in the parent
            if pid == 0:
                os._exit(0)


class UDPServer(Server):

    def makesocket(self):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.prot = IPPROTO_UDP

    def loop(self):
        while 1:
            self.session()

    def session(self):
        call, host_port = self.sock.recvfrom(8192)
        self.sender_port = host_port
        reply = self.handle(call)
        if reply != None:
            self.sock.sendto(reply, host_port)


# Simple test program -- dump local portmapper status

def test():
    pmap = UDPPortMapperClient('')
    list = pmap.Dump()
    list.sort()
    for prog, vers, prot, port in list:
        print(prog, vers, end=' ')
        if prot == IPPROTO_TCP: print('tcp', end=' ')
        elif prot == IPPROTO_UDP: print('udp', end=' ')
        else: print(prot, end=' ')
        print(port)


# Test program for broadcast operation -- dump everybody's portmapper status

def testbcast():
    import sys
    if sys.argv[1:]:
        bcastaddr = sys.argv[1]
    else:
        bcastaddr = '<broadcast>'
    def rh(reply, fromaddr):
        host, port = fromaddr
        print(host + '\t' + repr(reply))
    pmap = BroadcastUDPPortMapperClient(bcastaddr)
    pmap.set_reply_handler(rh)
    pmap.set_timeout(5)
    unused_replies = pmap.Getport((100002, 1, IPPROTO_UDP, 0))


# Test program for server, with corresponding client
# On machine A: python -c 'import rpc; rpc.testsvr()'
# On machine B: python -c 'import rpc; rpc.testclt()' A
# (A may be == B)

def testsvr():
    # Simple test class -- proc 1 doubles its string argument as reply
    class S(UDPServer):
        def handle_1(self):
            arg = self.unpacker.unpack_string()
            self.turn_around()
            print('RPC function 1 called, arg', repr(arg))
            self.packer.pack_string(arg + arg)
    #
    s = S('', 0x20000000, 1, 0)
    try:
        s.unregister()
    except PortMapError as e:
        print('RuntimeError:', e.args, '(ignored)')
    s.register()
    print('Service started...')
    try:
        s.loop()
    finally:
        s.unregister()
        print('Service interrupted.')


def testclt():
    import sys
    if sys.argv[1:]: host = sys.argv[1]
    else: host = ''
    # Client for above server
    class C(UDPClient):
        def call_1(self, arg):
            return self.make_call(1, arg, \
                    self.packer.pack_string, \
                    self.unpacker.unpack_string)
    c = C(host, 0x20000000, 1)
    print('Making call...')
    reply = c.call_1('hello, world, ')
    print('Call returned', repr(reply))


# Local variables:
# py-indent-offset: 4
# tab-width: 8
# End:
